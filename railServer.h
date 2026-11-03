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

#pragma once

#include <atomic>
#include <set>
#include <string>

#include <freerdp/server/rail.h>
#include <freerdp/settings.h>

#include "outputChannel.h"

/** @brief the rail channel and window orders of rdp-server-mock, and the rail_* commands.
 *
 * The rail server channel treats the client PDUs on its own thread: they're only reported and
 * answered there, everything sent on the main connection (window orders) is done from the main
 * thread, which is signalled through getEventHandle() once the client is ready. */
class RailServer {
public:
	RailServer(OutputChannel *output);
	~RailServer();

	/** the "rail on|off" command: rail is only proposed to the client when enabled */
	void setEnabled(bool enabled);
	bool isEnabled() const;

	/** sets what is advertised in the remote programs and window list capability sets */
	bool applySettings(rdpSettings *settings) const;

	/** toggles the "rail" notifications kind of the monitor command */
	void setMonitor(bool monitor);

	/** creates the channel context for a peer that asked for remote applications */
	bool create(HANDLE vcm, rdpContext *context);
	bool needsStart() const;
	/** the client joined the rail channel: starts it and sends the server handshake */
	UINT start();
	/** signalled when the client finished its part of the handshake, nullptr if not started */
	HANDLE getEventHandle() const;
	/** to call from the main thread when getEventHandle() is signalled */
	void onEvent();
	void destroy();

	/** treats the rail_* commands, returns false if @p cmd isn't one of them */
	bool handleCommand(const std::string &cmd, const std::string &args);

protected:
	static UINT _client_handshake(RailServerContext *context, const RAIL_HANDSHAKE_ORDER *pdu);
	static UINT _client_status(RailServerContext *context, const RAIL_CLIENT_STATUS_ORDER *pdu);
	static UINT _client_exec(RailServerContext *context, const RAIL_EXEC_ORDER *pdu);
	static UINT _client_activate(RailServerContext *context, const RAIL_ACTIVATE_ORDER *pdu);
	static UINT _client_syscommand(RailServerContext *context, const RAIL_SYSCOMMAND_ORDER *pdu);
	static UINT _client_window_move(RailServerContext *context, const RAIL_WINDOW_MOVE_ORDER *pdu);
	static UINT _client_sysparam(RailServerContext *context, const RAIL_SYSPARAM_ORDER *pdu);

	void notify(const std::string &msg);
	void windowCommand(const std::string &args);
	void windowDeleteCommand(const std::string &args);
	void execResultCommand(const std::string &args);
	void localMoveSizeCommand(const std::string &args);
	void minMaxCommand(const std::string &args);

	OutputChannel *output_;
	std::atomic<bool> enabled_;
	std::atomic<bool> monitor_;
	/** the result answered to the client's Execute PDUs, see rail_exec_result */
	std::atomic<UINT16> execResult_;

	RailServerContext *context_;
	rdpContext *rdpContext_;
	bool started_;
	HANDLE clientReadyEvent_;

	/** the remote application integration is activated, windows can be sent (main thread) */
	bool ready_;
	std::set<UINT32> windows_;
};
