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
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <winpr/tools/makecert.h>

#include <Magick++.h>

#include "rdp-server-mock.h"
#include "mockOptions.h"

#define TAG "rdp-server-mock"

/* raw (uncompressed) BITMAP_DATA rectangles are wire-limited to a UINT16 byte length, so a
 * loaded image is always split into tiles no larger than this before being sent */
#define BITMAP_UPDATE_TILE_SIZE 64

static BOOL sendBitmapUpdate(freerdp_peer *peer, const std::string &path, UINT32 x, UINT32 y) {
	Magick::Image image;
	try {
		image.read(path);
	} catch (Magick::Exception &e) {
		WLog_ERR(TAG, "unable to load image %s: %s", path.c_str(), e.what());
		return FALSE;
	}

	const UINT32 width = (UINT32)image.columns();
	const UINT32 height = (UINT32)image.rows();
	if (!width || !height) {
		WLog_ERR(TAG, "invalid image dimensions for %s", path.c_str());
		return FALSE;
	}

	/* BGRA/32bpp top-down, matching gdi_get_pixel_format(32) == PIXEL_FORMAT_BGRA32 */
	std::vector<BYTE> pixels((size_t)width * height * 4);
	try {
		image.write(0, 0, width, height, "BGRA", Magick::CharPixel, pixels.data());
	} catch (Magick::Exception &e) {
		WLog_ERR(TAG, "unable to extract pixels from %s: %s", path.c_str(), e.what());
		return FALSE;
	}

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
				const BYTE *srcPtr = pixels.data() + ((size_t)srcRow * width + tileLeft) * 4;
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

	/* a single BITMAP_UPDATE fast path PDU can't exceed the client's advertised
	 * MultifragMaxRequestSize, so tiles are sent in as few PDUs as fit rather than all at once
	 * (which for any reasonably sized image easily overflows a single PDU) */
	UINT32 maxRequestSize =
		freerdp_settings_get_uint32(peer->context->settings, FreeRDP_MultifragMaxRequestSize);
	if (!maxRequestSize)
		maxRequestSize = BITMAP_UPDATE_TILE_SIZE * BITMAP_UPDATE_TILE_SIZE * 4;
	/* leave headroom for the PDU/order headers around each tile's raw pixel payload */
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
, lastReachedState_((CONNECTION_STATE)-1) {
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

	mock->peer_ = client;
	return TRUE;
}

BOOL RdpServerMock::_peer_post_connect(freerdp_peer *client) {
	RdpServerMockContext *context = (RdpServerMockContext *)client->context;
	RdpServerMock *mock = context->mock_;
	mock->connectionEstablished_ = true;
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

		if (pollCmd) {
			if (WaitForSingleObject(hcmd, 0) == WAIT_OBJECT_0) {
				switch (commandChannel_.pollFd()) {
				case CommandChannel::POLL_SUCCESS:
					break;
				case CommandChannel::POLL_ERROR:
				case CommandChannel::POLL_CLOSED:
					doRun_ = FALSE;
					break;
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

	for (const auto &entry : stringSettingCommands) {
		if (cmd == entry.cmd) {
			if (entry.check && !entry.check(args))
				return TREAT_ERROR;
			return toTreatResult(
				freerdp_settings_set_string(settings, entry.settingId, args.c_str()));
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
			return TREAT_ERROR;

	} else if (cmd == "redirect") {
		if (args.empty()) {
			WLog_ERR(TAG, "redirect needs a target host");
			return TREAT_ERROR;
		}
		mock_->pendingRedirectHost_ = args;

	} else if (cmd == "bitmap_update") {
		if (!mock_->peer_ || !mock_->connectionEstablished_) {
			WLog_ERR(TAG, "bitmap_update needs an active connection");
			return TREAT_ERROR;
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

		return toTreatResult(sendBitmapUpdate(mock_->peer_, path, x, y));

	} else if (cmd == "pause") {
		UINT32 delay = (UINT32)strtoul(args.c_str(), nullptr, 10);
		if (delay && delay < 200 * 1000)
			mock_->pollCmdChannelStartDate_ = GetTickCount64() + delay;
		return TREAT_SKIP;

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
		};

		for (const auto &entry : monitoredItems) {
			if (args == entry.item) {
				*entry.target = true;
				return TREAT_SUCCESS;
			}
		}

		if (args == "off") {
			for (const auto &entry : monitoredItems) {
				*entry.target = false;
			}
		} else {
			WLog_ERR(TAG, "unknown monitor kind");
			return TREAT_ERROR;
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

	RdpServerMock mock(options.cmdFd,
		options.jsonOutput ? new JsonOutputChannel(options.outFd)
						   : new OutputChannel(options.outFd));

	return mock.run();
}
