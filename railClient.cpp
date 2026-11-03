/**
 * rdpmocks: remote applications (rail) support of rdp-client-mock
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

#include "railClient.h"

#include <algorithm>
#include <sstream>
#include <vector>

#include <freerdp/input.h>
#include <freerdp/log.h>

#include "commandChannel.h"
#include "railCommon.h"

#define TAG "rdpmocks.rail"

RailClient::RailClient(OutputChannel *output)
: output_(output)
, monitor_(false)
, context_(nullptr)
, defaultServerHandshake_(nullptr)
, defaultServerHandshakeEx_(nullptr)
, ready_(false)
, startSent_(false)
, initialExecPending_(false)
, execPending_(false)
, localMoveEnabled_(false)
, localMoveDx_(0)
, localMoveDy_(0)
, localMoveEvent_(CreateEvent(nullptr, TRUE, FALSE, nullptr)) {
	if (!localMoveEvent_)
		WLog_ERR(TAG, "unable to create the local move event");
}

RailClient::~RailClient() {
	if (localMoveEvent_)
		CloseHandle(localMoveEvent_);
}

HANDLE RailClient::getEventHandle() const {
	return localMoveEvent_;
}

void RailClient::setMonitor(bool monitor) {
	monitor_ = monitor;
}

void RailClient::notify(const std::string &msg) {
	if (monitor_)
		output_->sendNotification("rail", msg);
}

void RailClient::attach(RailClientContext *context) {
	std::lock_guard<std::mutex> lock(mutex_);

	context_ = context;
	context->custom = this;

	/* the addin's own handshake handlers answer the server (client handshake and status), keep
	 * them to chain to */
	defaultServerHandshake_ = context->ServerHandshake;
	defaultServerHandshakeEx_ = context->ServerHandshakeEx;
	context->ServerHandshake = _server_handshake;
	context->ServerHandshakeEx = _server_handshake_ex;
	context->ServerExecuteResult = _server_execute_result;
	context->ServerLocalMoveSize = _server_local_move_size;
	context->ServerMinMaxInfo = _server_min_max_info;
}

void RailClient::detach() {
	std::lock_guard<std::mutex> lock(mutex_);

	context_ = nullptr;
	ready_ = false;
	startSent_ = false;
	initialExecPending_ = false;
	windows_.clear();
	pendingLocalMoves_.clear();
	if (localMoveEvent_)
		ResetEvent(localMoveEvent_);
	if (execPending_) {
		execPending_ = false;
		output_->sendResult(false, "disconnected");
	}
}

UINT RailClient::_server_handshake(RailClientContext *context, const RAIL_HANDSHAKE_ORDER *pdu) {
	RailClient *rail = (RailClient *)context->custom;
	if (rail->defaultServerHandshake_) {
		UINT rc = rail->defaultServerHandshake_(context, pdu);
		if (rc != CHANNEL_RC_OK)
			return rc;
	}
	rail->onHandshake();
	return CHANNEL_RC_OK;
}

UINT RailClient::_server_handshake_ex(
	RailClientContext *context, const RAIL_HANDSHAKE_EX_ORDER *pdu) {
	RailClient *rail = (RailClient *)context->custom;
	if (rail->defaultServerHandshakeEx_) {
		UINT rc = rail->defaultServerHandshakeEx_(context, pdu);
		if (rc != CHANNEL_RC_OK)
			return rc;
	}
	rail->onHandshake();
	return CHANNEL_RC_OK;
}

void RailClient::onHandshake() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		ready_ = true;
	}
	notify("ready");
}

UINT RailClient::_server_execute_result(
	RailClientContext *context, const RAIL_EXEC_RESULT_ORDER *pdu) {
	((RailClient *)context->custom)->onExecuteResult(pdu);
	return CHANNEL_RC_OK;
}

void RailClient::onExecuteResult(const RAIL_EXEC_RESULT_ORDER *pdu) {
	std::string exeOrFile = railUnicodeStringToUtf8(pdu->exeOrFile);
	notify("execResult result=" + railExecResultName(pdu->execResult) + " exeOrFile=" + exeOrFile);

	std::lock_guard<std::mutex> lock(mutex_);

	/* the Execute Result PDU has no request id: a rail_exec is answered by the result naming its
	 * program, or by any result once the initial program's one was received */
	if (execPending_ && (exeOrFile == pendingProgram_ || !initialExecPending_)) {
		execPending_ = false;
		if (pdu->execResult == RAIL_EXEC_S_OK)
			output_->sendResult(true);
		else
			output_->sendResult(false, railExecResultName(pdu->execResult));
	} else {
		initialExecPending_ = false;
	}
}

UINT RailClient::_server_local_move_size(
	RailClientContext *context, const RAIL_LOCALMOVESIZE_ORDER *pdu) {
	RailClient *rail = (RailClient *)context->custom;
	rail->notify(std::string("localmovesize ") + (pdu->isMoveSizeStart ? "start" : "end") + " id=" +
		std::to_string(pdu->windowId) + " type=" + railMoveSizeTypeName(pdu->moveSizeType) +
		" x=" + std::to_string(pdu->posX) + " y=" + std::to_string(pdu->posY));

	if (!pdu->isMoveSizeStart)
		return CHANNEL_RC_OK;

	std::lock_guard<std::mutex> lock(rail->mutex_);
	if (!rail->localMoveEnabled_ || !rail->localMoveEvent_)
		return CHANNEL_RC_OK;

	/* the input events are sent from the main thread */
	rail->pendingLocalMoves_.push_back(*pdu);
	SetEvent(rail->localMoveEvent_);
	return CHANNEL_RC_OK;
}

UINT RailClient::_server_min_max_info(
	RailClientContext *context, const RAIL_MINMAXINFO_ORDER *pdu) {
	RailClient *rail = (RailClient *)context->custom;
	rail->notify("minmax id=" + std::to_string(pdu->windowId) +
		" maxW=" + std::to_string(pdu->maxWidth) + " maxH=" + std::to_string(pdu->maxHeight) +
		" maxX=" + std::to_string(pdu->maxPosX) + " maxY=" + std::to_string(pdu->maxPosY) +
		" minTrackW=" + std::to_string(pdu->minTrackWidth) + " minTrackH=" +
		std::to_string(pdu->minTrackHeight) + " maxTrackW=" + std::to_string(pdu->maxTrackWidth) +
		" maxTrackH=" + std::to_string(pdu->maxTrackHeight));

	/* the track sizes bound the sizes reached by the local moves */
	std::lock_guard<std::mutex> lock(rail->mutex_);
	Window &window = rail->windows_[pdu->windowId];
	window.minTrackWidth = pdu->minTrackWidth;
	window.minTrackHeight = pdu->minTrackHeight;
	window.maxTrackWidth = pdu->maxTrackWidth;
	window.maxTrackHeight = pdu->maxTrackHeight;
	return CHANNEL_RC_OK;
}

void RailClient::onEvent(rdpContext *context, InputClient &input) {
	std::lock_guard<std::mutex> lock(mutex_);

	ResetEvent(localMoveEvent_);
	std::vector<RAIL_LOCALMOVESIZE_ORDER> moves;
	moves.swap(pendingLocalMoves_);
	if (!context_ || !context)
		return;

	for (const auto &move : moves)
		localMove(context, input, move);
}

namespace {

/** the new length of a window edge pair, bounded by the track sizes (0 when unknown) */
INT32 boundLength(INT32 length, INT32 minTrack, INT32 maxTrack) {
	length = std::max(length, std::max(minTrack, 1));
	if (maxTrack > 0)
		length = std::min(length, std::max(maxTrack, 1));
	return length;
}

bool fitsInt16(INT32 value) {
	return value >= INT16_MIN && value <= INT16_MAX;
}

} // namespace

void RailClient::localMove(
	rdpContext *context, InputClient &input, const RAIL_LOCALMOVESIZE_ORDER &pdu) {
	const UINT16 type = pdu.moveSizeType;
	const bool keyboard = (type == RAIL_WMSZ_KEYMOVE) || (type == RAIL_WMSZ_KEYSIZE);
	const bool move = (type == RAIL_WMSZ_MOVE) || (type == RAIL_WMSZ_KEYMOVE);
	const bool sizeLeft =
		(type == RAIL_WMSZ_LEFT) || (type == RAIL_WMSZ_TOPLEFT) || (type == RAIL_WMSZ_BOTTOMLEFT);
	const bool sizeRight = (type == RAIL_WMSZ_RIGHT) || (type == RAIL_WMSZ_TOPRIGHT) ||
		(type == RAIL_WMSZ_BOTTOMRIGHT) || (type == RAIL_WMSZ_KEYSIZE);
	const bool sizeTop =
		(type == RAIL_WMSZ_TOP) || (type == RAIL_WMSZ_TOPLEFT) || (type == RAIL_WMSZ_TOPRIGHT);
	const bool sizeBottom = (type == RAIL_WMSZ_BOTTOM) || (type == RAIL_WMSZ_BOTTOMLEFT) ||
		(type == RAIL_WMSZ_BOTTOMRIGHT) || (type == RAIL_WMSZ_KEYSIZE);

	std::string msg = "localmove id=" + std::to_string(pdu.windowId);

	/* like xfreerdp: the window is moved/resized locally, then the server is told its new
	 * position (Window Move PDU) */
	auto it = windows_.find(pdu.windowId);
	if (it != windows_.end() && it->second.hasOffset && it->second.hasSize) {
		Window &window = it->second;
		INT32 left = window.x;
		INT32 top = window.y;
		INT32 right = window.x + (INT32)window.width;
		INT32 bottom = window.y + (INT32)window.height;

		if (move) {
			left += localMoveDx_;
			right += localMoveDx_;
			top += localMoveDy_;
			bottom += localMoveDy_;
		} else {
			if (sizeLeft)
				left += localMoveDx_;
			if (sizeRight)
				right += localMoveDx_;
			if (sizeTop)
				top += localMoveDy_;
			if (sizeBottom)
				bottom += localMoveDy_;

			INT32 width = boundLength(right - left, window.minTrackWidth, window.maxTrackWidth);
			INT32 height = boundLength(bottom - top, window.minTrackHeight, window.maxTrackHeight);
			if (sizeLeft)
				left = right - width;
			else
				right = left + width;
			if (sizeTop)
				top = bottom - height;
			else
				bottom = top + height;
		}

		if (fitsInt16(left) && fitsInt16(top) && fitsInt16(right) && fitsInt16(bottom)) {
			RAIL_WINDOW_MOVE_ORDER windowMove = {};
			windowMove.windowId = pdu.windowId;
			windowMove.left = (INT16)left;
			windowMove.top = (INT16)top;
			windowMove.right = (INT16)right;
			windowMove.bottom = (INT16)bottom;
			if (context_->ClientWindowMove(context_, &windowMove) != CHANNEL_RC_OK)
				WLog_ERR(TAG, "unable to send the window move of window %u", pdu.windowId);

			/* proactively, the server confirms with a window order */
			window.x = left;
			window.y = top;
			window.width = (UINT32)(right - left);
			window.height = (UINT32)(bottom - top);
			msg += " x=" + std::to_string(left) + " y=" + std::to_string(top) +
				" w=" + std::to_string(window.width) + " h=" + std::to_string(window.height);
		} else {
			WLog_ERR(TAG, "local move of window %u out of range", pdu.windowId);
		}
	}

	/* the server ends the move/size on the button release, at the final pointer position (not
	 * for the keyboard ones) */
	if (!keyboard) {
		UINT16 x = (UINT16)std::min(std::max((INT32)pdu.posX + localMoveDx_, 0), (INT32)UINT16_MAX);
		UINT16 y = (UINT16)std::min(std::max((INT32)pdu.posY + localMoveDy_, 0), (INT32)UINT16_MAX);
		input.setPosition(x, y);
		if (!freerdp_input_send_mouse_event(context->input, PTR_FLAGS_BUTTON1, x, y))
			WLog_ERR(TAG, "unable to send the button release of the local move");
		msg += " releaseX=" + std::to_string(x) + " releaseY=" + std::to_string(y);
	}

	notify(msg);
}

void RailClient::onWindowOrder(
	bool create, const WINDOW_ORDER_INFO *orderInfo, const WINDOW_STATE_ORDER *windowState) {
	{
		/* the geometry is kept for the local moves */
		std::lock_guard<std::mutex> lock(mutex_);
		Window &window = windows_[orderInfo->windowId];
		if (orderInfo->fieldFlags & WINDOW_ORDER_FIELD_WND_OFFSET) {
			window.hasOffset = true;
			window.x = windowState->windowOffsetX;
			window.y = windowState->windowOffsetY;
		}
		if (orderInfo->fieldFlags & WINDOW_ORDER_FIELD_WND_SIZE) {
			window.hasSize = true;
			window.width = windowState->windowWidth;
			window.height = windowState->windowHeight;
		}
	}

	if (!monitor_)
		return;

	std::string msg = std::string("window ") + (create ? "create" : "update") +
		" id=" + std::to_string(orderInfo->windowId);
	if (orderInfo->fieldFlags & WINDOW_ORDER_FIELD_WND_OFFSET)
		msg += " x=" + std::to_string(windowState->windowOffsetX) +
			" y=" + std::to_string(windowState->windowOffsetY);
	if (orderInfo->fieldFlags & WINDOW_ORDER_FIELD_WND_SIZE)
		msg += " w=" + std::to_string(windowState->windowWidth) +
			" h=" + std::to_string(windowState->windowHeight);
	/* last as it may contain spaces */
	if (orderInfo->fieldFlags & WINDOW_ORDER_FIELD_TITLE)
		msg += " title=" + railUnicodeStringToUtf8(windowState->titleInfo);
	notify(msg);
}

void RailClient::onWindowDelete(const WINDOW_ORDER_INFO *orderInfo) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		windows_.erase(orderInfo->windowId);
	}
	notify("window delete id=" + std::to_string(orderInfo->windowId));
}

void RailClient::onMonitoredDesktop(const WINDOW_ORDER_INFO *orderInfo, rdpSettings *settings) {
	if (!(orderInfo->fieldFlags & WINDOW_ORDER_FIELD_DESKTOP_ARC_COMPLETED))
		return;

	std::lock_guard<std::mutex> lock(mutex_);

	/* like FreeRDP's clients, the initial program is only executed once the server reported the
	 * remote application integration activation completed */
	const char *program = freerdp_settings_get_string(settings, FreeRDP_RemoteApplicationProgram);
	if (!context_ || startSent_ || !program || !*program)
		return;

	startSent_ = true;
	UINT rc = client_rail_server_start_cmd(context_);
	if (rc != CHANNEL_RC_OK)
		WLog_ERR(TAG, "unable to start the remote application, error %u", rc);
	else
		initialExecPending_ = true;
}

void RailClient::onMapSurfaceToWindow(const RDPGFX_MAP_SURFACE_TO_WINDOW_PDU *pdu) {
	notify("mapSurfaceToWindow surfaceId=" + std::to_string(pdu->surfaceId) + " windowId=" +
		std::to_string(pdu->windowId) + " mappedW=" + std::to_string(pdu->mappedWidth) +
		" mappedH=" + std::to_string(pdu->mappedHeight));
}

void RailClient::onMapSurfaceToScaledWindow(const RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU *pdu) {
	notify("mapSurfaceToScaledWindow surfaceId=" + std::to_string(pdu->surfaceId) + " windowId=" +
		std::to_string(pdu->windowId) + " mappedW=" + std::to_string(pdu->mappedWidth) +
		" mappedH=" + std::to_string(pdu->mappedHeight) + " targetW=" +
		std::to_string(pdu->targetWidth) + " targetH=" + std::to_string(pdu->targetHeight));
}

bool RailClient::handleCommand(const std::string &cmd, const std::string &args) {
	if (cmd == "rail_exec")
		execCommand(args);
	else if (cmd == "rail_syscommand")
		syscommandCommand(args);
	else if (cmd == "rail_activate")
		activateCommand(args);
	else if (cmd == "rail_window_move")
		windowMoveCommand(args);
	else if (cmd == "rail_sysparam")
		sysparamCommand(args);
	else if (cmd == "rail_localmove")
		localMoveCommand(args);
	else
		return false;
	return true;
}

void RailClient::execCommand(const std::string &args) {
	std::lock_guard<std::mutex> lock(mutex_);

	if (!context_ || !ready_) {
		output_->sendResult(false, "rail not ready");
		return;
	}
	if (execPending_) {
		output_->sendResult(false, "request pending");
		return;
	}

	/* <program> [<arguments>], the arguments being the rest of the line */
	std::vector<std::string> argv;
	if (!splitArgs(args, argv, 2)) {
		output_->sendResult(false, "invalid argument");
		return;
	}
	if (argv.empty() || argv[0].empty()) {
		output_->sendResult(false, "missing program");
		return;
	}
	std::string program = argv[0];
	std::string arguments = (argv.size() > 1) ? argv[1] : "";

	RAIL_EXEC_ORDER exec = {};
	exec.RemoteApplicationProgram = program.c_str();
	exec.RemoteApplicationArguments = arguments.c_str();
	exec.RemoteApplicationWorkingDir = "";
	if (context_->ClientExecute(context_, &exec) != CHANNEL_RC_OK) {
		output_->sendResult(false, "send failed");
		return;
	}

	/* the RESULT is emitted with the server's Execute Result, see onExecuteResult() */
	execPending_ = true;
	pendingProgram_ = program;
}

void RailClient::syscommandCommand(const std::string &args) {
	std::lock_guard<std::mutex> lock(mutex_);

	if (!context_ || !ready_) {
		output_->sendResult(false, "rail not ready");
		return;
	}

	size_t space = args.find(' ');
	RAIL_SYSCOMMAND_ORDER syscommand = {};
	if (space == std::string::npos ||
		!railParseWindowId(args.substr(0, space), syscommand.windowId) ||
		!railSyscommandFromName(args.substr(space + 1), syscommand.command)) {
		output_->sendResult(false, "invalid argument");
		return;
	}

	bool sent = context_->ClientSystemCommand(context_, &syscommand) == CHANNEL_RC_OK;
	output_->sendResult(sent, sent ? "" : "send failed");
}

void RailClient::activateCommand(const std::string &args) {
	std::lock_guard<std::mutex> lock(mutex_);

	if (!context_ || !ready_) {
		output_->sendResult(false, "rail not ready");
		return;
	}

	size_t space = args.find(' ');
	RAIL_ACTIVATE_ORDER activate = {};
	std::string state = (space == std::string::npos) ? "" : args.substr(space + 1);
	if (space == std::string::npos ||
		!railParseWindowId(args.substr(0, space), activate.windowId) ||
		(state != "on" && state != "off")) {
		output_->sendResult(false, "invalid argument");
		return;
	}
	activate.enabled = (state == "on");

	bool sent = context_->ClientActivate(context_, &activate) == CHANNEL_RC_OK;
	output_->sendResult(sent, sent ? "" : "send failed");
}

namespace {

/** parses <x> <y> <w> <h>, the size being non null */
bool parseRect(const std::vector<std::string> &argv, size_t first, INT32 &x, INT32 &y, INT32 &width,
	INT32 &height) {
	if (argv.size() != first + 4)
		return false;

	std::istringstream stream(
		argv[first] + " " + argv[first + 1] + " " + argv[first + 2] + " " + argv[first + 3]);
	std::string trailing;
	return (stream >> x >> y >> width >> height) && !(stream >> trailing) && width > 0 &&
		height > 0 && width <= UINT16_MAX && height <= UINT16_MAX && x >= INT16_MIN &&
		x <= UINT16_MAX && y >= INT16_MIN && y <= UINT16_MAX;
}

} // namespace

void RailClient::windowMoveCommand(const std::string &args) {
	std::lock_guard<std::mutex> lock(mutex_);

	if (!context_ || !ready_) {
		output_->sendResult(false, "rail not ready");
		return;
	}

	/* <id> <x> <y> <w> <h> */
	std::vector<std::string> argv;
	RAIL_WINDOW_MOVE_ORDER windowMove = {};
	INT32 x = 0;
	INT32 y = 0;
	INT32 width = 0;
	INT32 height = 0;
	if (!splitArgs(args, argv) || argv.empty() ||
		!railParseWindowId(argv[0], windowMove.windowId) ||
		!parseRect(argv, 1, x, y, width, height) || !fitsInt16(x) || !fitsInt16(y) ||
		!fitsInt16(x + width) || !fitsInt16(y + height)) {
		output_->sendResult(false, "invalid argument");
		return;
	}
	windowMove.left = (INT16)x;
	windowMove.top = (INT16)y;
	windowMove.right = (INT16)(x + width);
	windowMove.bottom = (INT16)(y + height);

	if (context_->ClientWindowMove(context_, &windowMove) != CHANNEL_RC_OK) {
		output_->sendResult(false, "send failed");
		return;
	}

	/* the window is now there locally */
	auto it = windows_.find(windowMove.windowId);
	if (it != windows_.end()) {
		it->second.hasOffset = it->second.hasSize = true;
		it->second.x = x;
		it->second.y = y;
		it->second.width = (UINT32)width;
		it->second.height = (UINT32)height;
	}
	output_->sendResult(true);
}

void RailClient::sysparamCommand(const std::string &args) {
	std::lock_guard<std::mutex> lock(mutex_);

	if (!context_ || !ready_) {
		output_->sendResult(false, "rail not ready");
		return;
	}

	/* workarea <x> <y> <w> <h> */
	std::vector<std::string> argv;
	INT32 x = 0;
	INT32 y = 0;
	INT32 width = 0;
	INT32 height = 0;
	if (!splitArgs(args, argv) || argv.empty() || argv[0] != "workarea" ||
		!parseRect(argv, 1, x, y, width, height) || x < 0 || y < 0 || x + width > UINT16_MAX ||
		y + height > UINT16_MAX) {
		output_->sendResult(false, "invalid argument");
		return;
	}

	RAIL_SYSPARAM_ORDER sysparam = {};
	sysparam.param = SPI_SET_WORK_AREA;
	sysparam.params = SPI_MASK_SET_WORK_AREA;
	sysparam.workArea.left = (UINT16)x;
	sysparam.workArea.top = (UINT16)y;
	sysparam.workArea.right = (UINT16)(x + width);
	sysparam.workArea.bottom = (UINT16)(y + height);

	bool sent = context_->ClientSystemParam(context_, &sysparam) == CHANNEL_RC_OK;
	output_->sendResult(sent, sent ? "" : "send failed");
}

void RailClient::localMoveCommand(const std::string &args) {
	std::lock_guard<std::mutex> lock(mutex_);

	/* off | <dx> <dy> */
	if (args == "off") {
		localMoveEnabled_ = false;
		output_->sendResult(true);
		return;
	}

	std::istringstream stream(args);
	INT32 dx = 0;
	INT32 dy = 0;
	std::string trailing;
	if (!(stream >> dx >> dy) || (stream >> trailing) || !fitsInt16(dx) || !fitsInt16(dy)) {
		output_->sendResult(false, "invalid argument");
		return;
	}

	localMoveEnabled_ = true;
	localMoveDx_ = dx;
	localMoveDy_ = dy;
	output_->sendResult(true);
}
