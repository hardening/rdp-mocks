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

#include <winpr/synch.h>
#include <winpr/sysinfo.h>
#include <winpr/path.h>

#include <freerdp/input.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <winpr/tools/makecert.h>

#include "rdp-server-mock.h"
#include "mockOptions.h"

#define TAG "rdp-server-mock"

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
, doRun_(true)
, commandChannel_(fd, this)
, peer_(nullptr)
, connectionEstablished_(false)
, pollCmdChannelStartDate_(0)
, lastReachedState_((CONNECTION_STATE)-1)
{
	settings_ = freerdp_settings_new(FREERDP_SETTINGS_SERVER_MODE);
}

RdpServerMock::~RdpServerMock() {
	if (peer_) {
		if (connectionEstablished_)
			peer_->Disconnect(peer_);
		freerdp_peer_context_free(peer_);
		freerdp_peer_free(peer_);
	}
	freerdp_settings_free(settings_);
	delete output_;
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
	RdpServerMock *mock = (RdpServerMock*)instance->info;
	if (!mock)
		return FALSE;

	client->ContextSize = sizeof(RdpServerMockContext);

	if (!freerdp_peer_context_new(client))
		return FALSE;

	((RdpServerMockContext*)client->context)->mock_ = mock;

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

	rdpInput* input = client->context->input;
	input->KeyboardEvent = _peer_keyboard_event;
	input->MouseEvent = _peer_mouse_event;

	mock->peer_ = client;
	return TRUE;
}

BOOL RdpServerMock::_peer_post_connect(freerdp_peer *client) {
	RdpServerMockContext *context = (RdpServerMockContext*)client->context;
	RdpServerMock *mock = context->mock_;
	mock->connectionEstablished_ = true;
	return TRUE;
}

BOOL RdpServerMock::_peer_activate(freerdp_peer *client) {


	return TRUE;
}

BOOL RdpServerMock::_peer_reached_state(freerdp_peer *client, CONNECTION_STATE state)
{
	RdpServerMockContext *context = (RdpServerMockContext*)client->context;
	RdpServerMock *mock = context->mock_;
	if (mock->monitorStates_ && state != mock->lastReachedState_) {
		mock->lastReachedState_ = state;
		mock->output_->sendNotification("states", freerdp_state_string(state));
	}
	return TRUE;
}

BOOL RdpServerMock::_peer_keyboard_event(rdpInput* input, UINT16 flags, UINT8 code)
{
	RdpServerMockContext *context = (RdpServerMockContext*)input->context;
	RdpServerMock *mock = context->mock_;
	if (mock->monitorKeyEvents_)
		mock->output_->sendNotification("key", "flags=" + std::to_string(flags) +
				" code=" + std::to_string(code));
	return TRUE;
}

BOOL RdpServerMock::_peer_mouse_event(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
	RdpServerMockContext *context = (RdpServerMockContext*)input->context;
	RdpServerMock *mock = context->mock_;
	if (mock->monitorMouseEvents_)
		mock->output_->sendNotification("mouse", "x=" + std::to_string(x) + " y=" +
				std::to_string(y) + " flags=" + std::to_string(flags));
	return TRUE;
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
		DWORD pollDelay;

		if (now > pollCmdChannelStartDate_) {
			pollCmd = true;
			handles[nhandles++] = hcmd;
			pollDelay = INFINITE;
		} else {
			pollCmd = false;
			pollDelay = (1 + pollCmdChannelStartDate_ - now);
		}

		if (peer_)
			nhandles += peer_->GetEventHandles(peer_, &handles[nhandles], MAXIMUM_WAIT_OBJECTS - nhandles);

		DWORD status = WAIT_TIMEOUT;
		if (nhandles)
			status = WaitForMultipleObjects(nhandles, handles, FALSE, pollDelay);
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

		if (peer_ && !peer_->CheckFileDescriptor(peer_)) {
			peer_->Disconnect(peer_);
			freerdp_peer_context_free(peer_);
			freerdp_peer_free(peer_);
			peer_ = nullptr;
			connectionEstablished_ = false;
			doRun_ = false;
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
, mock_(mock)
{
}

BOOL ServerCommandChannel::onCommand(const std::string &cmd, const std::string &args) {

	struct StringSettingCommand {
		const char *cmd;
		FreeRDP_Settings_Keys_String settingId;
		BOOL (*check)(const std::string &args);
	};

	static const StringSettingCommand stringSettingCommands[] = {
		{ "samFile", FreeRDP_NtlmSamFile, checkFileExists },
	};

	rdpSettings *settings = mock_->settings_;

	for (const auto &entry : stringSettingCommands) {
		if (cmd == entry.cmd) {
			if (entry.check && !entry.check(args))
				return FALSE;
			return freerdp_settings_set_string(settings, entry.settingId, args.c_str());
		}
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
	} else if (cmd == "cert") {
		return loadCertificateFile(settings, args.c_str());

	} else if (cmd == "key") {
		return loadPrivateKeyFile(settings, args.c_str());

	} else if (cmd == "autoCert") {
		char *makecertArgv[] = { (char*)"makecert", (char*)"-rdp", (char*)"-live", (char*)"-silent", (char*)"-y", (char*)"5" };

		MAKECERT_CONTEXT *makecert = makecert_context_new();
		if (!makecert)
			return FALSE;

		BOOL ok = makecert_context_process(makecert, ARRAYSIZE(makecertArgv), makecertArgv) >= 0 &&
			makecert_context_set_output_file_name(makecert, "rdp-server-mock") == 1 &&
			makecert_context_output_certificate_file(makecert, args.c_str()) == 1 &&
			makecert_context_output_private_key_file(makecert, args.c_str()) == 1;
		makecert_context_free(makecert);

		if (!ok) {
			WLog_ERR(TAG, "unable to generate certificate in %s", args.c_str());
			return FALSE;
		}

		return loadCertificateFile(settings, (args + "/rdp-server-mock.crt").c_str()) &&
			loadPrivateKeyFile(settings, (args + "/rdp-server-mock.key").c_str());

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
			return FALSE;
		}

		listener->info = mock_;
		listener->PeerAccepted = RdpServerMock::_peer_accepted;

		if (!listener->Open(listener, host.c_str(), (UINT16)port)) {
			mock_->output_->sendResult(false, "can't listen");
			WLog_ERR(TAG, "unable to listen on %s:%u", host.c_str(), port);
			freerdp_listener_free(listener);
			return FALSE;
		}

		mock_->output_->sendResult(true, "listen");

		/* block here until a connection is accepted */
		while (!mock_->peer_) {
			HANDLE handles[MAXIMUM_WAIT_OBJECTS];
			DWORD nhandles = listener->GetEventHandles(listener, handles, MAXIMUM_WAIT_OBJECTS);
			if (!nhandles) {
				WLog_ERR(TAG, "unable to get listener event handles");
				mock_->output_->sendResult(false, "can't get handles");
				break;
			}

			if (WaitForMultipleObjects(nhandles, handles, FALSE, INFINITE) == WAIT_FAILED)
				break;

			if (!listener->CheckFileDescriptor(listener)) {
				mock_->output_->sendResult(false);
				WLog_ERR(TAG, "listener CheckFileDescriptor failed");
				break;
			}
		}

		BOOL accepted = (mock_->peer_ != nullptr);

		listener->Close(listener);
		freerdp_listener_free(listener);

		mock_->output_->sendResult(accepted, "accepted");
		if (!accepted)
			return FALSE;

	} else if (cmd == "pause") {
		UINT32 delay = (UINT32)strtoul(args.c_str(), nullptr, 10);
		if (delay && delay < 100 * 1000)
			mock_->pollCmdChannelStartDate_ = GetTickCount64() + delay;


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
		};

		for (const auto &entry : monitoredItems) {
			if (args == entry.item) {
				*entry.target = true;
				return TRUE;
			}
		}

		if (args == "off") {
			for (const auto &entry : monitoredItems) {
				*entry.target = false;
			}
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

	RdpServerMock mock(options.cmdFd,
		options.jsonOutput ? new JsonOutputChannel(options.outFd) : new OutputChannel(options.outFd)
	);

	return mock.run();
}
