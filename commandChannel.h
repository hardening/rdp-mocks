/**
 * rdpmocks: line-based command channel shared by the mock RDP client and server
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

#include <freerdp/utils/ringbuffer.h>

/** @brief an abstract command channel */
class CommandChannel {
public:
	CommandChannel(int fd);
	virtual ~CommandChannel();

	/** @brief the result of a polling operation */
	enum PollResult {
		POLL_SUCCESS,
		POLL_ERROR,
		POLL_CLOSED
	};

	/** @brief the result of the treat() operation */
	enum TreatResult {
		TREAT_SUCCESS,
		TREAT_ERROR,
		TREAT_SKIP,
	};

	/** polls the file descriptor for new commands and fill the internal ringbuffer
	 * @return the PollResult of the operation
	 */
	PollResult pollFd();

	/** @return if some data are available internally */
	bool hasBufferData();

	/** treat the command buffer
	 * @return TREAT_ERROR if something wrong happened, TREAT_SKIP if treatment was paused, TREAT_SUCCESS otherwise
	 */
	PollResult treat();

	/**
	 * method to implement that is called when we have a command with its arguments
	 * @param cmd command string
	 * @param args command arguments
	 * @return a TreatResult that express the result of command's treatment
	 */
	virtual TreatResult onCommand(const std::string &cmd, const std::string &args) = 0;

	/** utility function to convert a bool either to TREAT_SUCCESS or TREAT_ERROR */
	static TreatResult toTreatResult(bool v);

protected:
	int fd_;
	bool closed_;
	RingBuffer buffer_;
};

/** @brief splits the arguments of a command on spaces.
 *
 * An argument starting with a single quote goes up to the closing quote and can contain spaces:
 * inside the quotes \' is a quote and \\ a backslash, any other backslash is kept as is (so
 * 'C:\Work Dir' is C:\Work Dir). Elsewhere quotes and backslashes have no special meaning.
 *
 * With @p maxArgs non-zero, the last argument is the rest of the line: taken as is, or unquoted
 * if it is a single quoted argument. With @p textEscapes, \n and \t are also new lines and tabs
 * inside the quotes.
 *
 * @return false if a quoted argument isn't terminated or is directly followed by something
 * else than a space, or if there are more than @p maxArgs arguments */
bool splitArgs(const std::string &args, std::vector<std::string> &out, size_t maxArgs = 0,
	bool textEscapes = false);

/** @brief the argument of a command taking the rest of the line (a path, a name, a text...):
 * a single quoted argument, or the rest of the line as is, see splitArgs()
 *
 * @return false on a malformed quoted argument */
bool unquoteArg(const std::string &args, std::string &out, bool textEscapes = false);
