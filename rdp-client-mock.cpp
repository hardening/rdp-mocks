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

#include <winpr/synch.h>
#include <winpr/sysinfo.h>

#include <freerdp/gdi/gdi.h>

#include "rdp-client-mock.h"
#include "mockOptions.h"

#define TAG "rdp-client-mock"

RdpClientMock::RdpClientMock(int fd, OutputChannel *output)
: cmdFd_(fd)
, output_(output)
, monitorStates_(false)
, monitorConnectionState_(false)
, doRun_(true)
, commandChannel_(fd, this)
, connectionEstablished_(false)
, pollCmdChannelStartDate_(0)
{
	RDP_CLIENT_ENTRY_POINTS entryPoints;
	ZeroMemory(&entryPoints, sizeof(entryPoints));
	entryPoints.Version = RDP_CLIENT_INTERFACE_VERSION;
	entryPoints.Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
	entryPoints.ContextSize = sizeof(RdpClientMockContext);
	entryPoints.ClientNew = _client_new;
	entryPoints.ClientFree = _client_free;
	entryPoints.ClientStart = _client_start;
	entryPoints.ClientStop = _client_stop;

	rdpClient_ = (RdpClientMockContext*)freerdp_client_context_new(&entryPoints);
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
	return TRUE;
}

void RdpClientMock::_client_free(freerdp *instance, rdpContext *context)
{
}

int RdpClientMock::_client_start(rdpContext *context)
{
	return 0;
}

int RdpClientMock::_client_stop(rdpContext *context)
{
	return 0;
}

void RdpClientMock::_on_state_changed(void *context, const StateChangedEventArgs *e) {
	/*WLog_INFO(TAG, "StateChanged: %s -> %s", freerdp_state_string(e->oldState),
	          freerdp_state_string(e->newState));*/

	RdpClientMockContext *mcontext = (RdpClientMockContext *)context;
	RdpClientMock *mock = mcontext->mock_;
	if (mock->monitorStates_)
		mock->output_->sendNotification("states", freerdp_state_string(e->newState));
}

void RdpClientMock::_on_connection_state_change(void *context, const ConnectionStateChangeEventArgs *e) {
	/*WLog_INFO(TAG, "ConnectionStateChange: state=%d active=%d", e->state, e->active);*/

	RdpClientMockContext *mcontext = (RdpClientMockContext *)context;
	RdpClientMock *mock = mcontext->mock_;
	if (mock->monitorConnectionState_)
		mock->output_->sendNotification("connectionState", "state=" + std::to_string(e->state) +
				" active=" + std::to_string(e->active));

}

void RdpClientMock::_on_connection_result(void *context, const ConnectionResultEventArgs *e) {
	//WLog_INFO(TAG, "ConnectionResult: %d", e->result);
}

BOOL RdpClientMock::_client_preconnect(freerdp *instance) {
	if (!instance || !instance->context)
		return FALSE;

	wPubSub *pubSub = instance->context->pubSub;
	PubSub_SubscribeStateChanged(pubSub, _on_state_changed);
	PubSub_SubscribeConnectionStateChange(pubSub, _on_connection_state_change);
	PubSub_SubscribeConnectionResult(pubSub, _on_connection_result);

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

	RdpClientMockContext *context = (RdpClientMockContext*)instance->context;
	context->mock_->connectionEstablished_ = true;

	//WLog_INFO(TAG, "postconnect");
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
				switch(commandChannel_.poll()) {
				case CommandChannel::POLL_SUCCESS:
					break;
				case CommandChannel::POLL_ERROR:
				case CommandChannel::POLL_CLOSED:
					doRun_ = FALSE;
				}
			}
		}

	}

	if (connectionEstablished_) {
		freerdp_disconnect(rdpClient_->context_.instance);
		connectionEstablished_ = false;
	}

	return 0;
}


ClientCommandChannel::ClientCommandChannel(int fd, RdpClientMock *mock)
: CommandChannel(fd)
, mock_(mock)
{
}

BOOL ClientCommandChannel::onCommand(const std::string &cmd, const std::string &args) {

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
			return freerdp_settings_set_string(settings, entry.settingId, args.c_str());
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
			return TRUE;
		}

		if (!freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE) ||
			!freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE) ||
			!freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, FALSE) ||
			!freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, FALSE))
			return FALSE;

		if (!freerdp_settings_set_bool(settings, secId, TRUE))
			return FALSE;
		return (secId != FreeRDP_RdpSecurity) ||
			freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, TRUE);

	} else if (cmd == "adminMode") {
		if (!freerdp_settings_set_bool(settings, FreeRDP_ConsoleSession, TRUE))
			return FALSE;

	} else if (cmd == "geometry") {
		size_t xPos = args.find('x');
		if (xPos == std::string::npos)
			return FALSE;
		std::string widthStr = args.substr(0, xPos);
		std::string heightStr = args.substr(xPos + 1);

		UINT32 width = (UINT32)strtoul(widthStr.c_str(), nullptr, 10);
		UINT32 height = (UINT32)strtoul(heightStr.c_str(), nullptr, 10);

		return freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, width) &&
				freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, height);

	} else if (cmd == "connect") {
		size_t colonPos = args.rfind(':');
		std::string host = (colonPos == std::string::npos) ? args : args.substr(0, colonPos);
		UINT32 port = 3389;
		if (colonPos != std::string::npos)
			port = (UINT32)strtoul(args.c_str() + colonPos + 1, nullptr, 10);

		if (!freerdp_settings_set_string(settings, FreeRDP_ServerHostname, host.c_str()))
			return FALSE;
		if (!freerdp_settings_set_uint32(settings, FreeRDP_ServerPort, port))
			return FALSE;

		mock_->connectClient();

	} else if (cmd == "pause") {
		UINT32 delay = (UINT32)strtoul(args.c_str(), nullptr, 10);
		if (delay && delay < 100 * 1000)
			mock_->pollCmdChannelStartDate_ = GetTickCount64() + delay;

	} else if (cmd == "mouse") {
		if (!mock_->connectionEstablished_)
			return FALSE;
		size_t spacePos = args.rfind(' ');
		if (spacePos == std::string::npos)
			return FALSE;
		std::string xStr = args.substr(0, spacePos);
		std::string yStr = args.substr(spacePos + 1);

		UINT32 x = strtoul(xStr.c_str(), nullptr, 10);
		UINT32 y = strtoul(yStr.c_str(), nullptr, 10);

		if (!freerdp_input_send_mouse_event(mock_->rdpClient_->context_.input, 0, x, y))
			return FALSE;
	} else if (cmd == "quit") {
		mock_->doRun_ = false;
		mock_->output_->sendResult(true);
	} else if (cmd == "monitor") {
		if (args == "states")
			mock_->monitorStates_ = true;
		else if (args == "connectionState")
			mock_->monitorConnectionState_ = true;
		else if (args == "off") {
			mock_->monitorStates_ = false;
			mock_->monitorConnectionState_ = false;
		} else {
			WLog_ERR(TAG, "unknown monitor kind");
			return FALSE;
		}
	}
	return TRUE;
}

int main(int argc, char *argv[]) {
	MockOptions options;

	int res = options.parseArgs(argc, argv);
	if (res)
		return res;

	if (!options.debugMode)
		WLog_SetLogLevel(WLog_GetRoot(), WLOG_OFF);

	RdpClientMock mock(options.cmdFd,
		options.jsonOutput ? new JsonOutputChannel(options.outFd) : new OutputChannel(options.outFd)
	);

	return mock.run();
}
