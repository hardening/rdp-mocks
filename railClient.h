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

#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <freerdp/channels/rdpgfx.h>
#include <freerdp/client/rail.h>
#include <freerdp/settings.h>
#include <freerdp/window.h>

#include "inputClient.h"
#include "outputChannel.h"

/** @brief the rail channel and window orders of rdp-client-mock, and the rail_* commands.
 *
 * The rail addin treats the server PDUs on its own thread while commands and window orders are
 * treated on the main thread, so the state is protected by a mutex. The local moves started by the
 * server are queued on the rail thread and performed on the main thread (input events), which is
 * signalled through getEventHandle(). */
class RailClient {
public:
	RailClient(OutputChannel *output);
	~RailClient();

	/** toggles the "rail" notifications kind of the monitor command */
	void setMonitor(bool monitor);

	/** the rail channel connected / disconnected */
	void attach(RailClientContext *context);
	void detach();

	/** window orders received on the main connection */
	void onWindowOrder(
		bool create, const WINDOW_ORDER_INFO *orderInfo, const WINDOW_STATE_ORDER *windowState);
	void onWindowDelete(const WINDOW_ORDER_INFO *orderInfo);
	void onMonitoredDesktop(const WINDOW_ORDER_INFO *orderInfo, rdpSettings *settings);

	/** the server mapped an EGFX surface to a window (MapSurfaceToWindow / MapSurfaceToScaledWindow
	 * PDUs), called from the gfx channel thread */
	void onMapSurfaceToWindow(const RDPGFX_MAP_SURFACE_TO_WINDOW_PDU *pdu);
	void onMapSurfaceToScaledWindow(const RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU *pdu);

	/** treats the rail_* commands, returns false if @p cmd isn't one of them */
	bool handleCommand(const std::string &cmd, const std::string &args);

	/** signalled when local moves are to be performed, see onEvent() */
	HANDLE getEventHandle() const;
	/** to call from the main thread when getEventHandle() is signalled: performs the queued
	 * local moves, the final pointer position becoming the one of @p input */
	void onEvent(rdpContext *context, InputClient &input);

protected:
	/** a window as described by the window orders */
	struct Window {
		bool hasOffset = false;
		INT32 x = 0;
		INT32 y = 0;
		bool hasSize = false;
		UINT32 width = 0;
		UINT32 height = 0;
		/** the track size limits of the Min Max Info PDU, 0 when unknown */
		INT32 minTrackWidth = 0;
		INT32 minTrackHeight = 0;
		INT32 maxTrackWidth = 0;
		INT32 maxTrackHeight = 0;
	};

	static UINT _server_local_move_size(
		RailClientContext *context, const RAIL_LOCALMOVESIZE_ORDER *pdu);
	static UINT _server_min_max_info(RailClientContext *context, const RAIL_MINMAXINFO_ORDER *pdu);

	static UINT _server_handshake(RailClientContext *context, const RAIL_HANDSHAKE_ORDER *pdu);
	static UINT _server_handshake_ex(
		RailClientContext *context, const RAIL_HANDSHAKE_EX_ORDER *pdu);
	static UINT _server_execute_result(
		RailClientContext *context, const RAIL_EXEC_RESULT_ORDER *pdu);

	void onHandshake();
	void onExecuteResult(const RAIL_EXEC_RESULT_ORDER *pdu);
	void notify(const std::string &msg);

	void execCommand(const std::string &args);
	void syscommandCommand(const std::string &args);
	void activateCommand(const std::string &args);
	void windowMoveCommand(const std::string &args);
	void sysparamCommand(const std::string &args);
	void localMoveCommand(const std::string &args);

	/** performs a local move: applies the delta to the window and sends the Window Move PDU and
	 * the button release, called with the mutex held */
	void localMove(rdpContext *context, InputClient &input, const RAIL_LOCALMOVESIZE_ORDER &pdu);

	std::mutex mutex_;
	OutputChannel *output_;
	std::atomic<bool> monitor_;

	RailClientContext *context_;
	pcRailServerHandshake defaultServerHandshake_;
	pcRailServerHandshakeEx defaultServerHandshakeEx_;

	/** the handshake was exchanged, the channel can be used */
	bool ready_;
	/** the initial program (remoteApp) was executed */
	bool startSent_;
	/** the result of the initial program execution is still to come */
	bool initialExecPending_;

	/** the single rail_exec waiting for its Execute Result */
	bool execPending_;
	std::string pendingProgram_;

	std::map<UINT32, Window> windows_;

	/** the local move mode (rail_localmove): the delta applied to the moves started by the
	 * server */
	bool localMoveEnabled_;
	INT32 localMoveDx_;
	INT32 localMoveDy_;
	/** the local moves started by the server, to be performed on the main thread */
	std::vector<RAIL_LOCALMOVESIZE_ORDER> pendingLocalMoves_;
	HANDLE localMoveEvent_;
};
