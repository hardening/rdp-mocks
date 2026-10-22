/**
 * rdpmocks: mock RDP client
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
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <vector>

#include <winpr/synch.h>
#include <winpr/sysinfo.h>

#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/gfx.h>

#include <Magick++.h>

#include "rdp-client-mock.h"
#include "mockOptions.h"
#include "securityProtocols.h"

#define TAG "rdp-client-mock"

#define DYN_RESOLUTION_MAX_MONITORS 16

/* parses one "<width>x<height>[:<originX>,<originY>[:landscape|portrait]]" monitor spec, as used by the
 * "dyn-resolution" command; Left/Top default to (0,0) and Orientation to landscape when omitted */
static bool parseMonitorSpec(const std::string &spec, DISPLAY_CONTROL_MONITOR_LAYOUT &layout) {
	layout = DISPLAY_CONTROL_MONITOR_LAYOUT{};

	size_t firstColon = spec.find(':');
	std::string sizePart = spec.substr(0, firstColon);
	std::string rest = (firstColon == std::string::npos) ? "" : spec.substr(firstColon + 1);

	size_t xPos = sizePart.find('x');
	if (xPos == std::string::npos)
		return false;
	UINT32 width = (UINT32)strtoul(sizePart.substr(0, xPos).c_str(), nullptr, 10);
	UINT32 height = (UINT32)strtoul(sizePart.c_str() + xPos + 1, nullptr, 10);
	if (!width || !height)
		return false;

	INT32 originX = 0;
	INT32 originY = 0;
	UINT32 orientation = ORIENTATION_LANDSCAPE;

	if (!rest.empty()) {
		size_t secondColon = rest.find(':');
		std::string posPart = rest.substr(0, secondColon);
		std::string orientPart =
			(secondColon == std::string::npos) ? "" : rest.substr(secondColon + 1);

		size_t commaPos = posPart.find(',');
		if (commaPos == std::string::npos)
			return false;
		originX = (INT32)strtol(posPart.substr(0, commaPos).c_str(), nullptr, 10);
		originY = (INT32)strtol(posPart.c_str() + commaPos + 1, nullptr, 10);

		if (!orientPart.empty()) {
			if (orientPart == "landscape")
				orientation = ORIENTATION_LANDSCAPE;
			else if (orientPart == "portrait")
				orientation = ORIENTATION_PORTRAIT;
			else
				return false;
		}
	}

	layout.Left = originX;
	layout.Top = originY;
	layout.Width = width;
	layout.Height = height;
	layout.Orientation = orientation;
	return true;
}

/* parses one "<width>x<height>:<originX>,<originY>" monitor spec, as used by the "screens"
 * command's ';'-separated list -- unlike parseMonitorSpec above (dyn-resolution's own PDU-time
 * layout, everything optional), position is mandatory here since these become the GCC Client
 * Monitor Data announced at connect time, not a later update relative to an existing layout */
static bool parseScreenSpec(const std::string &spec, rdpMonitor &monitor) {
	monitor = rdpMonitor{};

	size_t xPos = spec.find('x');
	size_t colonPos = spec.find(':', xPos == std::string::npos ? 0 : xPos);
	if (xPos == std::string::npos || colonPos == std::string::npos || colonPos < xPos)
		return false;

	UINT32 width = (UINT32)strtoul(spec.substr(0, xPos).c_str(), nullptr, 10);
	UINT32 height =
		(UINT32)strtoul(spec.substr(xPos + 1, colonPos - xPos - 1).c_str(), nullptr, 10);
	if (!width || !height)
		return false;

	std::string posPart = spec.substr(colonPos + 1);
	size_t commaPos = posPart.find(',');
	if (commaPos == std::string::npos)
		return false;
	INT32 x = (INT32)strtol(posPart.substr(0, commaPos).c_str(), nullptr, 10);
	INT32 y = (INT32)strtol(posPart.c_str() + commaPos + 1, nullptr, 10);

	monitor.x = x;
	monitor.y = y;
	monitor.width = (INT32)width;
	monitor.height = (INT32)height;
	monitor.attributes.physicalWidth = width;
	monitor.attributes.physicalHeight = height;
	monitor.attributes.orientation = ORIENTATION_LANDSCAPE;
	monitor.attributes.desktopScaleFactor = 100;
	monitor.attributes.deviceScaleFactor = 100;
	return true;
}

/* MS-RDPEDISP doesn't allow overlapping monitors in a layout update */
static bool monitorsOverlap(const std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT> &monitors) {
	for (size_t i = 0; i < monitors.size(); i++) {
		const DISPLAY_CONTROL_MONITOR_LAYOUT &a = monitors[i];
		INT64 aLeft = a.Left, aTop = a.Top;
		INT64 aRight = aLeft + a.Width, aBottom = aTop + a.Height;

		for (size_t j = i + 1; j < monitors.size(); j++) {
			const DISPLAY_CONTROL_MONITOR_LAYOUT &b = monitors[j];
			INT64 bLeft = b.Left, bTop = b.Top;
			INT64 bRight = bLeft + b.Width, bBottom = bTop + b.Height;

			if (aLeft < bRight && bLeft < aRight && aTop < bBottom && bTop < aBottom)
				return true;
		}
	}
	return false;
}

RdpClientMock::RdpClientMock(int fd, OutputChannel *output)
: cmdFd_(fd)
, output_(output)
, monitorStates_(false)
, monitorConnectionState_(false)
, monitorGraphicsUpdates_(true)
, monitorChannels_(false)
, monitorSecurity_(false)
, doRun_(true)
, commandChannel_(fd, this)
, connectionEstablished_(false)
, pollCmdChannelStartDate_(0)
, disp_(nullptr)
, gfx_(nullptr)
, clipboard_(output)
, screenshotsEnabled_(false)
, gdiBitmapUpdate_(nullptr)
, gdiSurfaceBits_(nullptr)
, gdiGfxSurfaceCommand_(nullptr) {
	RDP_CLIENT_ENTRY_POINTS entryPoints;
	ZeroMemory(&entryPoints, sizeof(entryPoints));
	entryPoints.Version = RDP_CLIENT_INTERFACE_VERSION;
	entryPoints.Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
	entryPoints.ContextSize = sizeof(RdpClientMockContext);
	entryPoints.ClientNew = _client_new;
	entryPoints.ClientFree = _client_free;
	entryPoints.ClientStart = _client_start;
	entryPoints.ClientStop = _client_stop;

	rdpClient_ = (RdpClientMockContext *)freerdp_client_context_new(&entryPoints);
	if (!rdpClient_)
		throw std::bad_alloc();
	rdpClient_->mock_ = this;
	settings_ = rdpClient_->common_.context.settings;
}

RdpClientMock::~RdpClientMock() {
	if (connectionEstablished_) {
		freerdp_disconnect(rdpClient_->common_.context.instance);
		connectionEstablished_ = false;
	}
	freerdp_client_context_free(&rdpClient_->common_.context);
	delete output_;
}

BOOL RdpClientMock::_client_new(freerdp *instance, rdpContext *context) {
	if (!instance || !context)
		return FALSE;

	instance->PreConnect = _client_preconnect;
	instance->PostConnect = _client_postconnect;
	instance->Redirect = _client_redirect;
	instance->PostDisconnect = _client_postdisconnect;
	return TRUE;
}

void RdpClientMock::_client_free(freerdp *instance, rdpContext *context) {}

int RdpClientMock::_client_start(rdpContext *context) {
	return 0;
}

int RdpClientMock::_client_stop(rdpContext *context) {
	return 0;
}
#ifdef HAVE_CLIENT_MONITOR_STATES
void RdpClientMock::_on_state_changed(void *context, const StateChangedEventArgs *e) {
	/*WLog_INFO(TAG, "StateChanged: %s -> %s", freerdp_state_string(e->oldState),
	          freerdp_state_string(e->newState));*/

	RdpClientMockContext *mcontext = (RdpClientMockContext *)context;
	RdpClientMock *mock = mcontext->mock_;
	if (mock->monitorStates_)
		mock->output_->sendNotification("states", freerdp_state_string(e->newState));
}

void RdpClientMock::_on_connection_state_change(
	void *context, const ConnectionStateChangeEventArgs *e) {
	/*WLog_INFO(TAG, "ConnectionStateChange: state=%d active=%d", e->state, e->active);*/

	RdpClientMockContext *mcontext = (RdpClientMockContext *)context;
	RdpClientMock *mock = mcontext->mock_;
	if (mock->monitorConnectionState_)
		mock->output_->sendNotification("connectionState",
			"state=" + std::to_string(e->state) + " active=" + std::to_string(e->active));
}
#endif

void RdpClientMock::_on_connection_result(void *context, const ConnectionResultEventArgs *e) {
	//WLog_INFO(TAG, "ConnectionResult: %d", e->result);
}

BOOL RdpClientMock::_client_preconnect(freerdp *instance) {
	if (!instance || !instance->context)
		return FALSE;

	wPubSub *pubSub = instance->context->pubSub;
#ifdef HAVE_CLIENT_MONITOR_STATES
	PubSub_SubscribeStateChanged(pubSub, _on_state_changed);
	PubSub_SubscribeConnectionStateChange(pubSub, _on_connection_state_change);
#endif
	PubSub_SubscribeConnectionResult(pubSub, _on_connection_result);
	PubSub_SubscribeChannelConnected(pubSub, _on_channel_connected);
	PubSub_SubscribeChannelDisconnected(pubSub, _on_channel_disconnected);

	instance->context->update->BitmapUpdate = _client_bitmap_update;
	instance->context->update->SurfaceBits = _client_surface_bits;

	/* this is a mock client for testing, not a real client acting on behalf of a user: never
	 * block on an interactive certificate trust decision, servers are expected to be test/mock
	 * instances with self-signed certificates */
	if (!freerdp_settings_set_bool(instance->context->settings, FreeRDP_IgnoreCertificate, TRUE))
		return FALSE;

	/* without this, freerdp_client_load_addins() (client/common/cmdline.c) never registers the
	 * "rdpgfx" dynamic channel addin at all, so CAPSADVERTISE is never sent and the server-side
	 * SupportGraphicsPipeline negotiation collapses to FALSE regardless of the server's own
	 * default */
	if (!freerdp_settings_set_bool(
			instance->context->settings, FreeRDP_SupportGraphicsPipeline, TRUE))
		return FALSE;

	/* same AND-with-server-default pattern as SupportGraphicsPipeline above (see gcc.c's
	 * updateEarlyClientCaps): without this, the server's own default is negotiated down to FALSE
	 * regardless of what it advertises, so the server never gets a validated MonitorCount/
	 * MonitorDefArray to build gfx surfaces from -- neither the "screens" command's explicit
	 * layout nor the server's synthesized single-monitor fallback would reach it */
	if (!freerdp_settings_set_bool(
			instance->context->settings, FreeRDP_SupportMonitorLayoutPdu, TRUE))
		return FALSE;

	/* same AND-with-server-default pattern again, this time for the base (non-EGFX) Bitmap Codecs
	 * capability set: rdp_write_bitmap_codecs_capability_set only includes the RemoteFX GUID in
	 * this client's own outgoing capability set (its Confirm Active reply) when its local
	 * FreeRDP_RemoteFxCodec is already true -- without this, the server never sees RemoteFX
	 * negotiated for the connection (FreeRDP_RemoteFxCodec stays FALSE on its own peer settings),
	 * so "encoding legacyRfx" server-side would never resolve to anything usable */
	if (!freerdp_settings_set_bool(instance->context->settings, FreeRDP_RemoteFxCodec, TRUE))
		return FALSE;

	/* loads (or not) the cliprdr static channel addin, see the "clipboard" command */
	RdpClientMock *mock = ((RdpClientMockContext *)instance->context)->mock_;
	if (!freerdp_settings_set_bool(
			instance->context->settings, FreeRDP_RedirectClipboard, mock->clipboard_.isEnabled()))
		return FALSE;

	//WLog_INFO(TAG, "preconnect");
	return TRUE;
}

BOOL RdpClientMock::_client_postconnect(freerdp *instance) {
	if (!instance || !instance->context)
		return FALSE;

	RdpClientMockContext *context = (RdpClientMockContext *)instance->context;
	RdpClientMock *mock = context->mock_;
	mock->connectionEstablished_ = true;

	if (mock->monitorSecurity_)
		mock->output_->sendNotification("security",
			securityProtocolName(freerdp_settings_get_uint32(
				instance->context->settings, FreeRDP_SelectedProtocol)));

	if (mock->screenshotsEnabled_) {
		/* gdi_init must run here (it needs the rdpCache created during the connect sequence), and
		 * it overrides the BitmapUpdate/SurfaceBits callbacks installed in _client_preconnect:
		 * keep gdi's own implementations to chain to, so the framebuffer gets drawn and the
		 * "graphics" notifications are still emitted */
		if (!gdi_init(instance, PIXEL_FORMAT_BGRX32))
			return FALSE;

		rdpUpdate *update = instance->context->update;
		mock->gdiBitmapUpdate_ = update->BitmapUpdate;
		mock->gdiSurfaceBits_ = update->SurfaceBits;
		update->BitmapUpdate = _client_bitmap_update;
		update->SurfaceBits = _client_surface_bits;

		/* also required by gdi's EGFX ResetGraphics handler, which asserts it is set */
		update->DesktopResize = _client_desktop_resize;
	}

	//WLog_INFO(TAG, "postconnect");
	return TRUE;
}

void RdpClientMock::_client_postdisconnect(freerdp *instance) {
	if (!instance || !instance->context)
		return;

	gdi_free(instance);
}

BOOL RdpClientMock::_client_desktop_resize(rdpContext *context) {
	rdpGdi *gdi = context->gdi;
	if (!gdi)
		return TRUE;

	return gdi_resize(gdi, freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth),
		freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight));
}

BOOL RdpClientMock::_client_redirect(freerdp *instance) {
	if (!instance || !instance->context)
		return FALSE;

	RdpClientMockContext *context = (RdpClientMockContext *)instance->context;
	RdpClientMock *mock = context->mock_;

	const char *host =
		freerdp_settings_get_string(instance->context->settings, FreeRDP_ServerHostname);
	mock->output_->sendNotification("redirect", host ? host : "");

	return TRUE;
}

void RdpClientMock::_on_channel_connected(void *context, const ChannelConnectedEventArgs *e) {
	RdpClientMockContext *mcontext = (RdpClientMockContext *)context;
	RdpClientMock *mock = mcontext->mock_;

	if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
		mock->clipboard_.attach((CliprdrClientContext *)e->pInterface);
	} else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {
		mock->disp_ = (DispClientContext *)e->pInterface;
		mock->disp_->custom = mock;
		mock->disp_->DisplayControlCaps = _on_disp_caps;
	} else if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
		mock->gfx_ = (RdpgfxClientContext *)e->pInterface;
		rdpGdi *gdi = mcontext->common_.context.gdi;
		if (mock->screenshotsEnabled_ && gdi) {
			/* gdi takes over gfx->custom (for its own rdpGdi) and all the surface callbacks,
			 * the _on_gdi_gfx_* variants reach the mock through gdi->context instead */
			if (gdi_graphics_pipeline_init(gdi, mock->gfx_)) {
				mock->gdiGfxSurfaceCommand_ = mock->gfx_->SurfaceCommand;
				mock->gfx_->SurfaceCommand = _on_gdi_gfx_surface_command;
				mock->gfx_->CapsConfirm = _on_gdi_gfx_caps_confirm;
			} else {
				WLog_ERR(TAG, "unable to initialize the gdi graphics pipeline");
			}
		} else {
			mock->gfx_->custom = mock;
			mock->gfx_->SurfaceCommand = _on_gfx_surface_command;
			mock->gfx_->CapsConfirm = _on_gfx_caps_confirm;
		}
	}

	/* dynamic virtual channels (like "disp") finish their own setup handshake some time after
	 * the main connection is already reported as established, so callers that need to act on a
	 * specific channel (e.g. "dyn-resolution") have a deterministic signal to wait on instead of
	 * guessing a delay */
	if (mock->monitorChannels_)
		mock->output_->sendNotification("channelConnected", e->name);
}

void RdpClientMock::_on_channel_disconnected(void *context, const ChannelDisconnectedEventArgs *e) {
	RdpClientMockContext *mcontext = (RdpClientMockContext *)context;
	RdpClientMock *mock = mcontext->mock_;

	if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
		mock->clipboard_.detach();

	if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0)
		mock->disp_ = nullptr;

	if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
		rdpGdi *gdi = mcontext->common_.context.gdi;
		if (gdi && gdi->gfx)
			gdi_graphics_pipeline_uninit(gdi, mock->gfx_);
		mock->gdiGfxSurfaceCommand_ = nullptr;
		mock->gfx_ = nullptr;
	}

	if (mock->monitorChannels_)
		mock->output_->sendNotification("channelDisconnected", e->name);
}

UINT RdpClientMock::_on_disp_caps(DispClientContext *context, UINT32 MaxNumMonitors,
	UINT32 MaxMonitorAreaFactorA, UINT32 MaxMonitorAreaFactorB) {
	RdpClientMock *mock = (RdpClientMock *)context->custom;

	/* the server's Display Control Caps PDU is the actual "ready" signal for the disp channel:
	 * the DVC itself may be connected (see _on_channel_connected) before the channel's own
	 * capability negotiation has completed, and monitor layout updates aren't meaningful until
	 * then */
	mock->output_->sendNotification("display", "channel ready");
	return CHANNEL_RC_OK;
}

UINT RdpClientMock::_on_gfx_caps_confirm(
	RdpgfxClientContext *context, const RDPGFX_CAPS_CONFIRM_PDU *pdu) {
	RdpClientMock *mock = (RdpClientMock *)context->custom;
	mock->onGfxCapsConfirm();
	return CHANNEL_RC_OK;
}

/* screenshots enabled: gfx->custom is gdi's rdpGdi, see _on_channel_connected */
static RdpClientMock *mockFromGdiGfx(RdpgfxClientContext *context) {
	rdpGdi *gdi = (rdpGdi *)context->custom;
	return ((RdpClientMockContext *)gdi->context)->mock_;
}

UINT RdpClientMock::_on_gdi_gfx_caps_confirm(
	RdpgfxClientContext *context, const RDPGFX_CAPS_CONFIRM_PDU *pdu) {
	mockFromGdiGfx(context)->onGfxCapsConfirm();
	return CHANNEL_RC_OK;
}

void RdpClientMock::onGfxCapsConfirm() {
	/* mirrors _on_disp_caps: the gfx DVC being "connected" (see _on_channel_connected) only means
	 * the underlying dynamic virtual channel opened, not that its own CAPSADVERTISE/CAPSCONFIRM
	 * handshake -- the real "server can now gfx_update" signal -- has completed */
	output_->sendNotification("gfx", "channel ready");
}

UINT RdpClientMock::_on_gfx_surface_command(
	RdpgfxClientContext *context, const RDPGFX_SURFACE_COMMAND *cmd) {
	RdpClientMock *mock = (RdpClientMock *)context->custom;
	mock->onGfxSurfaceCommand(cmd);
	return CHANNEL_RC_OK;
}

UINT RdpClientMock::_on_gdi_gfx_surface_command(
	RdpgfxClientContext *context, const RDPGFX_SURFACE_COMMAND *cmd) {
	RdpClientMock *mock = mockFromGdiGfx(context);
	UINT ret = IFCALLRESULT(CHANNEL_RC_OK, mock->gdiGfxSurfaceCommand_, context, cmd);
	mock->onGfxSurfaceCommand(cmd);
	return ret;
}

void RdpClientMock::onGfxSurfaceCommand(const RDPGFX_SURFACE_COMMAND *cmd) {
	/* reuses the same "graphics" notification category/format as the legacy BitmapUpdate/
	 * SurfaceBits paths below, so "monitor graphics" remains a single on/off switch across all
	 * graphics transports -- surfaceId has no equivalent there (those paths have no notion of
	 * multiple surfaces) so it's appended rather than folded into left/top/right/bottom, letting
	 * callers that care (e.g. a multi-monitor gfx test) tell surfaces apart while everything else
	 * still just checks the coordinate fields */
	if (monitorGraphicsUpdates_) {
		output_->sendNotification("graphics",
			"left=" + std::to_string(cmd->left) + " top=" + std::to_string(cmd->top) +
				" right=" + std::to_string(cmd->right) + " bottom=" + std::to_string(cmd->bottom) +
				" surfaceId=" + std::to_string(cmd->surfaceId));
	}
}

BOOL RdpClientMock::_client_bitmap_update(rdpContext *context, const BITMAP_UPDATE *bitmap) {
	RdpClientMockContext *mcontext = (RdpClientMockContext *)context;
	RdpClientMock *mock = mcontext->mock_;

	if (mock->gdiBitmapUpdate_ && !mock->gdiBitmapUpdate_(context, bitmap))
		return FALSE;

	if (mock->monitorGraphicsUpdates_) {
		for (UINT32 i = 0; i < bitmap->number; i++) {
			const BITMAP_DATA &rect = bitmap->rectangles[i];
			mock->output_->sendNotification("graphics",
				"left=" + std::to_string(rect.destLeft) + " top=" + std::to_string(rect.destTop) +
					" right=" + std::to_string(rect.destRight) +
					" bottom=" + std::to_string(rect.destBottom));
		}
	}
	return TRUE;
}

BOOL RdpClientMock::_client_surface_bits(rdpContext *context, const SURFACE_BITS_COMMAND *cmd) {
	RdpClientMockContext *mcontext = (RdpClientMockContext *)context;
	RdpClientMock *mock = mcontext->mock_;

	if (mock->gdiSurfaceBits_ && !mock->gdiSurfaceBits_(context, cmd))
		return FALSE;

	if (mock->monitorGraphicsUpdates_) {
		mock->output_->sendNotification("graphics",
			"left=" + std::to_string(cmd->destLeft) + " top=" + std::to_string(cmd->destTop) +
				" right=" + std::to_string(cmd->destRight) +
				" bottom=" + std::to_string(cmd->destBottom));
	}
	return TRUE;
}

ClientClipboard::ClientClipboard(OutputChannel *output)
: ClipboardHelper(output)
, context_(nullptr) {}

void ClientClipboard::attach(CliprdrClientContext *context) {
	context_ = context;
	context->custom = this;
	context->MonitorReady = _monitor_ready;
	context->ServerFormatList = _server_format_list;
	context->ServerFormatListResponse = _server_format_list_response;
	context->ServerFormatDataRequest = _server_format_data_request;
	context->ServerFormatDataResponse = _server_format_data_response;
	context->ServerFileContentsRequest = _server_file_contents_request;
	context->ServerFileContentsResponse = _server_file_contents_response;
}

void ClientClipboard::detach() {
	reset();
	context_ = nullptr;
}

UINT ClientClipboard::sendFormatList(const CLIPRDR_FORMAT_LIST *formatList) {
	return context_ ? context_->ClientFormatList(context_, formatList) : ERROR_INVALID_HANDLE;
}

UINT ClientClipboard::sendFormatListResponse(const CLIPRDR_FORMAT_LIST_RESPONSE *response) {
	return context_ ? context_->ClientFormatListResponse(context_, response) : ERROR_INVALID_HANDLE;
}

UINT ClientClipboard::sendFormatDataRequest(const CLIPRDR_FORMAT_DATA_REQUEST *request) {
	return context_ ? context_->ClientFormatDataRequest(context_, request) : ERROR_INVALID_HANDLE;
}

UINT ClientClipboard::sendFormatDataResponse(const CLIPRDR_FORMAT_DATA_RESPONSE *response) {
	return context_ ? context_->ClientFormatDataResponse(context_, response) : ERROR_INVALID_HANDLE;
}

UINT ClientClipboard::sendFileContentsRequest(const CLIPRDR_FILE_CONTENTS_REQUEST *request) {
	return context_ ? context_->ClientFileContentsRequest(context_, request) : ERROR_INVALID_HANDLE;
}

UINT ClientClipboard::sendFileContentsResponse(const CLIPRDR_FILE_CONTENTS_RESPONSE *response) {
	return context_ ? context_->ClientFileContentsResponse(context_, response)
					: ERROR_INVALID_HANDLE;
}

UINT ClientClipboard::_monitor_ready(
	CliprdrClientContext *context, const CLIPRDR_MONITOR_READY *pdu) {
	ClientClipboard *clipboard = (ClientClipboard *)context->custom;

	/* the addin masks these flags with what the server advertised before sending them */
	CLIPRDR_GENERAL_CAPABILITY_SET generalCaps = {};
	generalCaps.capabilitySetType = CB_CAPSTYPE_GENERAL;
	generalCaps.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
	generalCaps.version = CB_CAPS_VERSION_2;
	generalCaps.generalFlags = GENERAL_FLAGS;

	CLIPRDR_CAPABILITIES caps = {};
	caps.common.msgType = CB_CLIP_CAPS;
	caps.cCapabilitiesSets = 1;
	caps.capabilitySets = (CLIPRDR_CAPABILITY_SET *)&generalCaps;

	UINT rc = context->ClientCapabilities(context, &caps);
	if (rc != CHANNEL_RC_OK)
		return rc;

	/* sends the mandatory initial Format List */
	clipboard->onReady();
	return CHANNEL_RC_OK;
}

UINT ClientClipboard::_server_format_list(
	CliprdrClientContext *context, const CLIPRDR_FORMAT_LIST *pdu) {
	return ((ClientClipboard *)context->custom)->onPeerFormatList(pdu);
}

UINT ClientClipboard::_server_format_list_response(
	CliprdrClientContext *context, const CLIPRDR_FORMAT_LIST_RESPONSE *pdu) {
	return CHANNEL_RC_OK;
}

UINT ClientClipboard::_server_format_data_request(
	CliprdrClientContext *context, const CLIPRDR_FORMAT_DATA_REQUEST *pdu) {
	return ((ClientClipboard *)context->custom)->onPeerFormatDataRequest(pdu);
}

UINT ClientClipboard::_server_format_data_response(
	CliprdrClientContext *context, const CLIPRDR_FORMAT_DATA_RESPONSE *pdu) {
	return ((ClientClipboard *)context->custom)->onPeerFormatDataResponse(pdu);
}

UINT ClientClipboard::_server_file_contents_request(
	CliprdrClientContext *context, const CLIPRDR_FILE_CONTENTS_REQUEST *pdu) {
	return ((ClientClipboard *)context->custom)->onPeerFileContentsRequest(pdu);
}

UINT ClientClipboard::_server_file_contents_response(
	CliprdrClientContext *context, const CLIPRDR_FILE_CONTENTS_RESPONSE *pdu) {
	return ((ClientClipboard *)context->custom)->onPeerFileContentsResponse(pdu);
}

bool RdpClientMock::writeScreenshot(const std::string &path, std::string &error) {
	rdpGdi *gdi = rdpClient_->common_.context.gdi;
	if (!gdi || !gdi->primary_buffer) {
		error = "no framebuffer";
		return false;
	}

	/* EGFX surfaces are composed into the primary buffer from the drdynvc thread (see gdi's
	 * UpdateSurfaces/ResetGraphics, both run under the gfx mux), legacy updates are treated from
	 * this thread */
	RdpgfxClientContext *gfx = gdi->gfx;
	if (gfx)
		EnterCriticalSection(&gfx->mux);

	const UINT32 width = (UINT32)gdi->width;
	const UINT32 height = (UINT32)gdi->height;
	const size_t lineSize = (size_t)width * 4;
	std::vector<BYTE> pixels(lineSize * height);
	for (UINT32 y = 0; y < height; y++)
		memcpy(&pixels[y * lineSize], gdi->primary_buffer + (size_t)y * gdi->stride, lineSize);

	if (gfx)
		LeaveCriticalSection(&gfx->mux);

	try {
		/* the framebuffer is PIXEL_FORMAT_BGRX32 (see _client_postconnect), 'P' skips the padding
		 * byte */
		Magick::Image image(width, height, "BGRP", Magick::CharPixel, pixels.data());
		image.magick("PNG");
		image.write(path);
	} catch (Magick::Exception &e) {
		WLog_ERR(TAG, "unable to write screenshot %s: %s", path.c_str(), e.what());
		error = "write failed";
		return false;
	}
	return true;
}

BOOL RdpClientMock::connectClient() {
	if (freerdp_client_start(&rdpClient_->common_.context) != CHANNEL_RC_OK) {
		WLog_ERR(TAG, "unable to start freerdp client");
		return FALSE;
	}

	if (!freerdp_connect(rdpClient_->common_.context.instance)) {
		WLog_ERR(TAG, "unable to connect freerdp client");
		output_->sendResult(false);
		return FALSE;
	}

	connectionEstablished_ = true;
	output_->sendResult(true);
	return TRUE;
}


int RdpClientMock::run() {
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
		DWORD pollDelay;

		if (now > pollCmdChannelStartDate_) {
			pollCmd = true;
			handles[nhandles++] = hcmd;
			pollDelay = INFINITE;
		} else {
			pollCmd = false;
			pollDelay = (1 + pollCmdChannelStartDate_ - now);
		}

		/* before "connect" has run, the FreeRDP context has no transport yet: its layer defaults
		 * to closed, so freerdp_check_event_handles() below would spuriously report an error and
		 * kill this process before it ever gets to connect (usually masked because the initial
		 * commands arrive close enough together to be drained in a single pollFd()/treat() before
		 * this point is ever reached -- but not guaranteed, e.g. under CPU contention) */
		DWORD nrdpHandles = connectionEstablished_
			? freerdp_get_event_handles(
				  &rdpClient_->common_.context, &handles[nhandles], MAXIMUM_WAIT_OBJECTS - nhandles)
			: 0;
		if (nrdpHandles)
			nhandles += nrdpHandles;
		DWORD status = WaitForMultipleObjects(nhandles, handles, FALSE, pollDelay);

		switch (status) {
		case WAIT_TIMEOUT:
			continue;
		case WAIT_FAILED:
			doRun_ = FALSE;
			continue;
		default:
			break;
		}

		if (pollCmd) {
			if (WaitForSingleObject(hcmd, 0) == WAIT_OBJECT_0) {
				switch (commandChannel_.pollFd()) {
				case CommandChannel::POLL_SUCCESS:
					break;
				case CommandChannel::POLL_ERROR:
				case CommandChannel::POLL_CLOSED:
					doRun_ = false;
				}
			}

			if (commandChannel_.hasBufferData()) {
				switch (commandChannel_.treat()) {
				case CommandChannel::POLL_SUCCESS:
					break;
				case CommandChannel::POLL_ERROR:
				case CommandChannel::POLL_CLOSED:
					doRun_ = FALSE;
					break;
				}
			}
		}

		if (connectionEstablished_ && !freerdp_check_event_handles(&rdpClient_->common_.context))
			doRun_ = false;
	}

	if (connectionEstablished_) {
		freerdp_disconnect(rdpClient_->common_.context.instance);
		connectionEstablished_ = false;
	}

	return 0;
}


ClientCommandChannel::ClientCommandChannel(int fd, RdpClientMock *mock)
: CommandChannel(fd)
, mock_(mock) {}

CommandChannel::TreatResult ClientCommandChannel::onCommand(
	const std::string &cmd, const std::string &args) {
	struct StringSettingCommand {
		const char *cmd;
		FreeRDP_Settings_Keys_String settingId;
	};

	static const StringSettingCommand stringSettingCommands[] = {
		{ "user", FreeRDP_Username },
		{ "password", FreeRDP_Password },
		{ "domain", FreeRDP_Domain },
		{ "authpkglist", FreeRDP_AuthenticationPackageList },
	};

	rdpSettings *settings = mock_->settings_;

	if (mock_->clipboard_.handleCommand(cmd, args))
		return TREAT_SUCCESS;

	for (const auto &entry : stringSettingCommands) {
		if (cmd == entry.cmd)
			return toTreatResult(
				freerdp_settings_set_string(settings, entry.settingId, args.c_str()));
	}

	if (cmd == "authType") {
		return toTreatResult(setSecurityProtocols(settings, args));

	} else if (cmd == "adminMode") {
		if (!freerdp_settings_set_bool(settings, FreeRDP_ConsoleSession, TRUE))
			return TREAT_ERROR;

	} else if (cmd == "geometry") {
		size_t xPos = args.find('x');
		if (xPos == std::string::npos)
			return TREAT_ERROR;
		std::string widthStr = args.substr(0, xPos);
		std::string heightStr = args.substr(xPos + 1);

		UINT32 width = (UINT32)strtoul(widthStr.c_str(), nullptr, 10);
		UINT32 height = (UINT32)strtoul(heightStr.c_str(), nullptr, 10);

		return toTreatResult(freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, width) &&
			freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, height));

	} else if (cmd == "screens") {
		/* sets the monitor layout announced as GCC Client Monitor Data at connect time (2.2.1.3.6),
		 * unlike "dyn-resolution" below which sends a layout update over the disp channel after
		 * the fact -- must be issued before "connect" to have any effect */
		std::vector<rdpMonitor> monitors;
		size_t start = 0;
		bool parseError = false;
		while (start <= args.size()) {
			size_t semi = args.find(';', start);
			std::string spec =
				args.substr(start, semi == std::string::npos ? std::string::npos : semi - start);

			rdpMonitor monitor;
			if (!parseScreenSpec(spec, monitor)) {
				parseError = true;
				break;
			}
			monitors.push_back(monitor);

			if (semi == std::string::npos)
				break;
			start = semi + 1;
		}

		if (parseError || monitors.empty()) {
			WLog_ERR(TAG, "invalid screen spec: %s", args.c_str());
			return TREAT_ERROR;
		}
		/* 2.2.1.3.6: "the maximum allowed is 16" */
		if (monitors.size() > 16) {
			WLog_ERR(TAG, "screens needs at most 16 monitors");
			return TREAT_ERROR;
		}

		/* the first listed monitor is the primary one, same convention as "dyn-resolution" */
		monitors[0].is_primary = TRUE;

		/* a real client's platform-specific monitor setup (e.g. xf_monitor.c's own bounding-box
		 * computation, run before freerdp_connect) always keeps DesktopWidth/DesktopHeight in
		 * sync with the union of all monitors being announced -- nothing generic in FreeRDP does
		 * this for us (freerdp_settings_set_monitor_def_array_sorted below only touches
		 * MonitorDefArray/MonitorCount/MonitorLocalShiftX/Y), so it's done explicitly here.
		 * Skipping this would leave DesktopWidth/Height at whatever "geometry" set (or the
		 * built-in default) -- too small to cover every monitor -- and
		 * update_recv_surfcmd_is_rect_valid()/its BITMAP_UPDATE equivalent would then silently
		 * reject any legacy (non-EGFX) update landing on a monitor past that boundary. */
		INT64 boundLeft = monitors[0].x;
		INT64 boundTop = monitors[0].y;
		INT64 boundRight = boundLeft + monitors[0].width;
		INT64 boundBottom = boundTop + monitors[0].height;
		for (const rdpMonitor &monitor : monitors) {
			boundLeft = std::min<INT64>(boundLeft, monitor.x);
			boundTop = std::min<INT64>(boundTop, monitor.y);
			boundRight = std::max<INT64>(boundRight, (INT64)monitor.x + monitor.width);
			boundBottom = std::max<INT64>(boundBottom, (INT64)monitor.y + monitor.height);
		}

		if (!freerdp_settings_set_monitor_def_array_sorted(
				settings, monitors.data(), monitors.size()))
			return TREAT_ERROR;

		if (!freerdp_settings_set_uint32(
				settings, FreeRDP_DesktopWidth, (UINT32)(boundRight - boundLeft)) ||
			!freerdp_settings_set_uint32(
				settings, FreeRDP_DesktopHeight, (UINT32)(boundBottom - boundTop)))
			return TREAT_ERROR;

		/* the monitor data block is only actually written onto the wire (see
		 * gcc_write_client_data_blocks in libfreerdp/core/gcc.c) when UseMultimon is set and
		 * SpanMonitors isn't -- just populating MonitorDefArray/MonitorCount above isn't enough */
		return toTreatResult(freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, TRUE) &&
			freerdp_settings_set_bool(settings, FreeRDP_SpanMonitors, FALSE));

	} else if (cmd == "dyn-resolution") {
		/* unlike most commands, these are runtime conditions (the display control channel
		 * finishes its own setup asynchronously, see _on_channel_connected) or caller-supplied
		 * monitor data, not a misuse of the command itself -- report them as a RESULT:FAILURE
		 * that the caller can act on (e.g. retry) instead of TREAT_ERROR, which would tear down
		 * the whole client */
		if (!mock_->connectionEstablished_ || !mock_->disp_) {
			mock_->output_->sendResult(false, "channel not ready");
			return TREAT_SUCCESS;
		}

		std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT> monitors;
		std::istringstream iss(args);
		std::string spec;
		bool parseError = false;
		while (iss >> spec) {
			DISPLAY_CONTROL_MONITOR_LAYOUT layout;
			if (!parseMonitorSpec(spec, layout)) {
				parseError = true;
				break;
			}
			monitors.push_back(layout);
		}

		if (parseError || monitors.empty()) {
			mock_->output_->sendResult(false, "invalid monitor spec");
			return TREAT_SUCCESS;
		}
		if (monitors.size() > DYN_RESOLUTION_MAX_MONITORS) {
			mock_->output_->sendResult(false, "too many monitors");
			return TREAT_SUCCESS;
		}
		if (monitorsOverlap(monitors)) {
			mock_->output_->sendResult(false, "overlapping monitors");
			return TREAT_SUCCESS;
		}

		/* the first listed monitor is the primary one */
		monitors[0].Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
		for (DISPLAY_CONTROL_MONITOR_LAYOUT &monitor : monitors) {
			monitor.DesktopScaleFactor = 100;
			monitor.DeviceScaleFactor = 100;
		}

		bool sent = IFCALLRESULT(CHANNEL_RC_OK, mock_->disp_->SendMonitorLayout, mock_->disp_,
						(UINT32)monitors.size(), monitors.data()) == CHANNEL_RC_OK;
		mock_->output_->sendResult(sent, sent ? "" : "send failed");
		return TREAT_SUCCESS;

	} else if (cmd == "clipboard") {
		/* the cliprdr addin is loaded (or not) at connect time, see _client_preconnect */
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

	} else if (cmd == "screenshots_enable") {
		/* the gdi (local framebuffer) is only set up in _client_postconnect, so this must be
		 * issued before "connect" */
		if (mock_->connectionEstablished_) {
			mock_->output_->sendResult(false, "already connected");
			return TREAT_SUCCESS;
		}
		mock_->screenshotsEnabled_ = true;
		mock_->output_->sendResult(true);

	} else if (cmd == "screenshot") {
		/* same as "dyn-resolution": runtime conditions are reported as a RESULT:FAILURE rather
		 * than a TREAT_ERROR that would tear down the whole client */
		std::string error;
		if (!mock_->screenshotsEnabled_)
			error = "screenshots not enabled";
		else if (!mock_->connectionEstablished_)
			error = "not connected";
		else if (args.empty())
			error = "missing file name";
		else
			mock_->writeScreenshot(args, error);

		mock_->output_->sendResult(error.empty(), error);
		return TREAT_SUCCESS;

	} else if (cmd == "connect") {
		size_t colonPos = args.rfind(':');
		std::string host = (colonPos == std::string::npos) ? args : args.substr(0, colonPos);
		UINT32 port = 3389;
		if (colonPos != std::string::npos)
			port = (UINT32)strtoul(args.c_str() + colonPos + 1, nullptr, 10);

		if (!freerdp_settings_set_string(settings, FreeRDP_ServerHostname, host.c_str()))
			return TREAT_ERROR;
		if (!freerdp_settings_set_uint32(settings, FreeRDP_ServerPort, port))
			return TREAT_ERROR;

		mock_->connectClient();

	} else if (cmd == "pause") {
		UINT32 delay = (UINT32)strtoul(args.c_str(), nullptr, 10);
		if (delay && delay < 100 * 1000)
			mock_->pollCmdChannelStartDate_ = GetTickCount64() + delay;
		return TREAT_SKIP;

	} else if (cmd == "mouse") {
		if (!mock_->connectionEstablished_)
			return TREAT_ERROR;
		size_t spacePos = args.rfind(' ');
		if (spacePos == std::string::npos)
			return TREAT_ERROR;
		std::string xStr = args.substr(0, spacePos);
		std::string yStr = args.substr(spacePos + 1);

		UINT32 x = strtoul(xStr.c_str(), nullptr, 10);
		UINT32 y = strtoul(yStr.c_str(), nullptr, 10);

		if (!freerdp_input_send_mouse_event(mock_->rdpClient_->common_.context.input, 0, x, y))
			return TREAT_ERROR;
	} else if (cmd == "quit") {
		mock_->doRun_ = false;
		mock_->output_->sendResult(true);
	} else if (cmd == "monitor") {
		/* args is a comma separated list of kinds, e.g. "states,graphics" */
		size_t start = 0;
		while (start <= args.size()) {
			size_t comma = args.find(',', start);
			std::string token =
				args.substr(start, comma == std::string::npos ? std::string::npos : comma - start);

			if (token == "states") {
#ifdef HAVE_CLIENT_MONITOR_STATES
				mock_->monitorStates_ = true;
#else
				WLog_ERR(TAG, "RDP client state monitoring not supported by your FreeRDP");
				return TREAT_ERROR;
#endif
			} else if (token == "connectionState") {
#ifdef HAVE_CLIENT_MONITOR_STATES
				mock_->monitorConnectionState_ = true;
#else
				WLog_ERR(TAG, "RDP client state monitoring not supported by your FreeRDP");
				return TREAT_ERROR;
#endif
			} else if (token == "graphics") {
				mock_->monitorGraphicsUpdates_ = true;
			} else if (token == "channels") {
				mock_->monitorChannels_ = true;
			} else if (token == "security") {
				mock_->monitorSecurity_ = true;
			} else if (token == "clipboard") {
				mock_->clipboard_.setMonitor(true);
			} else if (token == "off") {
				mock_->clipboard_.setMonitor(false);
				mock_->monitorStates_ = false;
				mock_->monitorConnectionState_ = false;
				mock_->monitorGraphicsUpdates_ = false;
				mock_->monitorChannels_ = false;
				mock_->monitorSecurity_ = false;
			} else {
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
	MockOptions options;

	int res = options.parseArgs(argc, argv);
	if (res)
		return res;

	if (!options.debugMode)
		WLog_SetLogLevel(WLog_GetRoot(), WLOG_OFF);

	Magick::InitializeMagick(*argv);

	RdpClientMock mock(options.cmdFd,
		options.jsonOutput ? new JsonOutputChannel(options.outFd)
						   : new OutputChannel(options.outFd));

	return mock.run();
}
