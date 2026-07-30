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
