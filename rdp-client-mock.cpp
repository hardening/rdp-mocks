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
#include <cstdlib>
#include <sstream>
#include <vector>

#include <winpr/synch.h>
#include <winpr/sysinfo.h>

#include <freerdp/gdi/gdi.h>

#include "rdp-client-mock.h"
#include "mockOptions.h"

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
, doRun_(true)
, commandChannel_(fd, this)
, connectionEstablished_(false)
, pollCmdChannelStartDate_(0)
, disp_(nullptr) {
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
	settings_ = rdpClient_->context_.settings;
}

RdpClientMock::~RdpClientMock() {
	if (connectionEstablished_) {
		freerdp_disconnect(rdpClient_->context_.instance);
		connectionEstablished_ = false;
	}
	freerdp_client_context_free(&rdpClient_->context_);
	delete output_;
}

BOOL RdpClientMock::_client_new(freerdp *instance, rdpContext *context) {
	if (!instance || !context)
		return FALSE;

	instance->PreConnect = _client_preconnect;
	instance->PostConnect = _client_postconnect;
	instance->Redirect = _client_redirect;
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

	//WLog_INFO(TAG, "preconnect");
	return TRUE;
}

BOOL RdpClientMock::_client_postconnect(freerdp *instance) {
	if (!instance || !instance->context)
		return FALSE;

	RdpClientMockContext *context = (RdpClientMockContext *)instance->context;
	context->mock_->connectionEstablished_ = true;

	//WLog_INFO(TAG, "postconnect");
	return TRUE;
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

	if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {
		mock->disp_ = (DispClientContext *)e->pInterface;
		mock->disp_->custom = mock;
		mock->disp_->DisplayControlCaps = _on_disp_caps;
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

	if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0)
		mock->disp_ = nullptr;

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

BOOL RdpClientMock::_client_bitmap_update(rdpContext *context, const BITMAP_UPDATE *bitmap) {
	RdpClientMockContext *mcontext = (RdpClientMockContext *)context;
	RdpClientMock *mock = mcontext->mock_;

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

	if (mock->monitorGraphicsUpdates_) {
		mock->output_->sendNotification("graphics",
			"left=" + std::to_string(cmd->destLeft) + " top=" + std::to_string(cmd->destTop) +
				" right=" + std::to_string(cmd->destRight) +
				" bottom=" + std::to_string(cmd->destBottom));
	}
	return TRUE;
}

BOOL RdpClientMock::connectClient() {
	if (freerdp_client_start(&rdpClient_->context_) != CHANNEL_RC_OK) {
		WLog_ERR(TAG, "unable to start freerdp client");
		return FALSE;
	}

	if (!freerdp_connect(rdpClient_->context_.instance)) {
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

		DWORD nrdpHandles = freerdp_get_event_handles(
			&rdpClient_->context_, &handles[nhandles], MAXIMUM_WAIT_OBJECTS - nhandles);
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

		if (!freerdp_check_event_handles(&rdpClient_->context_))
			doRun_ = false;
	}

	if (connectionEstablished_) {
		freerdp_disconnect(rdpClient_->context_.instance);
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

	for (const auto &entry : stringSettingCommands) {
		if (cmd == entry.cmd)
			return toTreatResult(
				freerdp_settings_set_string(settings, entry.settingId, args.c_str()));
	}

	if (cmd == "authType") {
		FreeRDP_Settings_Keys_Bool secId;
		if (args == "rdp")
			secId = FreeRDP_RdpSecurity;
		else if (args == "nla")
			secId = FreeRDP_NlaSecurity;
		else if (args == "tls")
			secId = FreeRDP_TlsSecurity;
		else {
			WLog_ERR(TAG, "unknown authType: %s", args.c_str());
			return TREAT_ERROR;
		}

		if (!freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE) ||
			!freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE) ||
			!freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, FALSE) ||
			!freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, FALSE))
			return TREAT_ERROR;

		if (!freerdp_settings_set_bool(settings, secId, TRUE))
			return TREAT_ERROR;
		return toTreatResult((secId != FreeRDP_RdpSecurity) ||
			freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, TRUE));

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

		if (!freerdp_input_send_mouse_event(mock_->rdpClient_->context_.input, 0, x, y))
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
			} else if (token == "off") {
				mock_->monitorStates_ = false;
				mock_->monitorConnectionState_ = false;
				mock_->monitorGraphicsUpdates_ = false;
				mock_->monitorChannels_ = false;
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

	RdpClientMock mock(options.cmdFd,
		options.jsonOutput ? new JsonOutputChannel(options.outFd)
						   : new OutputChannel(options.outFd));

	return mock.run();
}
