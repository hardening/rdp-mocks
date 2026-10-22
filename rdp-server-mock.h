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

#pragma once

class RdpServerMock;

#include "clipboardHelper.h"
#include "commandChannel.h"
#include "outputChannel.h"

#include <freerdp/freerdp.h>
#include <freerdp/client.h>
#include <freerdp/peer.h>
#include <freerdp/listener.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/server/disp.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/codec/planar.h>
#include <freerdp/codec/rfx.h>
#include <freerdp/codec/progressive.h>
#include <freerdp/codec/h264.h>
#include <winpr/wtsapi.h>

#include <string>
#include <vector>

/** @brief a codec+transport combination usable in an "encoding" priority list. Every value names
 * exactly what will be sent and how: the EGFX_* ones go out as a graphics pipeline surface
 * command (RDPGFX_CODECID_*, via the "rdpgfx" dynamic virtual channel), the LEGACY_* ones go out
 * over the base RDP protocol instead -- LEGACY_RAW/LEGACY_PLANAR as a (bitmap-cache-style
 * compressed, for PLANAR) BITMAP_UPDATE, LEGACY_RFX as a SURFACE_BITS_COMMAND. There is no legacy
 * counterpart for progressive/clear/AVC420/AVC444 -- those codecs only ever existed as EGFX
 * surface commands in the real protocol. GFX_ENCODING_NONE is never user-selectable (see the
 * "encoding" command's name table) and only ever appears as RdpServerMock::currentEncoding_
 * itself, meaning nothing in the configured priority list is currently usable (or negotiation
 * hasn't reached the point where that's knowable yet) */
enum GraphicsEncoding {
	GFX_ENCODING_EGFX_RAW,
	GFX_ENCODING_EGFX_PLANAR,
	GFX_ENCODING_EGFX_RFX,
	GFX_ENCODING_EGFX_PROGRESSIVE,
	GFX_ENCODING_EGFX_CLEAR,
	GFX_ENCODING_EGFX_AVC420,
	GFX_ENCODING_EGFX_AVC444,
	GFX_ENCODING_LEGACY_RAW,
	GFX_ENCODING_LEGACY_PLANAR,
	GFX_ENCODING_LEGACY_RFX,
	GFX_ENCODING_NONE,
};

/** @brief one client-reported monitor's rect, at (left, top) already normalized to the 0,0-based
 * desktop coordinate space that gfx_update/graphics_update's own x/y arguments are expressed in
 * (see RdpServerMock::ensureMonitorLayout). surfaceId is reserved as the eventual EGFX surface id
 * for this monitor whether or not that surface actually ends up being created (see
 * ensureGfxSurfaces) -- legacy (non-EGFX) senders never create a surface at all, but still use
 * this same layout (and the same ids, purely as a stable per-monitor label) to keep every update
 * they send bound to a single monitor rather than crossing a boundary */
struct GfxMonitorSurface {
	UINT16 surfaceId;
	INT32 left;
	INT32 top;
	UINT32 width;
	UINT32 height;
};

/** @brief the portion of an image update that lands on one particular monitor's EGFX surface, in
 * that surface's own local coordinate space -- see RdpServerMock::splitImageOverMonitors. For the
 * legacy (non-EGFX) equivalent, kept in desktop coordinates instead since those mechanisms have no
 * per-monitor coordinate space of their own, see DesktopImageSlice/clipImageToMonitors. */
struct GfxImageSlice {
	UINT16 surfaceId;
	/* destination position/size within the target surface */
	UINT32 surfaceX;
	UINT32 surfaceY;
	UINT32 width;
	UINT32 height;
	/* matching offset within the source image buffer */
	UINT32 srcOffsetX;
	UINT32 srcOffsetY;
};

/** @brief the portion of an image update that lands within one monitor's bounds, kept in desktop
 * (not monitor-local) coordinates -- used by the legacy BITMAP_UPDATE/Surface Bits senders, which
 * unlike EGFX have no per-monitor surface/coordinate space to translate into, only monitor bounds
 * to clip against. See RdpServerMock::clipImageToMonitors. */
struct DesktopImageSlice {
	UINT32 destX;
	UINT32 destY;
	UINT32 width;
	UINT32 height;
	/* matching offset within the source image buffer */
	UINT32 srcOffsetX;
	UINT32 srcOffsetY;
};

/** @brief command channel implementation for rdp-server-mock */
class ServerCommandChannel : public CommandChannel {
public:
	ServerCommandChannel(int fd, RdpServerMock *mock);
	virtual ~ServerCommandChannel() = default;

	CommandChannel::TreatResult onCommand(const std::string &cmd, const std::string &args) override;

protected:
	RdpServerMock *mock_;
};

/** @brief clipboard of rdp-server-mock, sending through the cliprdr server channel context */
class ServerClipboard : public ClipboardHelper {
public:
	ServerClipboard(OutputChannel *output);

	void attach(CliprdrServerContext *context);
	void detach();

	/** sends the Clipboard Capabilities and Monitor Ready PDUs, once the channel is opened */
	UINT start();

protected:
	UINT sendFormatList(const CLIPRDR_FORMAT_LIST *formatList) override;
	UINT sendFormatListResponse(const CLIPRDR_FORMAT_LIST_RESPONSE *response) override;
	UINT sendFormatDataRequest(const CLIPRDR_FORMAT_DATA_REQUEST *request) override;
	UINT sendFormatDataResponse(const CLIPRDR_FORMAT_DATA_RESPONSE *response) override;
	UINT sendFileContentsRequest(const CLIPRDR_FILE_CONTENTS_REQUEST *request) override;
	UINT sendFileContentsResponse(const CLIPRDR_FILE_CONTENTS_RESPONSE *response) override;

	static UINT _client_capabilities(
		CliprdrServerContext *context, const CLIPRDR_CAPABILITIES *pdu);
	static UINT _client_format_list(CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST *pdu);
	static UINT _client_format_list_response(
		CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST_RESPONSE *pdu);
	static UINT _client_format_data_request(
		CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_REQUEST *pdu);
	static UINT _client_format_data_response(
		CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_RESPONSE *pdu);
	static UINT _client_file_contents_request(
		CliprdrServerContext *context, const CLIPRDR_FILE_CONTENTS_REQUEST *pdu);
	static UINT _client_file_contents_response(
		CliprdrServerContext *context, const CLIPRDR_FILE_CONTENTS_RESPONSE *pdu);

	CliprdrServerContext *context_;
};

/** @brief */
struct RdpServerMockContext {
	rdpContext context_;
	RdpServerMock *mock_;
};

/** @brief rdp-server-mock implementation */
class RdpServerMock {
	friend class ServerCommandChannel;

public:
	RdpServerMock(int inFd, OutputChannel *output);
	~RdpServerMock();

	int run();

protected:
	static BOOL _client_preconnect(freerdp *instance);
	static BOOL _client_postconnect(freerdp *instance);

	static BOOL _peer_accepted(freerdp_listener *instance, freerdp_peer *client);
	static BOOL _peer_post_connect(freerdp_peer *client);
	static BOOL _peer_activate(freerdp_peer *client);
	static BOOL _peer_reached_state(freerdp_peer *client, CONNECTION_STATE state);
	static BOOL _peer_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code);
	static BOOL _peer_mouse_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y);
	static UINT _disp_monitor_layout(
		DispServerContext *context, const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *pdu);
	static BOOL _disp_channel_id_assigned(DispServerContext *context, UINT32 channelId);
	static BOOL _dvc_creation_status(void *userdata, UINT32 channelId, INT32 creationStatus);
	static BOOL _gfx_channel_id_assigned(RdpgfxServerContext *context, UINT32 channelId);
	static UINT _gfx_caps_advertise(
		RdpgfxServerContext *context, const RDPGFX_CAPS_ADVERTISE_PDU *pdu);
	static UINT _gfx_frame_acknowledge(
		RdpgfxServerContext *context, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *pdu);
	void freeChannelContexts();
	static BOOL ensureMonitorLayout(RdpServerMock *mock);
	static BOOL ensureGfxSurfaces(RdpServerMock *mock);
	static void resolveCurrentEncoding(RdpServerMock *mock);
	static BOOL ensureH264Encoder(RdpServerMock *mock);
	static std::vector<GfxImageSlice> splitImageOverMonitors(
		RdpServerMock *mock, UINT32 x, UINT32 y, UINT32 width, UINT32 height);
	static std::vector<DesktopImageSlice> clipImageToMonitors(
		RdpServerMock *mock, UINT32 x, UINT32 y, UINT32 width, UINT32 height);
	static BOOL sendGfxSurfaceCommand(RdpServerMock *mock, RDPGFX_SURFACE_COMMAND &cmd);
	static BOOL trySendRaw(RdpServerMock *mock, UINT16 surfaceId, BYTE *pixels, UINT32 x, UINT32 y,
		UINT32 width, UINT32 height);
	static BOOL trySendPlanar(RdpServerMock *mock, UINT16 surfaceId, BYTE *pixels, UINT32 x,
		UINT32 y, UINT32 width, UINT32 height);
	static BOOL trySendRemoteFx(RdpServerMock *mock, UINT16 surfaceId, BYTE *pixels, UINT32 x,
		UINT32 y, UINT32 width, UINT32 height);
	static BOOL trySendProgressive(RdpServerMock *mock, UINT16 surfaceId, BYTE *pixels, UINT32 x,
		UINT32 y, UINT32 width, UINT32 height);
	static BOOL trySendAvc420(RdpServerMock *mock, UINT16 surfaceId, BYTE *pixels, UINT32 x,
		UINT32 y, UINT32 width, UINT32 height);
	static BOOL trySendAvc444(RdpServerMock *mock, UINT16 surfaceId, BYTE *pixels, UINT32 x,
		UINT32 y, UINT32 width, UINT32 height);
	static BOOL sendLegacyPlanarUpdate(RdpServerMock *mock, const std::string &path,
		const BYTE *pixels, UINT32 width, UINT32 height, UINT32 x, UINT32 y);
	static BOOL sendLegacyRfxUpdate(RdpServerMock *mock, const std::string &path,
		const BYTE *pixels, UINT32 width, UINT32 height, UINT32 x, UINT32 y);
	static BOOL sendLegacyUpdate(RdpServerMock *mock, const std::string &path, const BYTE *pixels,
		UINT32 width, UINT32 height, UINT32 x, UINT32 y);
	static BOOL sendGfxUpdate(RdpServerMock *mock, const std::string &path, UINT32 x, UINT32 y);
	static BOOL sendEgfxUpdate(RdpServerMock *mock, const std::string &path, const BYTE *pixels,
		UINT32 width, UINT32 height, UINT32 x, UINT32 y);
	static BOOL sendGraphicsUpdate(
		RdpServerMock *mock, const std::string &path, UINT32 x, UINT32 y, BOOL *noUsableEncoding);

protected:
	/** polls the command fd (if @p pollFd) and treats whatever is buffered, stopping doRun_ on
	 * error/close -- shared by run()'s main loop and the "listen" command's own accept wait,
	 * which needs to keep servicing commands (e.g. "quit") while it blocks for a peer */
	void pollAndTreatCommands(bool pollFd);

	int cmdFd_;
	OutputChannel *output_;
	bool monitorStates_;
	bool monitorKeyEvents_;
	bool monitorMouseEvents_;
	bool monitorResizeEvents_;
	bool monitorGfxFrameAcks_;
	bool monitorSecurity_;
	bool doRun_;
	ServerCommandChannel commandChannel_;

	rdpSettings *settings_;
	freerdp_peer *peer_;
	bool connectionEstablished_;
	UINT64 pollCmdChannelStartDate_;
	CONNECTION_STATE lastReachedState_;
	std::string pendingRedirectHost_;
	HANDLE vcm_;
	DispServerContext *disp_;
	bool dispOpened_;
	UINT32 dispChannelId_;
	RdpgfxServerContext *gfx_;
	bool gfxOpened_;
	UINT32 gfxChannelId_;
	bool gfxCapsReady_;
	CliprdrServerContext *cliprdr_;
	bool cliprdrOpened_;
	ServerClipboard clipboard_;
	bool gfxSurfacesCreated_;
	std::vector<GfxMonitorSurface> gfxMonitors_;
	UINT32 gfxFrameId_;
	bool gfxFrameOpen_;
	std::vector<GraphicsEncoding> encodingPriority_;
	/** the single encoding graphics_update actually uses, resolved from encodingPriority_ plus
	 * negotiation state by resolveCurrentEncoding() -- see that function's comment */
	GraphicsEncoding currentEncoding_;
	BITMAP_PLANAR_CONTEXT *planarEncoder_;
	RFX_CONTEXT *rfxEncoder_;
	PROGRESSIVE_CONTEXT *progressiveEncoder_;
	H264_CONTEXT *h264Encoder_;
	bool h264EncoderFailed_;
};
