/**
 * rdpmocks: keyboard and mouse input of rdp-client-mock
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

#include <string>
#include <vector>

#include <freerdp/freerdp.h>

#include "inputCommon.h"
#include "outputChannel.h"

/** @brief the mouse* and key* commands of rdp-client-mock, and the keyboard indicators sent by
 * the server. Everything runs on the main thread. */
class InputClient {
public:
	InputClient(OutputChannel *output);

	/** toggles the "keyboard" notifications kind of the monitor command */
	void setMonitor(bool monitor);

	/** the server sent the keyboard indicators (Set Keyboard Indicators PDU) */
	void onKeyboardIndicators(UINT16 ledFlags);

	/** treats the mouse* and key* commands, @p context being null when not connected
	 * @return false if @p cmd isn't one of them */
	bool handleCommand(const std::string &cmd, const std::string &args, rdpContext *context);

	/** sets the position used by the next mouse commands giving none (e.g. after a local move) */
	void setPosition(UINT16 x, UINT16 y);

protected:
	/** the result of a command, empty on success */
	std::string mouseCommand(const std::string &cmd, const std::string &args, rdpContext *context);
	std::string keyCommand(const std::string &cmd, const std::string &args, rdpContext *context);

	/** parses optional trailing <x> <y> arguments starting at @p argv[@p first] into the
	 * current position */
	bool parsePosition(const std::vector<std::string> &argv, size_t first);
	bool sendKey(rdpInput *input, const KeySpec &key, bool down);

	OutputChannel *output_;
	bool monitor_;

	/** the last mouse position, used when a command gives none */
	UINT16 x_;
	UINT16 y_;
};
