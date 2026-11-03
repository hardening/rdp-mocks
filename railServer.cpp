/**
 * rdpmocks: remote applications (rail) support of rdp-server-mock
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

#include "railServer.h"

#include <cstdlib>
#include <sstream>
#include <vector>

#include <winpr/string.h>
#include <winpr/synch.h>

#include <freerdp/freerdp.h>
#include <freerdp/log.h>
#include <freerdp/window.h>

#include "commandChannel.h"
#include "railCommon.h"

#define TAG "rdpmocks.rail"

/* MS-RDPERP 2.2.1.1.2 Window List Capability Set: TS_WINDOW_LEVEL_SUPPORTED_EX, the
 * WINDOW_LEVEL_* defines are private to libfreerdp */
#define RAIL_WINDOW_LEVEL_SUPPORTED_EX 0x00000002

/* the build number announced in the server handshake (Windows 7) */
#define RAIL_SERVER_BUILD_NUMBER 7601

RailServer::RailServer(OutputChannel *output)
: output_(output)
, enabled_(false)
, monitor_(false)
, execResult_(RAIL_EXEC_S_OK)
, context_(nullptr)
, rdpContext_(nullptr)
, started_(false)
, clientReadyEvent_(nullptr)
, ready_(false) {}

RailServer::~RailServer() {
	destroy();
}

void RailServer::setEnabled(bool enabled) {
	enabled_ = enabled;
}

bool RailServer::isEnabled() const {
	return enabled_;
}

bool RailServer::applySettings(rdpSettings *settings) const {
	/* the remote programs capability set masks railSupportLevel with this, and the client only
	 * enables remote applications when RAIL_LEVEL_SUPPORTED remains */
	UINT32 supportLevel =
		enabled_ ? freerdp_settings_get_uint32(settings, FreeRDP_RemoteApplicationSupportMask) : 0;
	return freerdp_settings_set_uint32(
			   settings, FreeRDP_RemoteApplicationSupportLevel, supportLevel) &&
		freerdp_settings_set_uint32(
			settings, FreeRDP_RemoteWndSupportLevel, RAIL_WINDOW_LEVEL_SUPPORTED_EX);
}

void RailServer::setMonitor(bool monitor) {
	monitor_ = monitor;
}

void RailServer::notify(const std::string &msg) {
	if (monitor_)
		output_->sendNotification("rail", msg);
}

bool RailServer::create(HANDLE vcm, rdpContext *context) {
	context_ = rail_server_context_new(vcm);
	if (!context_)
		return false;

	clientReadyEvent_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	if (!clientReadyEvent_) {
		destroy();
		return false;
	}

	rdpContext_ = context;
	context_->rdpcontext = context;
	context_->custom = this;
	context_->ClientHandshake = _client_handshake;
	context_->ClientClientStatus = _client_status;
	context_->ClientExec = _client_exec;
	context_->ClientActivate = _client_activate;
	context_->ClientSyscommand = _client_syscommand;
	context_->ClientWindowMove = _client_window_move;
	context_->ClientSysparam = _client_sysparam;
	return true;
}

bool RailServer::needsStart() const {
	return context_ && !started_;
}

UINT RailServer::start() {
	UINT rc = context_->Start(context_);
	if (rc != CHANNEL_RC_OK)
		return rc;
	started_ = true;

	/* the server initiates the rail handshake, the client answers with its own handshake and its
	 * Client Information PDU, see _client_status() */
	RAIL_HANDSHAKE_ORDER handshake = {};
	handshake.buildNumber = RAIL_SERVER_BUILD_NUMBER;
	return context_->ServerHandshake(context_, &handshake);
}

HANDLE RailServer::getEventHandle() const {
	return started_ ? clientReadyEvent_ : nullptr;
}

void RailServer::onEvent() {
	ResetEvent(clientReadyEvent_);
	if (ready_)
		return;

	/* the remote application integration activation: the client starts the remote application
	 * (Client Execute PDU) on ARC_COMPLETED */
	rdpUpdate *update = rdpContext_->update;
	MONITORED_DESKTOP_ORDER monitoredDesktop = {};
	WINDOW_ORDER_INFO began = {};
	began.fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_HOOKED |
		WINDOW_ORDER_FIELD_DESKTOP_ARC_BEGAN;
	WINDOW_ORDER_INFO completed = {};
	completed.fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_ARC_COMPLETED;

	if (!update->BeginPaint(rdpContext_) ||
		!update->window->MonitoredDesktop(rdpContext_, &began, &monitoredDesktop) ||
		!update->window->MonitoredDesktop(rdpContext_, &completed, &monitoredDesktop) ||
		!update->EndPaint(rdpContext_)) {
		WLog_ERR(TAG, "unable to send the monitored desktop orders");
		return;
	}

	ready_ = true;
	notify("ready");
}

void RailServer::destroy() {
	if (context_) {
		if (started_)
			context_->Stop(context_);
		rail_server_context_free(context_);
		context_ = nullptr;
	}
	if (clientReadyEvent_) {
		CloseHandle(clientReadyEvent_);
		clientReadyEvent_ = nullptr;
	}
	rdpContext_ = nullptr;
	started_ = false;
	ready_ = false;
	windows_.clear();
}

UINT RailServer::_client_handshake(RailServerContext *context, const RAIL_HANDSHAKE_ORDER *pdu) {
	return CHANNEL_RC_OK;
}

UINT RailServer::_client_status(RailServerContext *context, const RAIL_CLIENT_STATUS_ORDER *pdu) {
	/* window orders go through the main connection, let the main thread send them */
	RailServer *rail = (RailServer *)context->custom;
	SetEvent(rail->clientReadyEvent_);
	return CHANNEL_RC_OK;
}

UINT RailServer::_client_exec(RailServerContext *context, const RAIL_EXEC_ORDER *pdu) {
	RailServer *rail = (RailServer *)context->custom;
	std::string program = pdu->RemoteApplicationProgram ? pdu->RemoteApplicationProgram : "";

	/* one notification per field, their values may contain spaces */
	rail->notify("exec program=" + program);
	rail->notify(std::string("exec args=") +
		(pdu->RemoteApplicationArguments ? pdu->RemoteApplicationArguments : ""));
	rail->notify(std::string("exec workdir=") +
		(pdu->RemoteApplicationWorkingDir ? pdu->RemoteApplicationWorkingDir : ""));

	/* ExeOrFile must hold between 1 and 260 characters */
	std::string exeOrFile = program.empty() ? " " : program.substr(0, 260);
	size_t wlen = 0;
	WCHAR *wexeOrFile = ConvertUtf8ToWCharAlloc(exeOrFile.c_str(), &wlen);
	if (!wexeOrFile)
		return CHANNEL_RC_NO_MEMORY;

	RAIL_EXEC_RESULT_ORDER result = {};
	result.flags = pdu->flags;
	result.execResult = rail->execResult_;
	result.exeOrFile.string = wexeOrFile;
	result.exeOrFile.length = (UINT16)(wlen * sizeof(WCHAR));
	UINT rc = context->ServerExecResult(context, &result);
	free(wexeOrFile);
	return rc;
}

UINT RailServer::_client_activate(RailServerContext *context, const RAIL_ACTIVATE_ORDER *pdu) {
	RailServer *rail = (RailServer *)context->custom;
	rail->notify(
		"activate id=" + std::to_string(pdu->windowId) + " enabled=" + (pdu->enabled ? "1" : "0"));
	return CHANNEL_RC_OK;
}

UINT RailServer::_client_syscommand(RailServerContext *context, const RAIL_SYSCOMMAND_ORDER *pdu) {
	RailServer *rail = (RailServer *)context->custom;
	rail->notify("syscommand id=" + std::to_string(pdu->windowId) +
		" command=" + railSyscommandName(pdu->command));
	return CHANNEL_RC_OK;
}

UINT RailServer::_client_window_move(
	RailServerContext *context, const RAIL_WINDOW_MOVE_ORDER *pdu) {
	RailServer *rail = (RailServer *)context->custom;
	rail->notify("windowmove id=" + std::to_string(pdu->windowId) +
		" x=" + std::to_string(pdu->left) + " y=" + std::to_string(pdu->top) + " w=" +
		std::to_string(pdu->right - pdu->left) + " h=" + std::to_string(pdu->bottom - pdu->top));
	return CHANNEL_RC_OK;
}

UINT RailServer::_client_sysparam(RailServerContext *context, const RAIL_SYSPARAM_ORDER *pdu) {
	RailServer *rail = (RailServer *)context->custom;
	if (pdu->params & SPI_MASK_SET_WORK_AREA) {
		const RECTANGLE_16 &area = pdu->workArea;
		rail->notify("sysparam workarea x=" + std::to_string(area.left) +
			" y=" + std::to_string(area.top) + " w=" + std::to_string(area.right - area.left) +
			" h=" + std::to_string(area.bottom - area.top));
	} else {
		char param[16];
		snprintf(param, sizeof(param), "0x%08x", pdu->param);
		rail->notify(std::string("sysparam param=") + param);
	}
	return CHANNEL_RC_OK;
}

bool RailServer::handleCommand(const std::string &cmd, const std::string &args) {
	if (cmd == "rail_window")
		windowCommand(args);
	else if (cmd == "rail_window_delete")
		windowDeleteCommand(args);
	else if (cmd == "rail_exec_result")
		execResultCommand(args);
	else if (cmd == "rail_localmovesize")
		localMoveSizeCommand(args);
	else if (cmd == "rail_minmax")
		minMaxCommand(args);
	else
		return false;
	return true;
}

void RailServer::windowCommand(const std::string &args) {
	if (!ready_) {
		output_->sendResult(false, "rail not ready");
		return;
	}

	/* <id> <x> <y> <w> <h> [<title>], the title being the rest of the line */
	std::vector<std::string> argv;
	if (!splitArgs(args, argv, 6) || argv.size() < 5) {
		output_->sendResult(false, "invalid argument");
		return;
	}
	std::istringstream stream(argv[1] + " " + argv[2] + " " + argv[3] + " " + argv[4]);
	INT32 x = 0;
	INT32 y = 0;
	UINT32 width = 0;
	UINT32 height = 0;
	UINT32 windowId = 0;
	if (!(stream >> x >> y >> width >> height) || !railParseWindowId(argv[0], windowId) || !width ||
		!height || width > UINT16_MAX || height > UINT16_MAX) {
		output_->sendResult(false, "invalid argument");
		return;
	}
	std::string title = (argv.size() > 5) ? argv[5] : "";

	bool create = windows_.find(windowId) == windows_.end();

	WINDOW_ORDER_INFO orderInfo = {};
	orderInfo.windowId = windowId;
	orderInfo.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_FIELD_WND_OFFSET |
		WINDOW_ORDER_FIELD_WND_SIZE | WINDOW_ORDER_FIELD_CLIENT_AREA_OFFSET |
		WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE | WINDOW_ORDER_FIELD_WND_RECTS |
		WINDOW_ORDER_FIELD_VIS_OFFSET | WINDOW_ORDER_FIELD_VISIBILITY;
	if (create)
		orderInfo.fieldFlags |= WINDOW_ORDER_STATE_NEW | WINDOW_ORDER_FIELD_OWNER |
			WINDOW_ORDER_FIELD_STYLE | WINDOW_ORDER_FIELD_SHOW;

	/* a single rectangle covering the whole window, relative to its origin */
	RECTANGLE_16 rect = { 0, 0, (UINT16)width, (UINT16)height };

	WINDOW_STATE_ORDER windowState = {};
	windowState.ownerWindowId = 0;
	windowState.style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
	windowState.extendedStyle = WS_EX_APPWINDOW;
	windowState.showState = WINDOW_SHOW;
	windowState.windowOffsetX = x;
	windowState.windowOffsetY = y;
	windowState.windowWidth = width;
	windowState.windowHeight = height;
	windowState.clientOffsetX = x;
	windowState.clientOffsetY = y;
	windowState.clientAreaWidth = width;
	windowState.clientAreaHeight = height;
	windowState.numWindowRects = 1;
	windowState.windowRects = &rect;
	windowState.visibleOffsetX = x;
	windowState.visibleOffsetY = y;
	windowState.numVisibilityRects = 1;
	windowState.visibilityRects = &rect;

	WCHAR *wtitle = nullptr;
	if (!title.empty()) {
		size_t wlen = 0;
		wtitle = ConvertUtf8ToWCharAlloc(title.c_str(), &wlen);
		if (!wtitle || wlen * sizeof(WCHAR) > UINT16_MAX) {
			free(wtitle);
			output_->sendResult(false, "invalid argument");
			return;
		}
		orderInfo.fieldFlags |= WINDOW_ORDER_FIELD_TITLE;
		windowState.titleInfo.string = wtitle;
		windowState.titleInfo.length = (UINT16)(wlen * sizeof(WCHAR));
	}

	rdpUpdate *update = rdpContext_->update;
	bool sent = update->BeginPaint(rdpContext_) &&
		(create ? update->window->WindowCreate(rdpContext_, &orderInfo, &windowState)
				: update->window->WindowUpdate(rdpContext_, &orderInfo, &windowState)) &&
		update->EndPaint(rdpContext_);
	free(wtitle);

	if (sent)
		windows_.insert(windowId);
	output_->sendResult(sent, sent ? "" : "send failed");
}

void RailServer::windowDeleteCommand(const std::string &args) {
	if (!ready_) {
		output_->sendResult(false, "rail not ready");
		return;
	}

	UINT32 windowId = 0;
	if (!railParseWindowId(args, windowId)) {
		output_->sendResult(false, "invalid argument");
		return;
	}
	if (windows_.find(windowId) == windows_.end()) {
		output_->sendResult(false, "unknown window");
		return;
	}

	WINDOW_ORDER_INFO orderInfo = {};
	orderInfo.windowId = windowId;
	orderInfo.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_STATE_DELETED;

	rdpUpdate *update = rdpContext_->update;
	bool sent = update->BeginPaint(rdpContext_) &&
		update->window->WindowDelete(rdpContext_, &orderInfo) && update->EndPaint(rdpContext_);
	if (sent)
		windows_.erase(windowId);
	output_->sendResult(sent, sent ? "" : "send failed");
}

void RailServer::execResultCommand(const std::string &args) {
	UINT16 execResult = 0;
	if (!railExecResultFromName(args, execResult)) {
		output_->sendResult(false, "invalid argument");
		return;
	}
	execResult_ = execResult;
	output_->sendResult(true);
}

namespace {

/** parses @p count INT16 values from @p argv[@p first] */
bool parseInt16s(const std::vector<std::string> &argv, size_t first, size_t count, INT16 *out) {
	if (argv.size() != first + count)
		return false;
	for (size_t i = 0; i < count; i++) {
		std::istringstream stream(argv[first + i]);
		INT32 value = 0;
		std::string trailing;
		if (!(stream >> value) || (stream >> trailing) || value < INT16_MIN || value > INT16_MAX)
			return false;
		out[i] = (INT16)value;
	}
	return true;
}

} // namespace

void RailServer::localMoveSizeCommand(const std::string &args) {
	if (!ready_) {
		output_->sendResult(false, "rail not ready");
		return;
	}

	/* <id> start|end <type> <x> <y> */
	std::vector<std::string> argv;
	RAIL_LOCALMOVESIZE_ORDER localMoveSize = {};
	INT16 pos[2] = {};
	if (!splitArgs(args, argv) || argv.size() != 5 ||
		!railParseWindowId(argv[0], localMoveSize.windowId) ||
		(argv[1] != "start" && argv[1] != "end") ||
		!railMoveSizeTypeFromName(argv[2], localMoveSize.moveSizeType) ||
		!parseInt16s(argv, 3, 2, pos)) {
		output_->sendResult(false, "invalid argument");
		return;
	}
	localMoveSize.isMoveSizeStart = (argv[1] == "start");
	localMoveSize.posX = pos[0];
	localMoveSize.posY = pos[1];

	bool sent = context_->ServerLocalMoveSize(context_, &localMoveSize) == CHANNEL_RC_OK;
	output_->sendResult(sent, sent ? "" : "send failed");
}

void RailServer::minMaxCommand(const std::string &args) {
	if (!ready_) {
		output_->sendResult(false, "rail not ready");
		return;
	}

	/* <id> <maxW> <maxH> <maxX> <maxY> <minTrackW> <minTrackH> <maxTrackW> <maxTrackH> */
	std::vector<std::string> argv;
	RAIL_MINMAXINFO_ORDER minMax = {};
	INT16 values[8] = {};
	if (!splitArgs(args, argv) || argv.empty() || !railParseWindowId(argv[0], minMax.windowId) ||
		!parseInt16s(argv, 1, 8, values)) {
		output_->sendResult(false, "invalid argument");
		return;
	}
	minMax.maxWidth = values[0];
	minMax.maxHeight = values[1];
	minMax.maxPosX = values[2];
	minMax.maxPosY = values[3];
	minMax.minTrackWidth = values[4];
	minMax.minTrackHeight = values[5];
	minMax.maxTrackWidth = values[6];
	minMax.maxTrackHeight = values[7];

	bool sent = context_->ServerMinMaxInfo(context_, &minMax) == CHANNEL_RC_OK;
	output_->sendResult(sent, sent ? "" : "send failed");
}
