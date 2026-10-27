/**
 * rdpmocks: mock RDP server
 *
 * Copyright 2026 David Fort <contact@hardening-consulting.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>
#include <algorithm>

#include <winpr/synch.h>
#include <winpr/sysinfo.h>
#include <winpr/path.h>

#include <freerdp/input.h>
#include <freerdp/codec/color.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <winpr/tools/makecert.h>

#include <Magick++.h>

#include "rdp-server-mock.h"
#include "mockOptions.h"
#include "securityProtocols.h"

#define TAG "rdp-server-mock"

/* raw (uncompressed) BITMAP_DATA rectangles are wire-limited to a UINT16 byte length, so a
 * loaded image is always split into tiles no larger than this before being sent */
#define BITMAP_UPDATE_TILE_SIZE 64

/* splits an image update placed at (x, y, width, height) in the 0,0-normalized desktop
 * coordinate space across every monitor surface it overlaps -- most updates land on a single
 * monitor and get back one slice, but one spanning a monitor boundary (or a single-image
 * "desktop" update covering the whole layout) is split into one slice per monitor it touches */
std::vector<GfxImageSlice> RdpServerMock::splitImageOverMonitors(
	RdpServerMock *mock, UINT32 x, UINT32 y, UINT32 width, UINT32 height) {
	std::vector<GfxImageSlice> slices;
	const INT64 imgLeft = x;
	const INT64 imgTop = y;
	const INT64 imgRight = (INT64)x + width;
	const INT64 imgBottom = (INT64)y + height;

	for (const GfxMonitorSurface &monitor : mock->gfxMonitors_) {
		const INT64 monLeft = monitor.left;
		const INT64 monTop = monitor.top;
		const INT64 monRight = (INT64)monitor.left + monitor.width;
		const INT64 monBottom = (INT64)monitor.top + monitor.height;

		const INT64 interLeft = std::max(imgLeft, monLeft);
		const INT64 interTop = std::max(imgTop, monTop);
		const INT64 interRight = std::min(imgRight, monRight);
		const INT64 interBottom = std::min(imgBottom, monBottom);
		if (interLeft >= interRight || interTop >= interBottom)
			continue;

		GfxImageSlice slice = {};
		slice.surfaceId = monitor.surfaceId;
		slice.surfaceX = (UINT32)(interLeft - monLeft);
		slice.surfaceY = (UINT32)(interTop - monTop);
		slice.width = (UINT32)(interRight - interLeft);
		slice.height = (UINT32)(interBottom - interTop);
		slice.srcOffsetX = (UINT32)(interLeft - imgLeft);
		slice.srcOffsetY = (UINT32)(interTop - imgTop);
		slices.push_back(slice);
	}
	return slices;
}

/* the legacy (non-EGFX) equivalent of splitImageOverMonitors above: clips an image update placed
 * at (x, y, width, height) in the same 0,0-normalized desktop coordinate space to every monitor
 * it overlaps, so the legacy BITMAP_UPDATE/Surface Bits senders never emit a single update that
 * crosses a monitor boundary -- but unlike EGFX (one surface per monitor, each with its own local
 * coordinate space to translate into), legacy has only one flat desktop, so each returned slice's
 * destination stays in that same desktop coordinate space, just clipped to one monitor's bounds */
std::vector<DesktopImageSlice> RdpServerMock::clipImageToMonitors(
	RdpServerMock *mock, UINT32 x, UINT32 y, UINT32 width, UINT32 height) {
	std::vector<DesktopImageSlice> slices;
	const INT64 imgLeft = x;
	const INT64 imgTop = y;
	const INT64 imgRight = (INT64)x + width;
	const INT64 imgBottom = (INT64)y + height;

	for (const GfxMonitorSurface &monitor : mock->gfxMonitors_) {
		const INT64 monLeft = monitor.left;
		const INT64 monTop = monitor.top;
		const INT64 monRight = (INT64)monitor.left + monitor.width;
		const INT64 monBottom = (INT64)monitor.top + monitor.height;

		const INT64 interLeft = std::max(imgLeft, monLeft);
		const INT64 interTop = std::max(imgTop, monTop);
		const INT64 interRight = std::min(imgRight, monRight);
		const INT64 interBottom = std::min(imgBottom, monBottom);
		if (interLeft >= interRight || interTop >= interBottom)
			continue;

		DesktopImageSlice slice = {};
		slice.destX = (UINT32)interLeft;
		slice.destY = (UINT32)interTop;
		slice.width = (UINT32)(interRight - interLeft);
		slice.height = (UINT32)(interBottom - interTop);
		slice.srcOffsetX = (UINT32)(interLeft - imgLeft);
		slice.srcOffsetY = (UINT32)(interTop - imgTop);
		slices.push_back(slice);
	}
	return slices;
}

/** @brief an image loaded and decoded once, shared by every *_update path below so none of them
 * has to run Magick++ itself -- BGRA/32bpp, top-down, tightly packed (stride == width * 4),
 * matching gdi_get_pixel_format(32) == PIXEL_FORMAT_BGRA32 */
struct LoadedImage {
	UINT32 width = 0;
	UINT32 height = 0;
	std::vector<BYTE> pixels;
};

static BOOL loadImagePixels(const std::string &path, LoadedImage &out) {
	Magick::Image image;
	try {
		image.read(path);
	} catch (Magick::Exception &e) {
		WLog_ERR(TAG, "unable to load image %s: %s", path.c_str(), e.what());
		return FALSE;
	}

	out.width = (UINT32)image.columns();
	out.height = (UINT32)image.rows();
	if (!out.width || !out.height) {
		WLog_ERR(TAG, "invalid image dimensions for %s", path.c_str());
		return FALSE;
	}

	out.pixels.resize((size_t)out.width * out.height * 4);
	try {
		image.write(0, 0, out.width, out.height, "BGRA", Magick::CharPixel, out.pixels.data());
	} catch (Magick::Exception &e) {
		WLog_ERR(TAG, "unable to extract pixels from %s: %s", path.c_str(), e.what());
		return FALSE;
	}
	return TRUE;
}

/* extracts the tightly packed (stride == width * 4) sub-buffer for one slice out of a full,
 * tightly packed BGRA/BGRX source image -- every encoder below (raw, planar, RemoteFX,
 * progressive, H264) is called once per monitor slice, so each needs its own contiguous buffer
 * rather than a strided view into the original image. Takes plain scalars rather than a
 * GfxImageSlice/DesktopImageSlice so it works for either -- both share the same
 * srcOffsetX/srcOffsetY/width/height shape, they only differ in how the destination is expressed
 * (monitor-local vs desktop coordinates), which is irrelevant here. */
static std::vector<BYTE> extractSlicePixels(const BYTE *pixels, UINT32 imageWidth,
	UINT32 srcOffsetX, UINT32 srcOffsetY, UINT32 sliceWidth, UINT32 sliceHeight) {
	std::vector<BYTE> sub((size_t)sliceWidth * sliceHeight * 4);
	for (UINT32 row = 0; row < sliceHeight; row++) {
		const BYTE *srcPtr = pixels + ((size_t)(srcOffsetY + row) * imageWidth + srcOffsetX) * 4;
		BYTE *dstPtr = sub.data() + (size_t)row * sliceWidth * 4;
		memcpy(dstPtr, srcPtr, (size_t)sliceWidth * 4);
	}
	return sub;
}

/* batches already-built tiles into as few BITMAP_UPDATE fast path PDUs as fit within the peer's
 * advertised MultifragMaxRequestSize and sends them (a single PDU can't exceed it, which for any
 * reasonably sized image easily overflows one PDU) -- shared by the raw and legacy planar
 * senders below, which differ only in how each tile's bitmapDataStream/bitmapLength/compressed
 * flag were produced */
static BOOL sendBitmapUpdateTiles(
	freerdp_peer *peer, const std::string &path, std::vector<BITMAP_DATA> &tiles) {
	const UINT32 ntiles = (UINT32)tiles.size();

	UINT32 maxRequestSize =
		freerdp_settings_get_uint32(peer->context->settings, FreeRDP_MultifragMaxRequestSize);
	if (!maxRequestSize)
		maxRequestSize = BITMAP_UPDATE_TILE_SIZE * BITMAP_UPDATE_TILE_SIZE * 4;
	/* leave headroom for the PDU/order headers around each tile's payload */
	const UINT32 budget = maxRequestSize > 4096 ? maxRequestSize - 4096 : maxRequestSize;

	UINT32 batchStart = 0;
	while (batchStart < ntiles) {
		UINT32 batchEnd = batchStart;
		UINT32 batchBytes = 0;
		while (batchEnd < ntiles) {
			UINT32 tileBytes = tiles[batchEnd].bitmapLength;
			if (batchEnd > batchStart && batchBytes + tileBytes > budget)
				break;
			batchBytes += tileBytes;
			batchEnd++;
		}

		BITMAP_UPDATE update = BITMAP_UPDATE();
		update.number = batchEnd - batchStart;
		update.rectangles = tiles.data() + batchStart;

		if (!peer->context->update->BitmapUpdate(peer->context, &update)) {
			WLog_ERR(TAG, "unable to send bitmap update for %s", path.c_str());
			return FALSE;
		}

		batchStart = batchEnd;
	}

	return TRUE;
}

/* the pixel-based core of the legacy raw BITMAP_UPDATE path -- shared by sendBitmapUpdate (the
 * "bitmap_update" command's own path-based entry point) and sendGraphicsUpdate (which already has
 * the image loaded once for whichever encoding was resolved, and shouldn't decode it twice) */
static BOOL sendBitmapUpdateFromPixels(freerdp_peer *peer, const std::string &path,
	const BYTE *pixels, UINT32 width, UINT32 height, UINT32 x, UINT32 y) {
	const UINT32 cols = (width + BITMAP_UPDATE_TILE_SIZE - 1) / BITMAP_UPDATE_TILE_SIZE;
	const UINT32 rows = (height + BITMAP_UPDATE_TILE_SIZE - 1) / BITMAP_UPDATE_TILE_SIZE;
	const UINT32 ntiles = cols * rows;

	std::vector<BITMAP_DATA> tiles(ntiles);
	std::vector<std::vector<BYTE>> tileBuffers(ntiles);

	UINT32 idx = 0;
	for (UINT32 ty = 0; ty < rows; ty++) {
		for (UINT32 tx = 0; tx < cols; tx++, idx++) {
			UINT32 tileLeft = tx * BITMAP_UPDATE_TILE_SIZE;
			UINT32 tileTop = ty * BITMAP_UPDATE_TILE_SIZE;
			UINT32 tileWidth = std::min<UINT32>(BITMAP_UPDATE_TILE_SIZE, width - tileLeft);
			UINT32 tileHeight = std::min<UINT32>(BITMAP_UPDATE_TILE_SIZE, height - tileTop);

			/* the raw bitmap wire format is bottom-up (like a BMP); clients apply a vertical
			 * flip while decoding uncompressed data, see gdi_Bitmap_Decompress */
			std::vector<BYTE> &buffer = tileBuffers[idx];
			buffer.resize((size_t)tileWidth * tileHeight * 4);
			for (UINT32 row = 0; row < tileHeight; row++) {
				UINT32 srcRow = tileTop + (tileHeight - 1 - row);
				const BYTE *srcPtr = pixels + ((size_t)srcRow * width + tileLeft) * 4;
				BYTE *dstPtr = buffer.data() + (size_t)row * tileWidth * 4;
				memcpy(dstPtr, srcPtr, (size_t)tileWidth * 4);
			}

			BITMAP_DATA &data = tiles[idx];
			data = BITMAP_DATA();
			data.destLeft = x + tileLeft;
			data.destTop = y + tileTop;
			data.destRight = data.destLeft + tileWidth - 1;
			data.destBottom = data.destTop + tileHeight - 1;
			data.width = tileWidth;
			data.height = tileHeight;
			data.bitsPerPixel = 32;
			data.compressed = FALSE;
			data.bitmapLength = (UINT32)buffer.size();
			data.bitmapDataStream = buffer.data();
		}
	}

	return sendBitmapUpdateTiles(peer, path, tiles);
}

static BOOL sendBitmapUpdate(freerdp_peer *peer, const std::string &path, UINT32 x, UINT32 y) {
	LoadedImage img;
	if (!loadImagePixels(path, img))
		return FALSE;
	return sendBitmapUpdateFromPixels(peer, path, img.pixels.data(), img.width, img.height, x, y);
}

/* resolves mock->gfxMonitors_ (the per-monitor layout every *_update path slices/clips against)
 * from the client's GCC-time monitor data, independent of whether EGFX exists for this connection
 * at all -- called directly by the legacy senders (which never touch RDPGFX), and indirectly by
 * ensureGfxSurfaces below (which additionally creates a real gfx surface for each entry) */
BOOL RdpServerMock::ensureMonitorLayout(RdpServerMock *mock) {
	if (!mock->gfxMonitors_.empty())
		return TRUE;

	rdpSettings *settings = mock->peer_->context->settings;

	/* the monitor layout the client announced during the GCC connect sequence (see
	 * gcc_read_client_monitor_data): by the time PostConnect fires (and so, transitively, this
	 * first "*_update" call) CONNECTION_STATE_CAPABILITIES_EXCHANGE_MONITOR_LAYOUT has already
	 * run and guarantees MonitorCount/MonitorDefArray are populated with at least one monitor,
	 * synthesizing a single one matching DesktopWidth/Height if the client didn't send any (see
	 * rdp_peer_handle_state_active in libfreerdp/core/peer.c) */
	const UINT32 monitorCount = freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);
	const rdpMonitor *monitors =
		(const rdpMonitor *)freerdp_settings_get_pointer(settings, FreeRDP_MonitorDefArray);
	if (!monitorCount || !monitors) {
		WLog_ERR(TAG, "no monitor layout available from the client's GCC data");
		return FALSE;
	}

	/* the client's monitor coordinates can be negative (e.g. a monitor placed to the left of or
	 * above the primary), but RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU's outputOriginX/Y are UINT32 and
	 * can't represent that -- so every monitor is normalized here to a 0,0-based desktop
	 * coordinate space (bounding box of the whole layout), the same convention
	 * _disp_monitor_layout already uses for the legacy disp channel's width/height computation.
	 * This is also the coordinate space every "*_update" command's own x/y arguments are
	 * expressed in, for both EGFX and legacy sends alike. */
	INT64 boundLeft = monitors[0].x;
	INT64 boundTop = monitors[0].y;
	for (UINT32 i = 1; i < monitorCount; i++) {
		boundLeft = std::min<INT64>(boundLeft, monitors[i].x);
		boundTop = std::min<INT64>(boundTop, monitors[i].y);
	}

	/* surface ids are simply 1-based monitor indices, reserved here as the id ensureGfxSurfaces
	 * will later use for the real gfx surface it creates for this same monitor, if/when one ever
	 * gets created -- legacy senders never look at this field at all */
	std::vector<GfxMonitorSurface> layout;
	layout.reserve(monitorCount);
	for (UINT32 i = 0; i < monitorCount; i++) {
		GfxMonitorSurface entry = {};
		entry.surfaceId = (UINT16)(i + 1);
		entry.left = (INT32)(monitors[i].x - boundLeft);
		entry.top = (INT32)(monitors[i].y - boundTop);
		entry.width = (UINT32)monitors[i].width;
		entry.height = (UINT32)monitors[i].height;
		layout.push_back(entry);
	}

	mock->gfxMonitors_ = std::move(layout);
	return TRUE;
}

BOOL RdpServerMock::ensureGfxSurfaces(RdpServerMock *mock) {
	if (mock->gfxSurfacesCreated_)
		return TRUE;

	if (!ensureMonitorLayout(mock))
		return FALSE;

	RdpgfxServerContext *gfx = mock->gfx_;
	rdpSettings *settings = mock->peer_->context->settings;
	const UINT32 monitorCount = freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);
	const rdpMonitor *monitors =
		(const rdpMonitor *)freerdp_settings_get_pointer(settings, FreeRDP_MonitorDefArray);

	/* unlike gfxMonitors_'s normalized coordinates, RESET_GRAPHICS_PDU's monitorDefArray must
	 * mirror the client's raw GCC monitor data (MS-RDPEGFX 2.2.2.2: "a validated version of the
	 * data received in the Client Monitor Data") -- mirrors how shadow_client.c's own
	 * shadow_client_rdpgfx_reset_graphic() passes its subsystem's raw monitor rects through
	 * unchanged. desktopWidth/Height (the bounding box) are recovered from gfxMonitors_ instead
	 * of recomputing them from the raw data again. */
	std::vector<MONITOR_DEF> monitorDefs(monitorCount);
	UINT32 desktopWidth = 0;
	UINT32 desktopHeight = 0;
	for (UINT32 i = 0; i < monitorCount; i++) {
		MONITOR_DEF &def = monitorDefs[i];
		def.left = monitors[i].x;
		def.top = monitors[i].y;
		def.right = monitors[i].x + monitors[i].width;
		def.bottom = monitors[i].y + monitors[i].height;
		def.flags = monitors[i].is_primary ? MONITOR_PRIMARY : 0;

		const GfxMonitorSurface &entry = mock->gfxMonitors_[i];
		desktopWidth = std::max<UINT32>(desktopWidth, (UINT32)entry.left + entry.width);
		desktopHeight = std::max<UINT32>(desktopHeight, (UINT32)entry.top + entry.height);
	}

	RDPGFX_RESET_GRAPHICS_PDU reset = {};
	reset.width = desktopWidth;
	reset.height = desktopHeight;
	reset.monitorCount = monitorCount;
	reset.monitorDefArray = monitorDefs.data();
	if (gfx->ResetGraphics(gfx, &reset) != CHANNEL_RC_OK) {
		WLog_ERR(TAG, "gfx ResetGraphics failed");
		return FALSE;
	}

	/* one surface per monitor, each sized to that monitor and mapped to its own normalized
	 * origin in output space, reusing the layout (including surface ids) ensureMonitorLayout
	 * already resolved above */
	for (const GfxMonitorSurface &entry : mock->gfxMonitors_) {
		RDPGFX_CREATE_SURFACE_PDU create = {};
		create.surfaceId = entry.surfaceId;
		create.width = (UINT16)entry.width;
		create.height = (UINT16)entry.height;
		create.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;
		if (gfx->CreateSurface(gfx, &create) != CHANNEL_RC_OK) {
			WLog_ERR(TAG, "gfx CreateSurface failed for surface %u", entry.surfaceId);
			return FALSE;
		}

		RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU map = {};
		map.surfaceId = entry.surfaceId;
		map.outputOriginX = (UINT32)entry.left;
		map.outputOriginY = (UINT32)entry.top;
		if (gfx->MapSurfaceToOutput(gfx, &map) != CHANNEL_RC_OK) {
			WLog_ERR(TAG, "gfx MapSurfaceToOutput failed for surface %u", entry.surfaceId);
			return FALSE;
		}
	}

	mock->gfxSurfacesCreated_ = true;
	return TRUE;
}

BOOL RdpServerMock::sendGfxSurfaceCommand(RdpServerMock *mock, RDPGFX_SURFACE_COMMAND &cmd) {
	RdpgfxServerContext *gfx = mock->gfx_;

	/* while an explicit frame is open (see the "egfx_frame_begin"/"egfx_frame_end" commands),
	 * surface commands must be sent standalone -- context->SurfaceFrameCommand would otherwise
	 * bundle its own nested STARTFRAME/ENDFRAME pair around this one command, which the protocol
	 * doesn't allow nested */
	if (mock->gfxFrameOpen_)
		return gfx->SurfaceCommand(gfx, &cmd) == CHANNEL_RC_OK;

	RDPGFX_START_FRAME_PDU start = {};
	start.frameId = ++mock->gfxFrameId_;
	start.timestamp = (UINT32)GetTickCount64();
	RDPGFX_END_FRAME_PDU end = {};
	end.frameId = start.frameId;

	return gfx->SurfaceFrameCommand(gfx, &cmd, &start, &end) == CHANNEL_RC_OK;
}

BOOL RdpServerMock::trySendRaw(RdpServerMock *mock, UINT16 surfaceId, BYTE *pixels, UINT32 x,
	UINT32 y, UINT32 width, UINT32 height) {
	RDPGFX_SURFACE_COMMAND cmd = {};
	cmd.surfaceId = surfaceId;
	cmd.codecId = RDPGFX_CODECID_UNCOMPRESSED;
	cmd.format = PIXEL_FORMAT_BGRX32;
	cmd.left = x;
	cmd.top = y;
	cmd.right = x + width;
	cmd.bottom = y + height;
	cmd.width = width;
	cmd.height = height;
	cmd.data = pixels;
	cmd.length = width * height * 4;
	return sendGfxSurfaceCommand(mock, cmd);
}

BOOL RdpServerMock::trySendPlanar(RdpServerMock *mock, UINT16 surfaceId, BYTE *pixels, UINT32 x,
	UINT32 y, UINT32 width, UINT32 height) {
	if (!mock->planarEncoder_) {
		mock->planarEncoder_ =
			freerdp_bitmap_planar_context_new(PLANAR_FORMAT_HEADER_RLE, width, height);
		if (!mock->planarEncoder_)
			return FALSE;
	}
	if (!freerdp_bitmap_planar_context_reset(mock->planarEncoder_, width, height))
		return FALSE;
	/* RDPGFX_CODECID_PLANAR payloads are always top-down, unlike sendLegacyPlanarUpdate's
	 * compressed BITMAP_UPDATE tiles below (bottom-up, like a BMP) -- confirmed against the only
	 * other server-side EGFX planar sender in FreeRDP (shadow_client_send_planar in
	 * server/shadow/shadow_client.c, which sets this the same way) and against the client-side
	 * decoder's expectations (gdi_SurfaceCommand_Planar calls freerdp_bitmap_decompress_planar
	 * with vFlip=FALSE, the opposite of gdi_Bitmap_Decompress's hardcoded vFlip=TRUE for the
	 * legacy BITMAP_UPDATE path -- see sendLegacyPlanarUpdate's own comment). Both directions
	 * share this same planarEncoder_ context, so this must be set on every call rather than
	 * relying on whatever the previous caller (EGFX or legacy) last left it as. */
	freerdp_planar_topdown_image(mock->planarEncoder_, TRUE);

	UINT32 dstSize = 0;
	BYTE *dst = freerdp_bitmap_compress_planar(mock->planarEncoder_, pixels, PIXEL_FORMAT_BGRX32,
		width, height, width * 4, nullptr, &dstSize);
	if (!dst)
		return FALSE;

	RDPGFX_SURFACE_COMMAND cmd = {};
	cmd.surfaceId = surfaceId;
	cmd.codecId = RDPGFX_CODECID_PLANAR;
	cmd.format = PIXEL_FORMAT_BGRX32;
	cmd.left = x;
	cmd.top = y;
	cmd.right = x + width;
	cmd.bottom = y + height;
	cmd.width = width;
	cmd.height = height;
	cmd.data = dst;
	cmd.length = dstSize;

	/* freerdp_bitmap_compress_planar(..., dstData=nullptr, ...) allocates and hands back an
	 * owned buffer -- unlike progressive/H264 below, this one is ours to free */
	BOOL ok = sendGfxSurfaceCommand(mock, cmd);
	free(dst);
	return ok;
}

BOOL RdpServerMock::trySendRemoteFx(RdpServerMock *mock, UINT16 surfaceId, BYTE *pixels, UINT32 x,
	UINT32 y, UINT32 width, UINT32 height) {
	if (!mock->rfxEncoder_) {
		mock->rfxEncoder_ = rfx_context_new(TRUE);
		if (!mock->rfxEncoder_)
			return FALSE;
		/* rfx_compose_message hard-requires the context's configured pixel format to exactly
		 * match the buffer passed in on every call -- it does not convert -- so this is set once
		 * at context-creation time rather than per-call */
		rfx_context_set_pixel_format(mock->rfxEncoder_, PIXEL_FORMAT_BGRX32);
	}
	if (!rfx_context_reset(mock->rfxEncoder_, width, height))
		return FALSE;

	wStream *s = Stream_New(nullptr, 1024);
	if (!s)
		return FALSE;

	RFX_RECT rect = {};
	rect.x = 0;
	rect.y = 0;
	rect.width = (UINT16)width;
	rect.height = (UINT16)height;

	if (!rfx_compose_message(mock->rfxEncoder_, s, &rect, 1, pixels, width, height, width * 4)) {
		Stream_Free(s, TRUE);
		return FALSE;
	}

	RDPGFX_SURFACE_COMMAND cmd = {};
	cmd.surfaceId = surfaceId;
	cmd.codecId = RDPGFX_CODECID_CAVIDEO;
	cmd.format = PIXEL_FORMAT_BGRX32;
	cmd.left = x;
	cmd.top = y;
	cmd.right = x + width;
	cmd.bottom = y + height;
	cmd.width = width;
	cmd.height = height;
	cmd.data = Stream_Buffer(s);
	cmd.length = (UINT32)Stream_GetPosition(s);

	BOOL ok = sendGfxSurfaceCommand(mock, cmd);
	Stream_Free(s, TRUE);
	return ok;
}

BOOL RdpServerMock::trySendProgressive(RdpServerMock *mock, UINT16 surfaceId, BYTE *pixels,
	UINT32 x, UINT32 y, UINT32 width, UINT32 height) {
	if (!mock->progressiveEncoder_) {
		mock->progressiveEncoder_ = progressive_context_new(TRUE);
		if (!mock->progressiveEncoder_)
			return FALSE;
	}
	if (!progressive_context_reset(mock->progressiveEncoder_))
		return FALSE;

	REGION16 region;
	region16_init(&region);
	RECTANGLE_16 rect = {};
	rect.left = 0;
	rect.top = 0;
	rect.right = (UINT16)width;
	rect.bottom = (UINT16)height;
	BOOL unioned = region16_union_rect(&region, &region, &rect);

	BYTE *dst = nullptr;
	UINT32 dstSize = 0;
	int rc = unioned ? progressive_compress(mock->progressiveEncoder_, pixels, width * height * 4,
						   PIXEL_FORMAT_BGRX32, width, height, width * 4, &region, &dst, &dstSize)
					 : -1;
	region16_uninit(&region);
	if (rc <= 0 || !dst)
		return FALSE;

	RDPGFX_SURFACE_COMMAND cmd = {};
	cmd.surfaceId = surfaceId;
	cmd.codecId = RDPGFX_CODECID_CAPROGRESSIVE;
	cmd.format = PIXEL_FORMAT_BGRX32;
	cmd.left = x;
	cmd.top = y;
	cmd.right = x + width;
	cmd.bottom = y + height;
	cmd.width = width;
	cmd.height = height;
	cmd.data = dst;
	cmd.length = dstSize;
	/* cmd.contextId left at 0 -- mirrors shadow_client_send_progressive, which never allocates a
	 * separate server-side encoding context id; dst is owned by the PROGRESSIVE_CONTEXT itself
	 * (valid until the next compress call or context free) and must not be freed here */

	return sendGfxSurfaceCommand(mock, cmd);
}

/* lazily creates the shared H264 encoder context (used for both AVC420 and AVC444 -- FreeRDP's
 * H264_CONTEXT covers both), remembering a definitive failure (e.g. no encoder registered in the
 * linked libavcodec) so neither this nor later callers keep retrying a doomed context creation.
 * Used only to answer "is H264 usable at all" -- by resolveCurrentEncoding (probed once at
 * negotiation time, see its own comment) and by trySendAvc420/trySendAvc444 before they each
 * build their own fresh, throwaway context for the actual encode (see their shared comment on
 * why: this cached one must never be used for a real compress call). */
BOOL RdpServerMock::ensureH264Encoder(RdpServerMock *mock) {
	if (mock->h264EncoderFailed_)
		return FALSE;
	if (mock->h264Encoder_)
		return TRUE;

	mock->h264Encoder_ = h264_context_new(TRUE);
	if (!mock->h264Encoder_) {
		/* graceful, matches shadow_encoder_init_h264's own failure handling */
		mock->h264EncoderFailed_ = true;
		return FALSE;
	}
	return TRUE;
}

/* avc420_compress/avc444_compress (libfreerdp/codec/h264.c) both do inter-frame change detection
 * internally, diffing against whatever the H264_CONTEXT last encoded and returning 0 (not an
 * error, just "nothing changed, nothing to send") when the requested region is pixel-identical to
 * the previous call -- exactly the shadow server's own intended use (skip re-sending an unchanged
 * screen region). This mock is a test tool, not a desktop-diffing server: a caller re-sending the
 * same image at the same position via two graphics_update calls in a row does still expect the
 * second one to visibly land, so a fresh, throwaway context is created for every single call
 * below (unlike every other codec in this file, which reuse one long-lived context) -- there's no
 * public API to reset just the internal diff/reference-frame state on its own. mock->h264Encoder_
 * (see ensureH264Encoder) is never used here; it exists purely to answer "would creating one even
 * work" cheaply and repeatedly for resolveCurrentEncoding. */
BOOL RdpServerMock::trySendAvc420(RdpServerMock *mock, UINT16 surfaceId, BYTE *pixels, UINT32 x,
	UINT32 y, UINT32 width, UINT32 height) {
	if (!ensureH264Encoder(mock))
		return FALSE;

	H264_CONTEXT *h264 = h264_context_new(TRUE);
	if (!h264)
		return FALSE;
	if (!h264_context_reset(h264, width, height)) {
		h264_context_free(h264);
		return FALSE;
	}

	RDPGFX_AVC420_BITMAP_STREAM avc420 = {};
	RECTANGLE_16 regionRect = {};
	regionRect.left = 0;
	regionRect.top = 0;
	regionRect.right = (UINT16)width;
	regionRect.bottom = (UINT16)height;

	INT32 rc = avc420_compress(h264, pixels, PIXEL_FORMAT_BGRX32, width * 4, width, height,
		&regionRect, &avc420.data, &avc420.length, &avc420.meta);
	if (rc <= 0) {
		free_h264_metablock(&avc420.meta);
		h264_context_free(h264);
		return FALSE;
	}

	RDPGFX_SURFACE_COMMAND cmd = {};
	cmd.surfaceId = surfaceId;
	cmd.codecId = RDPGFX_CODECID_AVC420;
	cmd.format = PIXEL_FORMAT_BGRX32;
	cmd.left = x;
	cmd.top = y;
	cmd.right = x + width;
	cmd.bottom = y + height;
	cmd.width = width;
	cmd.height = height;
	/* AVC420 payloads go through .extra (a RDPGFX_AVC420_BITMAP_STREAM*), not .data/.length --
	 * the channel layer packs the region metablock + bitstream itself, see
	 * rdpgfx_write_surface_command()'s AVC420 branch */
	cmd.extra = &avc420;

	BOOL ok = sendGfxSurfaceCommand(mock, cmd);
	/* mirrors shadow_client_send_avc420: only the metablock is freed here -- avc420.data isn't
	 * freed by shadow either, presumably owned/reused internally by the H264 context the same
	 * way the progressive codec's output buffer is (so it must outlive the context, hence freeing
	 * the context only after the surface command has actually been sent) */
	free_h264_metablock(&avc420.meta);
	h264_context_free(h264);
	return ok;
}

/* estimates the wire size of one RDPGFX_AVC420_BITMAP_STREAM (metadata + bitstream), needed to
 * fill in RDPGFX_AVC444_BITMAP_STREAM::cbAvc420EncodedBitstream1 below -- reimplements FreeRDP's
 * own rdpgfx_estimate_h264_avc420 (channels/rdpgfx/server/rdpgfx_main.c), which computes exactly
 * this but isn't exported past that translation unit */
static UINT32 estimateAvc420StreamSize(const RDPGFX_AVC420_BITMAP_STREAM &avc420) {
	/* H264 metadata (numRegionRects + 10 bytes per regionRect/quantQualityVal pair) + bitstream,
	 * see rdpgfx_write_h264_avc420 */
	return (UINT32)sizeof(UINT32) + 10u * avc420.meta.numRegionRects + avc420.length;
}

BOOL RdpServerMock::trySendAvc444(RdpServerMock *mock, UINT16 surfaceId, BYTE *pixels, UINT32 x,
	UINT32 y, UINT32 width, UINT32 height) {
	if (!ensureH264Encoder(mock))
		return FALSE;

	/* fresh, throwaway context, not mock->h264Encoder_ -- see trySendAvc420's comment just above,
	 * same reasoning applies here (avc444_compress does its own internal luma/chroma change
	 * detection against the context's last-encoded frame) */
	H264_CONTEXT *h264 = h264_context_new(TRUE);
	if (!h264)
		return FALSE;
	if (!h264_context_reset(h264, width, height)) {
		h264_context_free(h264);
		return FALSE;
	}

	RDPGFX_AVC444_BITMAP_STREAM avc444 = {};
	RECTANGLE_16 regionRect = {};
	regionRect.left = 0;
	regionRect.top = 0;
	regionRect.right = (UINT16)width;
	regionRect.bottom = (UINT16)height;

	/* version 1 (plain AVC444, RDPGFX_CODECID_AVC444) rather than 2 (AVC444v2) -- this mock only
	 * ever advertises/sends the older of the two, there's no separate "avc444v2" encoding choice */
	INT32 rc = avc444_compress(h264, pixels, PIXEL_FORMAT_BGRX32, width * 4, width, height, 1,
		&regionRect, &avc444.LC, &avc444.bitstream[0].data, &avc444.bitstream[0].length,
		&avc444.bitstream[1].data, &avc444.bitstream[1].length, &avc444.bitstream[0].meta,
		&avc444.bitstream[1].meta);
	if (rc <= 0) {
		free_h264_metablock(&avc444.bitstream[0].meta);
		free_h264_metablock(&avc444.bitstream[1].meta);
		h264_context_free(h264);
		return FALSE;
	}

	RDPGFX_SURFACE_COMMAND cmd = {};
	cmd.surfaceId = surfaceId;
	cmd.codecId = RDPGFX_CODECID_AVC444;
	cmd.format = PIXEL_FORMAT_BGRX32;
	cmd.left = x;
	cmd.top = y;
	cmd.right = x + width;
	cmd.bottom = y + height;
	cmd.width = width;
	cmd.height = height;
	avc444.cbAvc420EncodedBitstream1 = estimateAvc420StreamSize(avc444.bitstream[0]);
	/* AVC444 payloads go through .extra (a RDPGFX_AVC444_BITMAP_STREAM*), same reasoning as
	 * AVC420 above */
	cmd.extra = &avc444;

	BOOL ok = sendGfxSurfaceCommand(mock, cmd);
	/* mirrors shadow_client_send_avc444: only the metablocks are freed here, same reasoning as
	 * trySendAvc420 above (the bitstream data itself must outlive the context until after the
	 * surface command is actually sent, hence freeing the context only now) */
	free_h264_metablock(&avc444.bitstream[0].meta);
	free_h264_metablock(&avc444.bitstream[1].meta);
	h264_context_free(h264);
	return ok;
}

/* resolves currentEncoding_ from the "encoding" priority list and whatever is currently known
 * about this connection's negotiation state -- called at each point that state changes (whether
 * EGFX/legacy RemoteFX are available is known right after PostConnect, EGFX's own gfx caps are
 * known once _gfx_caps_advertise confirms them, and the priority list itself can change live via
 * the "encoding" command) so that sendGraphicsUpdate never has to rediscover which codec to use
 * on every single call. Walks the list in order and picks the first entry that's usable *right
 * now*; unlike the EGFX_* entries (which all need the same two things: gfx_ negotiated and its
 * own caps handshake completed), each LEGACY_* entry's usability is independent of EGFX and of
 * the others, since they're each a distinct, older wire mechanism with its own (already-completed
 * by the time PostConnect fires) capability negotiation -- see sendLegacyPlanarUpdate/
 * sendLegacyRfxUpdate's own comments for what each one actually needs. */
void RdpServerMock::resolveCurrentEncoding(RdpServerMock *mock) {
	for (GraphicsEncoding encoding : mock->encodingPriority_) {
		switch (encoding) {
		case GFX_ENCODING_LEGACY_RAW:
		case GFX_ENCODING_LEGACY_PLANAR:
			/* both ride the base BITMAP_UPDATE mechanism, which every RDP peer must support --
			 * no capability to check, always usable once there's a connection to send on at all
			 * (guaranteed by the connectionEstablished_ gate callers already apply before ever
			 * reaching sendGraphicsUpdate) */
			mock->currentEncoding_ = encoding;
			return;
		case GFX_ENCODING_LEGACY_RFX:
			/* needs the peer to have actually negotiated RemoteFX support during the base RDP
			 * capability exchange (completed well before PostConnect) -- see
			 * sendLegacyRfxUpdate's own comment for why this also requires FreeRDP_RemoteFxCodec
			 * to be enabled on our own settings_ template (see the constructor) in the first
			 * place, or the client never even gets offered it to negotiate */
			if (!mock->peer_ ||
				!freerdp_settings_get_bool(mock->peer_->context->settings, FreeRDP_RemoteFxCodec))
				continue;
			mock->currentEncoding_ = encoding;
			return;
		default:
			break;
		}

		/* every other entry is an EGFX surface command, which needs the graphics pipeline
		 * negotiated at all (not supported by the client, or disabled via "noegfx") and its own
		 * CAPSADVERTISE/CAPSCONFIRM handshake completed */
		if (!mock->gfx_)
			continue;
		if (!mock->gfxCapsReady_) {
			/* still waiting on that handshake -- callers already gate on gfxCapsReady_ before
			 * ever reaching sendGraphicsUpdate for a real send, this just keeps currentEncoding_
			 * well-defined (as "nothing resolved yet") until it completes and this function gets
			 * called again from _gfx_caps_advertise */
			mock->currentEncoding_ = GFX_ENCODING_NONE;
			return;
		}

		switch (encoding) {
		case GFX_ENCODING_EGFX_CLEAR:
			/* FreeRDP ships a public ClearCodec compressor (clear_compress()), but no client or
			 * server anywhere in the codebase ever exercises it with Compressor=TRUE (only
			 * decode is used in practice) -- there's no reference wire-format behavior to mirror,
			 * so this is always treated as unavailable rather than risk sending a codec real
			 * clients have never had to handle */
			continue;
		case GFX_ENCODING_EGFX_AVC420:
		case GFX_ENCODING_EGFX_AVC444:
			/* probed once here, right as the encoding is resolved, instead of lazily on first
			 * send -- ensureH264Encoder() remembers the result either way (h264EncoderFailed_),
			 * so this doesn't duplicate the context creation trySendAvc420/trySendAvc444 still do
			 * for the context itself */
			if (!ensureH264Encoder(mock))
				continue;
			break;
		default:
			break;
		}
		mock->currentEncoding_ = encoding;
		return;
	}

	/* nothing in the configured priority list is usable -- e.g. "encoding clear" alone, or
	 * "encoding avc420" on a host with no usable H264 backend */
	mock->currentEncoding_ = GFX_ENCODING_NONE;
}

BOOL RdpServerMock::sendGfxUpdate(
	RdpServerMock *mock, const std::string &path, UINT32 x, UINT32 y) {
	/* BGRA/32bpp top-down: unlike the legacy BITMAP_UPDATE path, gfx raw payloads need no
	 * bottom-up flip and no tiling -- the dynamic virtual channel transport fragments arbitrarily
	 * large payloads on its own */
	LoadedImage img;
	if (!loadImagePixels(path, img))
		return FALSE;

	if (!ensureGfxSurfaces(mock))
		return FALSE;

	std::vector<GfxImageSlice> slices = splitImageOverMonitors(mock, x, y, img.width, img.height);
	if (slices.empty()) {
		WLog_ERR(TAG, "update at %u,%u %ux%u for %s does not overlap any monitor", x, y, img.width,
			img.height, path.c_str());
		return FALSE;
	}

	for (const GfxImageSlice &slice : slices) {
		std::vector<BYTE> subPixels = extractSlicePixels(img.pixels.data(), img.width,
			slice.srcOffsetX, slice.srcOffsetY, slice.width, slice.height);
		if (!trySendRaw(mock, slice.surfaceId, subPixels.data(), slice.surfaceX, slice.surfaceY,
				slice.width, slice.height))
			return FALSE;
	}
	return TRUE;
}

BOOL RdpServerMock::sendLegacyPlanarUpdate(RdpServerMock *mock, const std::string &path,
	const BYTE *pixels, UINT32 width, UINT32 height, UINT32 x, UINT32 y) {
	if (!mock->planarEncoder_) {
		mock->planarEncoder_ =
			freerdp_bitmap_planar_context_new(PLANAR_FORMAT_HEADER_RLE, width, height);
		if (!mock->planarEncoder_)
			return FALSE;
	}
	if (!freerdp_bitmap_planar_context_reset(mock->planarEncoder_, width, height))
		return FALSE;
	/* unlike RDPGFX_CODECID_PLANAR (see trySendPlanar), a compressed (RDP6.0 planar codec)
	 * BITMAP_UPDATE is always bottom-up, like the legacy raw BITMAP_UPDATE path (sendBitmapUpdate)
	 * it sits alongside -- confirmed by the client-side decoder, which for this exact path
	 * (compressed, bpp>=32, codecId forced to RDP_CODEC_ID_NONE by gdi_bitmap_update) hardcodes
	 * vFlip=TRUE in gdi_Bitmap_Decompress, the opposite of the EGFX planar decoder's vFlip=FALSE.
	 * The only other in-tree server-side sender for this exact mechanism
	 * (server/shadow/shadow_client.c's legacy-BitmapUpdate tiling) agrees: it never calls
	 * freerdp_planar_topdown_image at all for this path, relying on the context's own bottom-up
	 * (FALSE) default -- set explicitly here since this shares planarEncoder_ with trySendPlanar
	 * (EGFX, topdown=TRUE), which would otherwise leak into this call. */
	freerdp_planar_topdown_image(mock->planarEncoder_, FALSE);

	const UINT32 cols = (width + BITMAP_UPDATE_TILE_SIZE - 1) / BITMAP_UPDATE_TILE_SIZE;
	const UINT32 rows = (height + BITMAP_UPDATE_TILE_SIZE - 1) / BITMAP_UPDATE_TILE_SIZE;
	const UINT32 ntiles = cols * rows;

	std::vector<BITMAP_DATA> tiles(ntiles);
	std::vector<BYTE *> tileBuffers(ntiles, nullptr);

	BOOL ok = TRUE;
	UINT32 idx = 0;
	for (UINT32 ty = 0; ty < rows && ok; ty++) {
		for (UINT32 tx = 0; tx < cols && ok; tx++, idx++) {
			UINT32 tileLeft = tx * BITMAP_UPDATE_TILE_SIZE;
			UINT32 tileTop = ty * BITMAP_UPDATE_TILE_SIZE;
			UINT32 tileWidth = std::min<UINT32>(BITMAP_UPDATE_TILE_SIZE, width - tileLeft);
			UINT32 tileHeight = std::min<UINT32>(BITMAP_UPDATE_TILE_SIZE, height - tileTop);

			/* unlike the raw path, no manual row copy is needed here: the codec reads directly
			 * out of the full image buffer via an offset pointer + the image's own stride, the
			 * same way shadow_client.c's reference implementation does */
			const BYTE *tileSrc = pixels + ((size_t)tileTop * width + tileLeft) * 4;
			UINT32 dstSize = 0;
			BYTE *dst = freerdp_bitmap_compress_planar(mock->planarEncoder_, tileSrc,
				PIXEL_FORMAT_BGRX32, tileWidth, tileHeight, width * 4, nullptr, &dstSize);
			if (!dst) {
				ok = FALSE;
				break;
			}
			tileBuffers[idx] = dst;

			BITMAP_DATA &data = tiles[idx];
			data = BITMAP_DATA();
			data.destLeft = x + tileLeft;
			data.destTop = y + tileTop;
			data.destRight = data.destLeft + tileWidth - 1;
			data.destBottom = data.destTop + tileHeight - 1;
			data.width = tileWidth;
			data.height = tileHeight;
			data.bitsPerPixel = 32;
			data.compressed = TRUE;
			data.bitmapLength = dstSize;
			data.bitmapDataStream = dst;
			/* TS_CD_HEADER fields: update_write_bitmap_data only auto-computes these when the
			 * peer negotiated NoBitmapCompressionHeader, so they're always filled in explicitly
			 * here rather than relying on that -- matches shadow_client.c's own legacy planar
			 * tiling exactly (cbCompFirstRowSize is always 0, "present for historical reasons") */
			data.cbCompFirstRowSize = 0;
			data.cbCompMainBodySize = dstSize;
			data.cbScanWidth = tileWidth * 4;
			data.cbUncompressedSize = (UINT32)tileWidth * tileHeight * 4;
		}
	}

	if (ok)
		ok = sendBitmapUpdateTiles(mock->peer_, path, tiles);

	for (BYTE *buffer : tileBuffers)
		free(buffer);

	return ok;
}

BOOL RdpServerMock::sendLegacyRfxUpdate(RdpServerMock *mock, const std::string &path,
	const BYTE *pixels, UINT32 width, UINT32 height, UINT32 x, UINT32 y) {
	if (!mock->rfxEncoder_) {
		mock->rfxEncoder_ = rfx_context_new(TRUE);
		if (!mock->rfxEncoder_)
			return FALSE;
		rfx_context_set_pixel_format(mock->rfxEncoder_, PIXEL_FORMAT_BGRX32);
	}
	if (!rfx_context_reset(mock->rfxEncoder_, width, height))
		return FALSE;

	wStream *s = Stream_New(nullptr, 1024);
	if (!s)
		return FALSE;

	RFX_RECT rect = {};
	rect.x = 0;
	rect.y = 0;
	rect.width = (UINT16)width;
	rect.height = (UINT16)height;

	if (!rfx_compose_message(mock->rfxEncoder_, s, &rect, 1, pixels, width, height, width * 4)) {
		Stream_Free(s, TRUE);
		return FALSE;
	}

	/* legacy RemoteFX doesn't go through a gfx surface command at all -- it's a base RDP protocol
	 * mechanism (predating EGFX), sent as a Set/Stream Surface Bits Command (MS-RDPBCGR 2.2.9.2.1)
	 * over the main connection, exactly like server/Sample/sfreerdp.c's own reference RemoteFX
	 * sender. Requires the peer to have negotiated RemoteFX support in the base Bitmap Codecs
	 * capability set (checked in resolveCurrentEncoding before this encoding is ever picked) --
	 * which in turn requires our own settings_ template to have advertised it in the first place
	 * (see the constructor), the same "AND with server default" pattern as
	 * SupportGraphicsPipeline/SupportMonitorLayoutPdu. */
	rdpSettings *settings = mock->peer_->context->settings;
	SURFACE_BITS_COMMAND cmd = {};
	cmd.bmp.codecID = (UINT16)freerdp_settings_get_uint32(settings, FreeRDP_RemoteFxCodecId);
	const UINT32 surfaceCommandsSupported =
		freerdp_settings_get_uint32(settings, FreeRDP_SurfaceCommandsSupported);
	cmd.cmdType = (surfaceCommandsSupported & SURFCMDS_STREAM_SURFACE_BITS)
		? CMDTYPE_STREAM_SURFACE_BITS
		: CMDTYPE_SET_SURFACE_BITS;
	cmd.destLeft = x;
	cmd.destTop = y;
	cmd.destRight = x + width;
	cmd.destBottom = y + height;
	cmd.bmp.bpp = 32;
	cmd.bmp.width = (UINT16)width;
	cmd.bmp.height = (UINT16)height;
	cmd.bmp.bitmapDataLength = (UINT32)Stream_GetPosition(s);
	cmd.bmp.bitmapData = Stream_Buffer(s);

	BOOL ok = mock->peer_->context->update->SurfaceBits(mock->peer_->context, &cmd);
	if (!ok)
		WLog_ERR(TAG, "unable to send legacy RFX surface bits update for %s", path.c_str());
	Stream_Free(s, TRUE);
	return ok;
}

/* the legacy (non-EGFX) side of sendGraphicsUpdate: resolves the monitor layout if needed, clips
 * the image to whichever monitors it overlaps (see clipImageToMonitors -- unlike EGFX, each piece
 * stays in desktop coordinates, there's no per-monitor surface to translate into), and sends every
 * piece with mock->currentEncoding_'s mechanism (one of the LEGACY_* values -- this is never
 * called for an EGFX_* one) */
BOOL RdpServerMock::sendLegacyUpdate(RdpServerMock *mock, const std::string &path,
	const BYTE *pixels, UINT32 width, UINT32 height, UINT32 x, UINT32 y) {
	if (!ensureMonitorLayout(mock))
		return FALSE;

	std::vector<DesktopImageSlice> slices = clipImageToMonitors(mock, x, y, width, height);
	if (slices.empty()) {
		WLog_ERR(TAG, "update at %u,%u %ux%u for %s does not overlap any monitor", x, y, width,
			height, path.c_str());
		return FALSE;
	}

	for (const DesktopImageSlice &slice : slices) {
		std::vector<BYTE> subPixels = extractSlicePixels(
			pixels, width, slice.srcOffsetX, slice.srcOffsetY, slice.width, slice.height);
		BOOL sent = FALSE;

		switch (mock->currentEncoding_) {
		case GFX_ENCODING_LEGACY_RAW:
			sent = sendBitmapUpdateFromPixels(mock->peer_, path, subPixels.data(), slice.width,
				slice.height, slice.destX, slice.destY);
			break;
		case GFX_ENCODING_LEGACY_PLANAR:
			sent = sendLegacyPlanarUpdate(
				mock, path, subPixels.data(), slice.width, slice.height, slice.destX, slice.destY);
			break;
		case GFX_ENCODING_LEGACY_RFX:
			sent = sendLegacyRfxUpdate(
				mock, path, subPixels.data(), slice.width, slice.height, slice.destX, slice.destY);
			break;
		case GFX_ENCODING_EGFX_RAW:
		case GFX_ENCODING_EGFX_PLANAR:
		case GFX_ENCODING_EGFX_RFX:
		case GFX_ENCODING_EGFX_PROGRESSIVE:
		case GFX_ENCODING_EGFX_CLEAR:
		case GFX_ENCODING_EGFX_AVC420:
		case GFX_ENCODING_EGFX_AVC444:
		case GFX_ENCODING_NONE:
			/* unreachable: sendLegacyUpdate is only ever called (from sendGraphicsUpdate below)
			 * for a LEGACY_* currentEncoding_ */
			break;
		}

		if (!sent) {
			WLog_ERR(TAG, "legacy send failed for the region at %u,%u for %s", slice.destX,
				slice.destY, path.c_str());
			return FALSE;
		}
	}

	return TRUE;
}

/* the EGFX side of sendGraphicsUpdate: builds the gfx surfaces if needed, splits the image across
 * whichever monitor surfaces it overlaps, and sends every slice with mock->currentEncoding_'s
 * codec (one of the EGFX_* values -- this is never called for a LEGACY_* one) */
BOOL RdpServerMock::sendEgfxUpdate(RdpServerMock *mock, const std::string &path, const BYTE *pixels,
	UINT32 width, UINT32 height, UINT32 x, UINT32 y) {
	if (!ensureGfxSurfaces(mock))
		return FALSE;

	std::vector<GfxImageSlice> slices = splitImageOverMonitors(mock, x, y, width, height);
	if (slices.empty()) {
		WLog_ERR(TAG, "update at %u,%u %ux%u for %s does not overlap any monitor", x, y, width,
			height, path.c_str());
		return FALSE;
	}

	/* every slice uses the same, already-resolved currentEncoding_ -- unlike the old per-call
	 * fallback loop, a failure to actually send one here is now a genuine error (the codec was
	 * already established as usable back when currentEncoding_ was resolved), not a sign that
	 * this codec itself is unusable, so it's not reported as *noUsableEncoding */
	for (const GfxImageSlice &slice : slices) {
		std::vector<BYTE> subPixels = extractSlicePixels(
			pixels, width, slice.srcOffsetX, slice.srcOffsetY, slice.width, slice.height);
		BOOL sent = FALSE;

		switch (mock->currentEncoding_) {
		case GFX_ENCODING_EGFX_RAW:
			sent = trySendRaw(mock, slice.surfaceId, subPixels.data(), slice.surfaceX,
				slice.surfaceY, slice.width, slice.height);
			break;
		case GFX_ENCODING_EGFX_PLANAR:
			sent = trySendPlanar(mock, slice.surfaceId, subPixels.data(), slice.surfaceX,
				slice.surfaceY, slice.width, slice.height);
			break;
		case GFX_ENCODING_EGFX_RFX:
			sent = trySendRemoteFx(mock, slice.surfaceId, subPixels.data(), slice.surfaceX,
				slice.surfaceY, slice.width, slice.height);
			break;
		case GFX_ENCODING_EGFX_PROGRESSIVE:
			sent = trySendProgressive(mock, slice.surfaceId, subPixels.data(), slice.surfaceX,
				slice.surfaceY, slice.width, slice.height);
			break;
		case GFX_ENCODING_EGFX_AVC420:
			sent = trySendAvc420(mock, slice.surfaceId, subPixels.data(), slice.surfaceX,
				slice.surfaceY, slice.width, slice.height);
			break;
		case GFX_ENCODING_EGFX_AVC444:
			sent = trySendAvc444(mock, slice.surfaceId, subPixels.data(), slice.surfaceX,
				slice.surfaceY, slice.width, slice.height);
			break;
		case GFX_ENCODING_EGFX_CLEAR:
		case GFX_ENCODING_LEGACY_RAW:
		case GFX_ENCODING_LEGACY_PLANAR:
		case GFX_ENCODING_LEGACY_RFX:
		case GFX_ENCODING_NONE:
			/* unreachable: resolveCurrentEncoding() never resolves to CLEAR, and sendEgfxUpdate
			 * is only ever called (from sendGraphicsUpdate below) for an EGFX_* currentEncoding_ */
			break;
		}

		if (!sent) {
			WLog_ERR(TAG, "gfx send failed on surface %u for %s", slice.surfaceId, path.c_str());
			return FALSE;
		}
	}

	return TRUE;
}

BOOL RdpServerMock::sendGraphicsUpdate(
	RdpServerMock *mock, const std::string &path, UINT32 x, UINT32 y, BOOL *noUsableEncoding) {
	*noUsableEncoding = FALSE;

	/* currentEncoding_ is resolved ahead of time by resolveCurrentEncoding(), as soon as the
	 * relevant negotiation step makes it knowable (see that function's own comment) -- nothing
	 * here needs to probe or fall back between codecs anymore */
	if (mock->currentEncoding_ == GFX_ENCODING_NONE) {
		WLog_ERR(TAG, "no usable encoding among the configured priority list for %s", path.c_str());
		*noUsableEncoding = TRUE;
		return FALSE;
	}

	/* loaded once up front, regardless of which encoding was resolved -- every "send" function
	 * below just takes the already-decoded image */
	LoadedImage img;
	if (!loadImagePixels(path, img))
		return FALSE;

	switch (mock->currentEncoding_) {
	case GFX_ENCODING_LEGACY_RAW:
	case GFX_ENCODING_LEGACY_PLANAR:
	case GFX_ENCODING_LEGACY_RFX:
		return sendLegacyUpdate(mock, path, img.pixels.data(), img.width, img.height, x, y);
	case GFX_ENCODING_EGFX_RAW:
	case GFX_ENCODING_EGFX_PLANAR:
	case GFX_ENCODING_EGFX_RFX:
	case GFX_ENCODING_EGFX_PROGRESSIVE:
	case GFX_ENCODING_EGFX_AVC420:
	case GFX_ENCODING_EGFX_AVC444:
		return sendEgfxUpdate(mock, path, img.pixels.data(), img.width, img.height, x, y);
	case GFX_ENCODING_EGFX_CLEAR:
	case GFX_ENCODING_NONE:
		/* unreachable: resolveCurrentEncoding() never resolves to CLEAR, and NONE was already
		 * handled above */
		break;
	}

	return FALSE;
}

static BOOL checkFileExists(const std::string &path) {
	if (!winpr_PathFileExists(path.c_str())) {
		WLog_ERR(TAG, "file does not exist: %s", path.c_str());
		return FALSE;
	}
	return TRUE;
}

static BOOL loadCertificateFile(rdpSettings *settings, const char *path) {
	rdpCertificate *cert = freerdp_certificate_new_from_file(path);
	if (!cert) {
		WLog_ERR(TAG, "unable to load certificate %s", path);
		return FALSE;
	}
	return freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1);
}

static BOOL loadPrivateKeyFile(rdpSettings *settings, const char *path) {
	rdpPrivateKey *key = freerdp_key_new_from_file(path);
	if (!key) {
		WLog_ERR(TAG, "unable to load private key %s", path);
		return FALSE;
	}
	return freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1);
}

RdpServerMock::RdpServerMock(int fd, OutputChannel *output)
: cmdFd_(fd)
, output_(output)
, monitorStates_(false)
, monitorKeyEvents_(false)
, monitorMouseEvents_(false)
, monitorResizeEvents_(false)
, monitorGfxFrameAcks_(false)
, monitorSecurity_(false)
, monitorClientInfo_(false)
, doRun_(true)
, commandChannel_(fd, this)
, peer_(nullptr)
, connectionEstablished_(false)
, pollCmdChannelStartDate_(0)
, lastReachedState_((CONNECTION_STATE)-1)
, vcm_(nullptr)
, disp_(nullptr)
, dispOpened_(false)
, dispChannelId_(0)
, gfx_(nullptr)
, gfxOpened_(false)
, gfxChannelId_(0)
, gfxCapsReady_(false)
, cliprdr_(nullptr)
, cliprdrOpened_(false)
, clipboard_(output)
, gfxSurfacesCreated_(false)
, gfxFrameId_(0)
, gfxFrameOpen_(false)
/* falls back to GFX_ENCODING_LEGACY_RAW so a fresh connection "just works" (matches every RDP
 * peer, since it has no capability of its own to negotiate) even when the peer never negotiates
 * EGFX at all, without any hidden special-casing elsewhere -- resolveCurrentEncoding() just walks
 * this list like any other and finds GFX_ENCODING_EGFX_RAW unusable first, same as it would for
 * a user-configured list */
, encodingPriority_{ GFX_ENCODING_EGFX_RAW, GFX_ENCODING_LEGACY_RAW }
, currentEncoding_(GFX_ENCODING_NONE)
, planarEncoder_(nullptr)
, rfxEncoder_(nullptr)
, progressiveEncoder_(nullptr)
, h264Encoder_(nullptr)
, h264EncoderFailed_(false) {
	settings_ = freerdp_settings_new(FREERDP_SETTINGS_SERVER_MODE);

	/* off by default (see freerdp_settings_new/settings_get_default_bool) -- needed both to
	 * negotiate the client's own RNS_UD_CS_SUPPORT_MONITOR_LAYOUT_PDU early capability flag
	 * during the GCC exchange (see gcc.c's updateEarlyClientCaps, same AND-with-server-default
	 * pattern as FreeRDP_SupportGraphicsPipeline) and to get a synthesized single-monitor
	 * fallback in MonitorCount/MonitorDefArray (see rdp_peer_handle_state_active in
	 * libfreerdp/core/peer.c) when the client didn't send any Client Monitor Data of its own --
	 * ensureGfxSurfaces() below relies on one of those two always having populated it by the
	 * time PostConnect fires */
	freerdp_settings_set_bool(settings_, FreeRDP_SupportMonitorLayoutPdu, TRUE);

	/* also off by default -- needed for GFX_ENCODING_LEGACY_RFX (see sendLegacyRfxUpdate): the
	 * server only ever advertises RemoteFX in its own outgoing Bitmap Codecs capability set (and
	 * so only ever gets a chance to negotiate it with the client) when this is already true
	 * beforehand, the same AND-with-server-default pattern as SupportGraphicsPipeline/
	 * SupportMonitorLayoutPdu above (see rdp_write_bitmap_codecs_capability_set in
	 * libfreerdp/core/capabilities.c) */
	freerdp_settings_set_bool(settings_, FreeRDP_RemoteFxCodec, TRUE);
}

RdpServerMock::~RdpServerMock() {
	/* disp_, gfx_, cliprdr_ and vcm_ hold references into the peer's context (e.g. vcm->client),
	 * so they must be torn down before freerdp_peer_context_free()/freerdp_peer_free() below
	 * invalidate it */
	freeChannelContexts();
	/* encoder contexts are pure compression engines with no reference to the peer/vcm, so unlike
	 * disp_/gfx_/vcm_ above they're kept alive across reconnects and only freed here */
	if (planarEncoder_)
		freerdp_bitmap_planar_context_free(planarEncoder_);
	if (rfxEncoder_)
		rfx_context_free(rfxEncoder_);
	if (progressiveEncoder_)
		progressive_context_free(progressiveEncoder_);
	if (h264Encoder_)
		h264_context_free(h264Encoder_);
	if (peer_) {
		if (connectionEstablished_)
			peer_->Disconnect(peer_);
		freerdp_peer_context_free(peer_);
		freerdp_peer_free(peer_);
	}
	freerdp_settings_free(settings_);
	delete output_;
}


void RdpServerMock::freeChannelContexts() {
	if (disp_) {
		disp_server_context_free(disp_);
		disp_ = nullptr;
	}
	if (gfx_) {
		rdpgfx_server_context_free(gfx_);
		gfx_ = nullptr;
	}
	if (cliprdr_) {
		clipboard_.detach();
		if (cliprdrOpened_)
			cliprdr_->Close(cliprdr_);
		cliprdr_server_context_free(cliprdr_);
		cliprdr_ = nullptr;
		cliprdrOpened_ = false;
	}
	if (vcm_) {
		WTSCloseServer(vcm_);
		vcm_ = nullptr;
	}
}

ServerClipboard::ServerClipboard(OutputChannel *output)
: ClipboardHelper(output)
, context_(nullptr) {}

void ServerClipboard::attach(CliprdrServerContext *context) {
	context_ = context;
	context->custom = this;

	/* our own advertised capabilities, the channel lowers them to what the client also supports
	 * once its Clipboard Capabilities PDU is received */
	context->useLongFormatNames = (GENERAL_FLAGS & CB_USE_LONG_FORMAT_NAMES) != 0;
	context->streamFileClipEnabled = (GENERAL_FLAGS & CB_STREAM_FILECLIP_ENABLED) != 0;
	context->fileClipNoFilePaths = (GENERAL_FLAGS & CB_FILECLIP_NO_FILE_PATHS) != 0;
	context->canLockClipData = FALSE;
	context->hasHugeFileSupport = (GENERAL_FLAGS & CB_HUGE_FILE_SUPPORT_ENABLED) != 0;

	context->ClientCapabilities = _client_capabilities;
	context->ClientFormatList = _client_format_list;
	context->ClientFormatListResponse = _client_format_list_response;
	context->ClientFormatDataRequest = _client_format_data_request;
	context->ClientFormatDataResponse = _client_format_data_response;
	context->ClientFileContentsRequest = _client_file_contents_request;
	context->ClientFileContentsResponse = _client_file_contents_response;
}

void ServerClipboard::detach() {
	reset();
	context_ = nullptr;
}

UINT ServerClipboard::start() {
	CLIPRDR_GENERAL_CAPABILITY_SET generalCaps = {};
	generalCaps.capabilitySetType = CB_CAPSTYPE_GENERAL;
	generalCaps.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
	generalCaps.version = CB_CAPS_VERSION_2;
	generalCaps.generalFlags = GENERAL_FLAGS;

	CLIPRDR_CAPABILITIES caps = {};
	caps.common.msgType = CB_CLIP_CAPS;
	caps.common.dataLen = 4 + CB_CAPSTYPE_GENERAL_LEN;
	caps.cCapabilitiesSets = 1;
	caps.capabilitySets = (CLIPRDR_CAPABILITY_SET *)&generalCaps;

	UINT rc = context_->ServerCapabilities(context_, &caps);
	if (rc != CHANNEL_RC_OK)
		return rc;

	/* the client answers with its capabilities and initial Format List, which makes the
	 * channel ready (see ClipboardHelper::onPeerFormatList) */
	CLIPRDR_MONITOR_READY monitorReady = {};
	monitorReady.common.msgType = CB_MONITOR_READY;
	return context_->MonitorReady(context_, &monitorReady);
}

UINT ServerClipboard::sendFormatList(const CLIPRDR_FORMAT_LIST *formatList) {
	return context_ ? context_->ServerFormatList(context_, formatList) : ERROR_INVALID_HANDLE;
}

UINT ServerClipboard::sendFormatListResponse(const CLIPRDR_FORMAT_LIST_RESPONSE *response) {
	return context_ ? context_->ServerFormatListResponse(context_, response) : ERROR_INVALID_HANDLE;
}

UINT ServerClipboard::sendFormatDataRequest(const CLIPRDR_FORMAT_DATA_REQUEST *request) {
	return context_ ? context_->ServerFormatDataRequest(context_, request) : ERROR_INVALID_HANDLE;
}

UINT ServerClipboard::sendFormatDataResponse(const CLIPRDR_FORMAT_DATA_RESPONSE *response) {
	return context_ ? context_->ServerFormatDataResponse(context_, response) : ERROR_INVALID_HANDLE;
}

UINT ServerClipboard::sendFileContentsRequest(const CLIPRDR_FILE_CONTENTS_REQUEST *request) {
	return context_ ? context_->ServerFileContentsRequest(context_, request) : ERROR_INVALID_HANDLE;
}

UINT ServerClipboard::sendFileContentsResponse(const CLIPRDR_FILE_CONTENTS_RESPONSE *response) {
	return context_ ? context_->ServerFileContentsResponse(context_, response)
					: ERROR_INVALID_HANDLE;
}

UINT ServerClipboard::_client_capabilities(
	CliprdrServerContext *context, const CLIPRDR_CAPABILITIES *pdu) {
	return CHANNEL_RC_OK;
}

UINT ServerClipboard::_client_format_list(
	CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST *pdu) {
	return ((ServerClipboard *)context->custom)->onPeerFormatList(pdu);
}

UINT ServerClipboard::_client_format_list_response(
	CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST_RESPONSE *pdu) {
	return CHANNEL_RC_OK;
}

UINT ServerClipboard::_client_format_data_request(
	CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_REQUEST *pdu) {
	return ((ServerClipboard *)context->custom)->onPeerFormatDataRequest(pdu);
}

UINT ServerClipboard::_client_format_data_response(
	CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_RESPONSE *pdu) {
	return ((ServerClipboard *)context->custom)->onPeerFormatDataResponse(pdu);
}

UINT ServerClipboard::_client_file_contents_request(
	CliprdrServerContext *context, const CLIPRDR_FILE_CONTENTS_REQUEST *pdu) {
	return ((ServerClipboard *)context->custom)->onPeerFileContentsRequest(pdu);
}

UINT ServerClipboard::_client_file_contents_response(
	CliprdrServerContext *context, const CLIPRDR_FILE_CONTENTS_RESPONSE *pdu) {
	return ((ServerClipboard *)context->custom)->onPeerFileContentsResponse(pdu);
}

BOOL RdpServerMock::_client_preconnect(freerdp *instance) {
	if (!instance || !instance->context)
		return FALSE;

	//WLog_INFO(TAG, "preconnect");
	return TRUE;
}

BOOL RdpServerMock::_client_postconnect(freerdp *instance) {
	if (!instance || !instance->context)
		return FALSE;

	//RdpClientMockContext *context = (RdpClientMockContext*)instance->context;
	//context->mock_->connectionEstablished_ = true;

	//WLog_INFO(TAG, "postconnect");
	return TRUE;
}

BOOL RdpServerMock::_peer_accepted(freerdp_listener *instance, freerdp_peer *client) {
	RdpServerMock *mock = (RdpServerMock *)instance->info;
	if (!mock)
		return FALSE;

	client->ContextSize = sizeof(RdpServerMockContext);

	if (!freerdp_peer_context_new(client))
		return FALSE;

	((RdpServerMockContext *)client->context)->mock_ = mock;

	if (!freerdp_settings_copy(client->context->settings, mock->settings_)) {
		freerdp_peer_context_free(client);
		return FALSE;
	}

	client->PostConnect = _peer_post_connect;
	client->Activate = _peer_activate;
	client->ReachedState = _peer_reached_state;


	if (!client->Initialize(client)) {
		freerdp_peer_context_free(client);
		return FALSE;
	}

	rdpInput *input = client->context->input;
	input->KeyboardEvent = _peer_keyboard_event;
	input->MouseEvent = _peer_mouse_event;

	mock->vcm_ = WTSOpenServerA((LPSTR)client->context);
	if (!mock->vcm_ || mock->vcm_ == INVALID_HANDLE_VALUE) {
		WLog_ERR(TAG, "unable to open the virtual channel manager");
		mock->vcm_ = nullptr;
		freerdp_peer_context_free(client);
		return FALSE;
	}
	WTSVirtualChannelManagerSetDVCCreationCallback(mock->vcm_, _dvc_creation_status, mock);

	mock->peer_ = client;
	return TRUE;
}

BOOL RdpServerMock::_peer_post_connect(freerdp_peer *client) {
	RdpServerMockContext *context = (RdpServerMockContext *)client->context;
	RdpServerMock *mock = context->mock_;
	mock->connectionEstablished_ = true;

	if (mock->monitorSecurity_)
		mock->output_->sendNotification("security",
			securityProtocolName(
				freerdp_settings_get_uint32(client->context->settings, FreeRDP_SelectedProtocol)));

	if (mock->monitorClientInfo_) {
		struct ClientInfoField {
			const char *name;
			FreeRDP_Settings_Keys_String setting;
		};

		/* received in the client's Info PDU, one notification per field so that values can
		 * contain spaces */
		static const ClientInfoField clientInfoFields[] = {
			{ "user", FreeRDP_Username },
			{ "domain", FreeRDP_Domain },
			{ "alternateShell", FreeRDP_AlternateShell },
			{ "shellDirectory", FreeRDP_ShellWorkingDirectory },
		};

		for (const auto &field : clientInfoFields) {
			const char *value =
				freerdp_settings_get_string(client->context->settings, field.setting);
			mock->output_->sendNotification(
				"clientInfo", std::string(field.name) + "=" + (value ? value : ""));
		}
	}

	mock->disp_ = disp_server_context_new(mock->vcm_);
	if (!mock->disp_) {
		WLog_ERR(TAG, "unable to create the display control channel context");
	} else {
		mock->disp_->rdpcontext = client->context;
		mock->disp_->custom = mock;
		mock->disp_->DispMonitorLayout = _disp_monitor_layout;
		mock->disp_->ChannelIdAssigned = _disp_channel_id_assigned;
		/* disp_server_context_new() leaves these at 0; an advertised MaxNumMonitors of 0 makes
		 * the client clamp every monitor layout it sends down to 0 monitors (see
		 * disp_send_display_control_monitor_layout_pdu's NumMonitors clamp), so the caps we send
		 * out below need real limits */
		mock->disp_->MaxNumMonitors = 16;
		mock->disp_->MaxMonitorAreaFactorA = DISPLAY_CONTROL_MAX_MONITOR_WIDTH;
		mock->disp_->MaxMonitorAreaFactorB = DISPLAY_CONTROL_MAX_MONITOR_HEIGHT;
	}

	/* SupportGraphicsPipeline reflects the client's own negotiated support at this point (post
	 * GCC exchange), not just our own server-side default -- see freerdp_settings.c's
	 * SupportGraphicsPipeline default and gcc.c's updateEarlyClientCaps */
	if (freerdp_settings_get_bool(client->context->settings, FreeRDP_SupportGraphicsPipeline)) {
		mock->gfx_ = rdpgfx_server_context_new(mock->vcm_);
		if (!mock->gfx_) {
			WLog_ERR(TAG, "unable to create the graphics pipeline channel context");
		} else {
			mock->gfx_->rdpcontext = client->context;
			mock->gfx_->custom = mock;
			mock->gfx_->ChannelIdAssigned = _gfx_channel_id_assigned;
			mock->gfx_->CapsAdvertise = _gfx_caps_advertise;
			mock->gfx_->FrameAcknowledge = _gfx_frame_acknowledge;
			/* externalThread=TRUE: unlike disp_ (which the "disp" DVC library always drives from
			 * its own internal thread), rdpgfx supports being pumped from an external event loop
			 * instead -- we want it driven from run()'s own single-threaded
			 * WaitForMultipleObjectsEx loop, via rdpgfx_server_get_event_handle()/
			 * rdpgfx_server_handle_messages() there, rather than spawning a second thread */
			mock->gfx_->Initialize(mock->gfx_, TRUE);
		}
	} else {
		WLog_ERR(
			TAG, "peer did not negotiate the graphics pipeline, gfx_update will not be available");
	}

	/* the static channel itself is opened from run() once the client has joined it, see there */
	if (mock->clipboard_.isEnabled()) {
		mock->cliprdr_ = cliprdr_server_context_new(mock->vcm_);
		if (!mock->cliprdr_) {
			WLog_ERR(TAG, "unable to create the clipboard channel context");
		} else {
			mock->cliprdr_->rdpcontext = client->context;
			mock->clipboard_.attach(mock->cliprdr_);
		}
	}

	/* by this point every negotiation resolveCurrentEncoding() cares about except EGFX's own gfx
	 * caps handshake has already happened (whether EGFX exists at all, and the base RDP Bitmap
	 * Codecs capability exchange that GFX_ENCODING_LEGACY_RFX depends on) -- so this can already
	 * resolve to a LEGACY_* entry from the priority list if one comes before any EGFX_* entry, or
	 * else park at GFX_ENCODING_NONE until _gfx_caps_advertise below runs it again */
	resolveCurrentEncoding(mock);

	return TRUE;
}

BOOL RdpServerMock::_peer_activate(freerdp_peer *client) {
	RdpServerMockContext *context = (RdpServerMockContext *)client->context;
	RdpServerMock *mock = context->mock_;

	if (!mock->pendingRedirectHost_.empty()) {
		std::string host = mock->pendingRedirectHost_;
		mock->pendingRedirectHost_.clear();

		rdpRedirection *redirection = redirection_new();
		if (!redirection)
			return FALSE;

		BOOL ok = redirection_set_flags(redirection, LB_TARGET_NET_ADDRESS) &&
			redirection_set_string_option(redirection, LB_TARGET_NET_ADDRESS, host.c_str()) &&
			client->SendServerRedirection(client, redirection);

		redirection_free(redirection);

		if (!ok) {
			WLog_ERR(TAG, "unable to redirect client to %s", host.c_str());
			return FALSE;
		}
	}

	return TRUE;
}

BOOL RdpServerMock::_peer_reached_state(freerdp_peer *client, CONNECTION_STATE state) {
	RdpServerMockContext *context = (RdpServerMockContext *)client->context;
	RdpServerMock *mock = context->mock_;
	if (mock->monitorStates_ && state != mock->lastReachedState_) {
		mock->lastReachedState_ = state;
		mock->output_->sendNotification("states", freerdp_state_string(state));
	}
	return TRUE;
}

BOOL RdpServerMock::_peer_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code) {
	RdpServerMockContext *context = (RdpServerMockContext *)input->context;
	RdpServerMock *mock = context->mock_;
	if (mock->monitorKeyEvents_)
		mock->output_->sendNotification(
			"key", "flags=" + std::to_string(flags) + " code=" + std::to_string(code));
	return TRUE;
}

BOOL RdpServerMock::_peer_mouse_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y) {
	RdpServerMockContext *context = (RdpServerMockContext *)input->context;
	RdpServerMock *mock = context->mock_;
	if (mock->monitorMouseEvents_)
		mock->output_->sendNotification("mouse",
			"x=" + std::to_string(x) + " y=" + std::to_string(y) +
				" flags=" + std::to_string(flags));
	return TRUE;
}

/* mirrors the "landscape"/"portrait" vocabulary accepted by rdp-client-mock's "dyn-resolution"
 * command; the flipped variants can't be produced by that command but a real client under test
 * can still send them, so they're covered here too */
static std::string orientationToString(UINT32 orientation) {
	switch (orientation) {
	case ORIENTATION_LANDSCAPE:
		return "landscape";
	case ORIENTATION_PORTRAIT:
		return "portrait";
	case ORIENTATION_LANDSCAPE_FLIPPED:
		return "landscape_flipped";
	case ORIENTATION_PORTRAIT_FLIPPED:
		return "portrait_flipped";
	default:
		return std::to_string(orientation);
	}
}

UINT RdpServerMock::_disp_monitor_layout(
	DispServerContext *context, const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *pdu) {
	RdpServerMock *mock = (RdpServerMock *)context->custom;
	if (!pdu->NumMonitors)
		return CHANNEL_RC_OK;

	/* the framebuffer covers the bounding box of the whole monitor layout, not any single
	 * monitor -- e.g. two 1024x768 monitors side by side make a 2048x768 framebuffer */
	INT64 left = pdu->Monitors[0].Left;
	INT64 top = pdu->Monitors[0].Top;
	INT64 right = left + pdu->Monitors[0].Width;
	INT64 bottom = top + pdu->Monitors[0].Height;
	for (UINT32 i = 1; i < pdu->NumMonitors; i++) {
		const DISPLAY_CONTROL_MONITOR_LAYOUT &monitor = pdu->Monitors[i];
		left = std::min<INT64>(left, monitor.Left);
		top = std::min<INT64>(top, monitor.Top);
		right = std::max<INT64>(right, (INT64)monitor.Left + monitor.Width);
		bottom = std::max<INT64>(bottom, (INT64)monitor.Top + monitor.Height);
	}
	UINT32 width = (UINT32)(right - left);
	UINT32 height = (UINT32)(bottom - top);

	if (context->rdpcontext && context->rdpcontext->settings) {
		freerdp_settings_set_uint32(context->rdpcontext->settings, FreeRDP_DesktopWidth, width);
		freerdp_settings_set_uint32(context->rdpcontext->settings, FreeRDP_DesktopHeight, height);
	}

	if (mock->monitorResizeEvents_) {
		/* one "monitor" notification per monitor in the layout, sent before the aggregate
		 * "resize" notification below */
		for (UINT32 i = 0; i < pdu->NumMonitors; i++) {
			const DISPLAY_CONTROL_MONITOR_LAYOUT &monitor = pdu->Monitors[i];
			mock->output_->sendNotification("monitor",
				"width=" + std::to_string(monitor.Width) + " height=" +
					std::to_string(monitor.Height) + " left=" + std::to_string(monitor.Left) +
					" top=" + std::to_string(monitor.Top) + " primary=" +
					std::to_string((monitor.Flags & DISPLAY_CONTROL_MONITOR_PRIMARY) ? 1 : 0) +
					" orientation=" + orientationToString(monitor.Orientation));
		}

		mock->output_->sendNotification(
			"resize", "width=" + std::to_string(width) + " height=" + std::to_string(height));
	}
	return CHANNEL_RC_OK;
}

BOOL RdpServerMock::_disp_channel_id_assigned(DispServerContext *context, UINT32 channelId) {
	RdpServerMock *mock = (RdpServerMock *)context->custom;
	mock->dispChannelId_ = channelId;
	return TRUE;
}

BOOL RdpServerMock::_dvc_creation_status(void *userdata, UINT32 channelId, INT32 creationStatus) {
	RdpServerMock *mock = (RdpServerMock *)userdata;

	/* disp_->Open() only creates our local end of the "disp" DVC and hands back its channel id
	 * (see _disp_channel_id_assigned) -- the client hasn't actually accepted the channel yet at
	 * that point, so sending DisplayControlCaps right there races the client's own channel setup
	 * (and can lose that race under load). This fires once the client's create response for the
	 * channel actually arrives, which is the real "ready for traffic" signal; scoped by channelId
	 * so it stays correct if more server-opened DVCs are added later. */
	if (mock->disp_ && channelId == mock->dispChannelId_) {
		if (creationStatus >= 0)
			IFCALLRESULT(CHANNEL_RC_OK, mock->disp_->DisplayControlCaps, mock->disp_);
		else
			WLog_ERR(TAG, "disp channel creation failed with status %d", (int)creationStatus);
	}
	return TRUE;
}

BOOL RdpServerMock::_gfx_channel_id_assigned(RdpgfxServerContext *context, UINT32 channelId) {
	RdpServerMock *mock = (RdpServerMock *)context->custom;
	mock->gfxChannelId_ = channelId;
	return TRUE;
}

UINT RdpServerMock::_gfx_caps_advertise(
	RdpgfxServerContext *context, const RDPGFX_CAPS_ADVERTISE_PDU *pdu) {
	RdpServerMock *mock = (RdpServerMock *)context->custom;

	/* newest-to-oldest baseline versions understood here; no codec-specific flags are ever set
	 * since this implementation only ever sends RDPGFX_CODECID_UNCOMPRESSED, so none of the
	 * AVC/thinclient/smallcache capability flags need negotiating */
	static const UINT32 kSupportedVersions[] = {
		RDPGFX_CAPVERSION_107,
		RDPGFX_CAPVERSION_106,
		RDPGFX_CAPVERSION_105,
		RDPGFX_CAPVERSION_104,
		RDPGFX_CAPVERSION_103,
		RDPGFX_CAPVERSION_102,
		RDPGFX_CAPVERSION_101,
		RDPGFX_CAPVERSION_10,
		RDPGFX_CAPVERSION_81,
		RDPGFX_CAPVERSION_8,
	};

	for (UINT32 version : kSupportedVersions) {
		for (UINT16 i = 0; i < pdu->capsSetCount; i++) {
			if (pdu->capsSets[i].version != version)
				continue;

			RDPGFX_CAPSET capsSet = {};
			capsSet.version = version;
			capsSet.length = 4; /* just the flags DWORD, no version-specific payload */
			capsSet.flags = 0;

			RDPGFX_CAPS_CONFIRM_PDU confirm = {};
			confirm.capsSet = &capsSet;

			UINT rc = context->CapsConfirm(context, &confirm);
			mock->gfxCapsReady_ = (rc == CHANNEL_RC_OK);
			if (!mock->gfxCapsReady_)
				WLog_ERR(TAG, "gfx CapsConfirm failed with error %u", rc);
			/* second (and, for this mock, final) negotiation step relevant to the encoding
			 * choice: the gfx channel is now truly usable end-to-end, so this resolves
			 * currentEncoding_ to a real codec (or GFX_ENCODING_NONE if nothing in the
			 * configured priority list turns out usable, e.g. no H264 backend on this host) */
			resolveCurrentEncoding(mock);
			return rc;
		}
	}

	WLog_ERR(TAG, "client did not advertise a gfx caps version we support");
	return CHANNEL_RC_UNSUPPORTED_VERSION;
}

UINT RdpServerMock::_gfx_frame_acknowledge(
	RdpgfxServerContext *context, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *pdu) {
	RdpServerMock *mock = (RdpServerMock *)context->custom;
	if (mock->monitorGfxFrameAcks_)
		mock->output_->sendNotification("frameAck",
			"frameId=" + std::to_string(pdu->frameId) +
				" queueDepth=" + std::to_string(pdu->queueDepth) +
				" totalFramesDecoded=" + std::to_string(pdu->totalFramesDecoded));
	return CHANNEL_RC_OK;
}

void RdpServerMock::pollAndTreatCommands(bool pollFd) {
	if (pollFd) {
		switch (commandChannel_.pollFd()) {
		case CommandChannel::POLL_SUCCESS:
			break;
		case CommandChannel::POLL_ERROR:
		case CommandChannel::POLL_CLOSED:
			doRun_ = false;
			return;
		}
	}

	if (commandChannel_.hasBufferData()) {
		switch (commandChannel_.treat()) {
		case CommandChannel::POLL_SUCCESS:
			break;
		case CommandChannel::POLL_ERROR:
		case CommandChannel::POLL_CLOSED:
			doRun_ = false;
			break;
		}
	}
}

int RdpServerMock::run() {
	HANDLE hcmd = CreateFileDescriptorEventA(nullptr, FALSE, FALSE, cmdFd_, WINPR_FD_READ);
	if (!hcmd) {
		WLog_ERR(TAG, "unable to create command channel");
		return 2;
	}

	while (doRun_) {
		HANDLE handles[MAXIMUM_WAIT_OBJECTS];
		DWORD nhandles = 0;
		UINT64 now = GetTickCount64();
		bool pollCmd;
		DWORD pollDelay = INFINITE;

		if (now > pollCmdChannelStartDate_) {
			pollCmd = true;
			handles[nhandles++] = hcmd;
		} else {
			pollCmd = false;
			pollDelay = (1 + pollCmdChannelStartDate_ - now);
		}

		if (peer_)
			nhandles +=
				peer_->GetEventHandles(peer_, &handles[nhandles], MAXIMUM_WAIT_OBJECTS - nhandles);

		if (vcm_)
			handles[nhandles++] = WTSVirtualChannelManagerGetEventHandle(vcm_);

		if (gfx_ && gfxOpened_)
			handles[nhandles++] = rdpgfx_server_get_event_handle(gfx_);

		HANDLE cliprdrEvent =
			(cliprdr_ && cliprdrOpened_) ? cliprdr_->GetEventHandle(cliprdr_) : nullptr;
		if (cliprdrEvent)
			handles[nhandles++] = cliprdrEvent;

		DWORD status = WAIT_TIMEOUT;
		if (nhandles)
			status = WaitForMultipleObjectsEx(nhandles, handles, FALSE, pollDelay, TRUE);
		else
			SleepEx(pollDelay, TRUE);

		switch (status) {
		case WAIT_TIMEOUT:
			continue;
		case WAIT_FAILED:
			doRun_ = FALSE;
			continue;
		default:
			break;
		}

		if (pollCmd)
			pollAndTreatCommands(WaitForSingleObject(hcmd, 0) == WAIT_OBJECT_0);

		if (peer_ && !peer_->CheckFileDescriptor(peer_)) {
			peer_->Disconnect(peer_);
			/* they hold references into the peer's context (e.g. vcm->client), so they must be
			 * torn down before freerdp_peer_context_free()/freerdp_peer_free() below invalidate it */
			freeChannelContexts();
			cliprdrEvent = nullptr;
			dispOpened_ = false;
			dispChannelId_ = 0;
			gfxOpened_ = false;
			gfxChannelId_ = 0;
			gfxCapsReady_ = false;
			gfxSurfacesCreated_ = false;
			gfxMonitors_.clear();
			currentEncoding_ = GFX_ENCODING_NONE;
			gfxFrameId_ = 0;
			gfxFrameOpen_ = false;
			freerdp_peer_context_free(peer_);
			freerdp_peer_free(peer_);
			peer_ = nullptr;
			connectionEstablished_ = false;
			doRun_ = false;
		}

		/* WTSVirtualChannelManagerOpen() (called via WTSVirtualChannelManagerCheckFileDescriptor's
		 * autoOpen) attempts to open the "drdynvc" static channel and send its capabilities
		 * request exactly once: if that first attempt runs before "drdynvc" is actually joined at
		 * the MCS level, the send silently fails and the manager is stuck forever past
		 * DRDYNVC_STATE_NONE. Gating on peer_->activated ensures the channel join has completed
		 * (it happens as part of the connection sequence) before that first attempt is made. */
		if (peer_ && vcm_ && peer_->activated) {
			if (!WTSVirtualChannelManagerCheckFileDescriptor(vcm_)) {
				doRun_ = false;
			} else {
				bool dvcReady =
					WTSVirtualChannelManagerIsChannelJoined(vcm_, DRDYNVC_SVC_CHANNEL_NAME) &&
					WTSVirtualChannelManagerGetDrdynvcState(vcm_) == DRDYNVC_STATE_READY;

				/* Open() only creates our local end of the channel; DisplayControlCaps is sent
				 * from _dvc_creation_status() once the client's own create response for it
				 * actually arrives, not here -- see that function */
				if (dvcReady && disp_ && !dispOpened_ && disp_->Open(disp_) == CHANNEL_RC_OK)
					dispOpened_ = true;

				/* unlike disp_->Open() above, RdpgfxServerContext::Open() returns a BOOL, not a
				 * CHANNEL_RC_* code */
				if (dvcReady && gfx_ && !gfxOpened_ && gfx_->Open(gfx_))
					gfxOpened_ = true;

				/* unlike the DVCs above, cliprdr is a static channel: it only needs the client to
				 * have joined it (it doesn't when its own clipboard redirection is disabled) */
				if (cliprdr_ && !cliprdrOpened_ &&
					WTSVirtualChannelManagerIsChannelJoined(vcm_, CLIPRDR_SVC_CHANNEL_NAME) &&
					cliprdr_->Open(cliprdr_) == CHANNEL_RC_OK) {
					cliprdrOpened_ = true;
					if (clipboard_.start() != CHANNEL_RC_OK)
						WLog_ERR(TAG, "unable to start the clipboard channel");
				}
			}

			if (cliprdrEvent && WaitForSingleObject(cliprdrEvent, 0) == WAIT_OBJECT_0) {
				UINT rc = cliprdr_->CheckEventHandle(cliprdr_);
				if (rc != CHANNEL_RC_OK) {
					WLog_ERR(TAG, "clipboard channel failed with error %u", rc);
					doRun_ = false;
				}
			}

			if (gfx_ && gfxOpened_) {
				UINT rc = rdpgfx_server_handle_messages(gfx_);
				if (rc != CHANNEL_RC_OK && rc != ERROR_NO_DATA) {
					WLog_ERR(TAG, "rdpgfx_server_handle_messages failed with error %u", rc);
					doRun_ = false;
				}
			}
		}
	}

	if (peer_ && connectionEstablished_) {
		peer_->Disconnect(peer_);
		connectionEstablished_ = false;
	}

	return 0;
}


ServerCommandChannel::ServerCommandChannel(int fd, RdpServerMock *mock)
: CommandChannel(fd)
, mock_(mock) {}


CommandChannel::TreatResult ServerCommandChannel::onCommand(
	const std::string &cmd, const std::string &args) {
	struct StringSettingCommand {
		const char *cmd;
		FreeRDP_Settings_Keys_String settingId;
		BOOL (*check)(const std::string &args);
	};

	static const StringSettingCommand stringSettingCommands[] = {
		{ "samFile", FreeRDP_NtlmSamFile, checkFileExists },
	};

	rdpSettings *settings = mock_->settings_;

	if (mock_->clipboard_.handleCommand(cmd, args))
		return TREAT_SUCCESS;

	for (const auto &entry : stringSettingCommands) {
		if (cmd == entry.cmd) {
			if (entry.check && !entry.check(args))
				return TREAT_ERROR;
			return toTreatResult(
				freerdp_settings_set_string(settings, entry.settingId, args.c_str()));
		}
	}

	if (cmd == "authType") {
		return toTreatResult(setSecurityProtocols(settings, args));

#if 0
	} else if (cmd == "geometry") {
		size_t xPos = args.find('x');
		if (xPos == std::string::npos)
			return FALSE;
		std::string widthStr = args.substr(0, xPos);
		std::string heightStr = args.substr(xPos + 1);

		UINT32 width = (UINT32)strtoul(widthStr.c_str(), nullptr, 10);
		UINT32 height = (UINT32)strtoul(heightStr.c_str(), nullptr, 10);
#endif
	} else if (cmd == "noegfx") {
		/* clearing this on our own settings template (copied into every future peer's context in
		 * _peer_accepted) makes the graphics pipeline negotiate itself off through the normal GCC
		 * capability exchange, exactly as if the client hadn't advertised support for it -- see
		 * gcc.c's updateEarlyClientCaps -- rather than a separate mock-only bypass flag */
		return toTreatResult(
			freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, FALSE));

	} else if (cmd == "encoding") {
		struct EncodingName {
			const char *name;
			GraphicsEncoding value;
		};

		static const EncodingName kEncodingNames[] = {
			{ "raw", GFX_ENCODING_EGFX_RAW },
			{ "planar", GFX_ENCODING_EGFX_PLANAR },
			{ "remoteFx", GFX_ENCODING_EGFX_RFX },
			{ "progressive", GFX_ENCODING_EGFX_PROGRESSIVE },
			{ "clear", GFX_ENCODING_EGFX_CLEAR },
			{ "avc420", GFX_ENCODING_EGFX_AVC420 },
			{ "avc444", GFX_ENCODING_EGFX_AVC444 },
			{ "legacyRaw", GFX_ENCODING_LEGACY_RAW },
			{ "legacyPlanar", GFX_ENCODING_LEGACY_PLANAR },
			{ "legacyRfx", GFX_ENCODING_LEGACY_RFX },
		};

		/* args is a comma separated priority list, e.g. "remoteFx,planar,raw" */
		std::vector<GraphicsEncoding> priority;
		size_t start = 0;
		while (start <= args.size()) {
			size_t comma = args.find(',', start);
			std::string token =
				args.substr(start, comma == std::string::npos ? std::string::npos : comma - start);

			/* "h264" is a convenience alias, not a value of its own -- expands in place to
			 * "avc444,avc420" (prefer AVC444 over the plain AVC420, fall back to it) */
			if (token == "h264") {
				priority.push_back(GFX_ENCODING_EGFX_AVC444);
				priority.push_back(GFX_ENCODING_EGFX_AVC420);
			} else {
				bool matched = false;
				for (const auto &entry : kEncodingNames) {
					if (token == entry.name) {
						priority.push_back(entry.value);
						matched = true;
						break;
					}
				}

				if (!matched) {
					WLog_ERR(TAG, "unknown encoding: %s", token.c_str());
					return TREAT_ERROR;
				}
			}

			if (comma == std::string::npos)
				break;
			start = comma + 1;
		}

		if (priority.empty()) {
			WLog_ERR(TAG, "encoding needs at least one format");
			return TREAT_ERROR;
		}

		mock_->encodingPriority_ = std::move(priority);
		/* the priority list can change live, mid-connection -- re-resolve immediately so the
		 * very next graphics_update already reflects it instead of needing another negotiation
		 * round-trip to notice; harmless to call before any connection exists too (peer_/gfx_ are
		 * both null then, so every entry is skipped as not-yet-resolvable and this just leaves
		 * currentEncoding_ at GFX_ENCODING_NONE until a real connection resolves it for real via
		 * _peer_post_connect/_gfx_caps_advertise) */
		RdpServerMock::resolveCurrentEncoding(mock_);

	} else if (cmd == "cert") {
		return toTreatResult(loadCertificateFile(settings, args.c_str()));

	} else if (cmd == "key") {
		return toTreatResult(loadPrivateKeyFile(settings, args.c_str()));

	} else if (cmd == "autoCert") {
		char *makecertArgv[] = { (char *)"makecert", (char *)"-rdp", (char *)"-live",
			(char *)"-silent", (char *)"-y", (char *)"5" };

		MAKECERT_CONTEXT *makecert = makecert_context_new();
		if (!makecert)
			return TREAT_ERROR;

		BOOL ok = makecert_context_process(makecert, ARRAYSIZE(makecertArgv), makecertArgv) >= 0 &&
			makecert_context_set_output_file_name(makecert, "rdp-server-mock") == 1 &&
			makecert_context_output_certificate_file(makecert, args.c_str()) == 1 &&
			makecert_context_output_private_key_file(makecert, args.c_str()) == 1;
		makecert_context_free(makecert);

		if (!ok) {
			WLog_ERR(TAG, "unable to generate certificate in %s", args.c_str());
			return TREAT_ERROR;
		}

		return toTreatResult(
			loadCertificateFile(settings, (args + "/rdp-server-mock.crt").c_str()) &&
			loadPrivateKeyFile(settings, (args + "/rdp-server-mock.key").c_str()));

	} else if (cmd == "listen") {
		size_t colonPos = args.rfind(':');
		std::string host = (colonPos == std::string::npos) ? args : args.substr(0, colonPos);
		UINT32 port = 3389;
		if (colonPos != std::string::npos)
			port = (UINT32)strtoul(args.c_str() + colonPos + 1, nullptr, 10);
		if (host.empty())
			host = "127.0.0.1";

		freerdp_listener *listener = freerdp_listener_new();
		if (!listener) {
			mock_->output_->sendResult(false, "no mem");
			return TREAT_ERROR;
		}

		listener->info = mock_;
		listener->PeerAccepted = RdpServerMock::_peer_accepted;

		if (!listener->Open(listener, host.c_str(), (UINT16)port)) {
			mock_->output_->sendResult(false, "can't listen");
			WLog_ERR(TAG, "unable to listen on %s:%u", host.c_str(), port);
			freerdp_listener_free(listener);
			return TREAT_ERROR;
		}

		mock_->output_->sendResult(true, "listen");

		/* block here until a connection is accepted, but keep servicing the command channel
		 * concurrently (via the same hcmd event run() itself waits on): a peer that never shows
		 * up (client crashed, never started, or otherwise didn't connect) is not an error by
		 * itself, so without this a plain WaitForMultipleObjects(..., INFINITE) below would wedge
		 * this process forever -- unable to even react to "quit" -- instead of just failing this
		 * one command */
		HANDLE hcmd =
			CreateFileDescriptorEventA(nullptr, FALSE, FALSE, mock_->cmdFd_, WINPR_FD_READ);
		while (mock_->doRun_ && !mock_->peer_) {
			HANDLE handles[MAXIMUM_WAIT_OBJECTS];
			DWORD nhandles = 0;
			if (hcmd)
				handles[nhandles++] = hcmd;

			DWORD nlistener = listener->GetEventHandles(
				listener, &handles[nhandles], MAXIMUM_WAIT_OBJECTS - nhandles);
			if (!nlistener) {
				WLog_ERR(TAG, "unable to get listener event handles");
				mock_->output_->sendResult(false, "can't get handles");
				break;
			}
			nhandles += nlistener;

			DWORD status = WaitForMultipleObjects(nhandles, handles, FALSE, INFINITE);
			if (status == WAIT_FAILED)
				break;

			if (hcmd && status == WAIT_OBJECT_0) {
				mock_->pollAndTreatCommands(true);
				continue;
			}

			if (!listener->CheckFileDescriptor(listener)) {
				mock_->output_->sendResult(false, "error accepting peer");
				WLog_ERR(TAG, "listener CheckFileDescriptor failed");
				break;
			}
		}
		if (hcmd)
			CloseHandle(hcmd);

		BOOL accepted = (mock_->peer_ != nullptr);

		listener->Close(listener);
		freerdp_listener_free(listener);

		mock_->output_->sendResult(accepted, "accepted");
		if (!accepted)
			return TREAT_ERROR;

	} else if (cmd == "redirect") {
		if (args.empty()) {
			WLog_ERR(TAG, "redirect needs a target host");
			return TREAT_ERROR;
		}
		mock_->pendingRedirectHost_ = args;

	} else if (cmd == "bitmap_update") {
		/* like "dyn-resolution" on the client side, this is a runtime condition (the peer is
		 * accepted -- see "listen"'s RESULT:SUCCESS:accepted -- some time before this process's
		 * own event loop gets to run the rest of the connection sequence and flip
		 * connectionEstablished_, see _peer_post_connect), not a misuse of the command itself --
		 * report it as a RESULT:FAILURE that the caller can retry instead of TREAT_ERROR, which
		 * would tear down the whole server */
		if (!mock_->peer_ || !mock_->connectionEstablished_) {
			mock_->output_->sendResult(false, "not ready");
			return TREAT_SUCCESS;
		}

		std::istringstream iss(args);
		std::string path;
		UINT32 x = 0;
		UINT32 y = 0;
		if (!(iss >> path)) {
			WLog_ERR(TAG, "bitmap_update needs a file path");
			return TREAT_ERROR;
		}
		iss >> x >> y;

		bool sent = sendBitmapUpdate(mock_->peer_, path, x, y);
		mock_->output_->sendResult(sent, sent ? "" : "send failed");
		return toTreatResult(sent);

	} else if (cmd == "gfx_update") {
		/* like "bitmap_update" above, gfx caps negotiation completes asynchronously some time
		 * after the peer is accepted, so this is a retryable RESULT:FAILURE, not a TREAT_ERROR */
		if (!mock_->peer_ || !mock_->connectionEstablished_ || !mock_->gfx_ ||
			!mock_->gfxCapsReady_) {
			mock_->output_->sendResult(false, "not ready");
			return TREAT_SUCCESS;
		}

		std::istringstream iss(args);
		std::string path;
		UINT32 x = 0;
		UINT32 y = 0;
		if (!(iss >> path)) {
			WLog_ERR(TAG, "gfx_update needs a file path");
			return TREAT_ERROR;
		}
		iss >> x >> y;

		bool sent = RdpServerMock::sendGfxUpdate(mock_, path, x, y);
		mock_->output_->sendResult(sent, sent ? "" : "send failed");
		return toTreatResult(sent);

	} else if (cmd == "graphics_update") {
		if (!mock_->peer_ || !mock_->connectionEstablished_) {
			mock_->output_->sendResult(false, "not ready");
			return TREAT_SUCCESS;
		}
		/* if the graphics pipeline was negotiated for this connection (and not disabled via
		 * "noegfx"), its own caps handshake still needs to complete asynchronously -- same
		 * retryable "not ready" as gfx_update -- before we know whether to use it (and which of
		 * the configured encodings it can carry) or fall back to the legacy path. If it was never
		 * negotiated at all, mock_->gfx_ stays null permanently and sendGraphicsUpdate() goes
		 * straight to the legacy fallback below instead of waiting on this. */
		if (mock_->gfx_ && !mock_->gfxCapsReady_) {
			mock_->output_->sendResult(false, "not ready");
			return TREAT_SUCCESS;
		}

		std::istringstream iss(args);
		std::string path;
		UINT32 x = 0;
		UINT32 y = 0;
		if (!(iss >> path)) {
			WLog_ERR(TAG, "graphics_update needs a file path");
			return TREAT_ERROR;
		}
		iss >> x >> y;

		BOOL noUsableEncoding = FALSE;
		bool sent = RdpServerMock::sendGraphicsUpdate(mock_, path, x, y, &noUsableEncoding);
		if (sent) {
			mock_->output_->sendResult(true);
			return TREAT_SUCCESS;
		}
		if (noUsableEncoding) {
			/* nothing in the configured "encoding" priority list was actually usable for this
			 * connection -- a benign configuration mismatch (e.g. "encoding clear" alone), not a
			 * wire/protocol failure, so unlike a genuine send failure below this must not tear
			 * down the whole server: the caller can just retry with a different "encoding" list */
			mock_->output_->sendResult(false, "no usable encoding");
			return TREAT_SUCCESS;
		}
		mock_->output_->sendResult(false, "send failed");
		return TREAT_ERROR;

	} else if (cmd == "egfx_frame_begin") {
		if (!mock_->peer_ || !mock_->connectionEstablished_ || !mock_->gfx_ ||
			!mock_->gfxCapsReady_) {
			mock_->output_->sendResult(false, "not ready");
			return TREAT_SUCCESS;
		}
		if (mock_->gfxFrameOpen_) {
			mock_->output_->sendResult(false, "frame already open");
			return TREAT_SUCCESS;
		}

		RDPGFX_START_FRAME_PDU start = {};
		start.frameId = ++mock_->gfxFrameId_;
		start.timestamp = (UINT32)GetTickCount64();
		bool sent = mock_->gfx_->StartFrame(mock_->gfx_, &start) == CHANNEL_RC_OK;
		if (sent)
			mock_->gfxFrameOpen_ = true;
		mock_->output_->sendResult(sent, sent ? "" : "send failed");
		return toTreatResult(sent);

	} else if (cmd == "egfx_frame_end") {
		if (!mock_->peer_ || !mock_->connectionEstablished_ || !mock_->gfx_ ||
			!mock_->gfxCapsReady_) {
			mock_->output_->sendResult(false, "not ready");
			return TREAT_SUCCESS;
		}
		if (!mock_->gfxFrameOpen_) {
			mock_->output_->sendResult(false, "no frame open");
			return TREAT_SUCCESS;
		}

		RDPGFX_END_FRAME_PDU end = {};
		end.frameId = mock_->gfxFrameId_;
		bool sent = mock_->gfx_->EndFrame(mock_->gfx_, &end) == CHANNEL_RC_OK;
		mock_->gfxFrameOpen_ = false;
		mock_->output_->sendResult(sent, sent ? "" : "send failed");
		return toTreatResult(sent);

	} else if (cmd == "pause") {
		UINT32 delay = (UINT32)strtoul(args.c_str(), nullptr, 10);
		if (delay && delay < 200 * 1000)
			mock_->pollCmdChannelStartDate_ = GetTickCount64() + delay;
		return TREAT_SKIP;

	} else if (cmd == "clipboard") {
		/* decides whether the cliprdr channel is opened for the next accepted peer, see
		 * _peer_post_connect */
		if (mock_->connectionEstablished_) {
			mock_->output_->sendResult(false, "already connected");
			return TREAT_SUCCESS;
		}
		if (args != "on" && args != "off") {
			mock_->output_->sendResult(false, "invalid argument");
			return TREAT_SUCCESS;
		}
		mock_->clipboard_.setEnabled(args == "on");
		mock_->output_->sendResult(true);

	} else if (cmd == "debug") {
		mock_->output_->sendNotification("debug", args);

	} else if (cmd == "quit") {
		mock_->doRun_ = false;
		mock_->output_->sendResult(true);

	} else if (cmd == "monitor") {
		struct MonitoredItem {
			const char *item;
			bool *target;
		};

		static const MonitoredItem monitoredItems[] = {
			{ "states", &mock_->monitorStates_ },
			{ "keys", &mock_->monitorKeyEvents_ },
			{ "mouse", &mock_->monitorMouseEvents_ },
			{ "resize", &mock_->monitorResizeEvents_ },
			{ "gfxFrames", &mock_->monitorGfxFrameAcks_ },
			{ "security", &mock_->monitorSecurity_ },
			{ "clientInfo", &mock_->monitorClientInfo_ },
		};

		/* args is a comma separated list of kinds, e.g. "states,mouse" */
		size_t start = 0;
		while (start <= args.size()) {
			size_t comma = args.find(',', start);
			std::string token =
				args.substr(start, comma == std::string::npos ? std::string::npos : comma - start);

			bool matched = false;
			if (token == "clipboard") {
				mock_->clipboard_.setMonitor(true);
				matched = true;
			}
			for (const auto &entry : monitoredItems) {
				if (token == entry.item) {
					*entry.target = true;
					matched = true;
					break;
				}
			}

			if (!matched && token == "off") {
				for (const auto &entry : monitoredItems)
					*entry.target = false;
				mock_->clipboard_.setMonitor(false);
				matched = true;
			}

			if (!matched) {
				WLog_ERR(TAG, "unknown monitor kind: %s", token.c_str());
				return TREAT_ERROR;
			}

			if (comma == std::string::npos)
				break;
			start = comma + 1;
		}
	}

	return TREAT_SUCCESS;
}

int main(int argc, char *argv[]) {
	Magick::InitializeMagick(*argv);

	MockOptions options;
	int res = options.parseArgs(argc, argv);
	if (res)
		return res;

	if (!options.debugMode)
		WLog_SetLogLevel(WLog_GetRoot(), WLOG_OFF);

	if (!WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi())) {
		WLog_ERR(TAG, "unable to register the WtsApi function table");
		return 1;
	}

	RdpServerMock mock(options.cmdFd,
		options.jsonOutput ? new JsonOutputChannel(options.outFd)
						   : new OutputChannel(options.outFd));

	return mock.run();
}
