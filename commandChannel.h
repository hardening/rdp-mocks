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

/** @brief */
class CommandChannel {
public:
	CommandChannel(int fd);
	virtual ~CommandChannel();

	enum PollResult {
		POLL_SUCCESS,
		POLL_ERROR,
		POLL_CLOSED
	};

	PollResult poll();
	virtual BOOL onCommand(const std::string &cmd, const std::string &args) = 0;
protected:
	int fd_;
	RingBuffer buffer_;
};
