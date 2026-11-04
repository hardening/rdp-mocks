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

#include <unistd.h>
#include "commandChannel.h"

#define TAG "rdp-mock-common"

CommandChannel::CommandChannel(int fd)
: fd_(fd)
, closed_(false) {
	if (!ringbuffer_init(&buffer_, 30))
		throw std::bad_alloc();
}

CommandChannel::~CommandChannel() {
	ringbuffer_destroy(&buffer_);
}

bool CommandChannel::hasBufferData() {
	return ringbuffer_used(&buffer_) > 0;
}

CommandChannel::PollResult CommandChannel::pollFd() {
	BYTE *ptr = ringbuffer_ensure_linear_write(&buffer_, 1000);
	int ret = read(fd_, ptr, 1000);
	if (!ret) {
		closed_ = true;
	} else if (ret < 0)
		return POLL_ERROR;

	if (ret && !ringbuffer_commit_written_bytes(&buffer_, ret))
		return POLL_ERROR;

	return (closed_ && ringbuffer_used(&buffer_) == 0) ? POLL_CLOSED : POLL_SUCCESS;
}

CommandChannel::PollResult CommandChannel::treat() {
	while (ringbuffer_used(&buffer_)) {
		size_t avail = ringbuffer_used(&buffer_);

#define MAX_LINE_BUFFER 1000
		char lineBuffer[MAX_LINE_BUFFER + 1];
		DataChunk chunks[2];
		int nchunks = ringbuffer_peek(&buffer_, chunks, avail);

		/* copy stuff from the ringbuffer to the lineBuffer */
		size_t remaining = MAX_LINE_BUFFER;
		char *writePtr = lineBuffer;
		for (int i = 0; i < nchunks && remaining; i++) {
			size_t toWrite = chunks[i].size > remaining ? remaining : chunks[i].size;
			memcpy(writePtr, chunks[i].data, toWrite);
			remaining -= toWrite;
			writePtr += toWrite;
		}

		size_t nchars = (size_t)(writePtr - lineBuffer);
		lineBuffer[nchars] = 0;
		char *eol = strchr(lineBuffer, '\n');
		size_t toCommit;
		if (!eol) {
			if (avail > MAX_LINE_BUFFER) {
				WLog_ERR(TAG, "provided command doesn't feet on a buffer line (max supported=%d)",
					MAX_LINE_BUFFER);
				return POLL_ERROR;
			}
			if (!closed_)
				return POLL_SUCCESS;

			eol = lineBuffer + strlen(lineBuffer);
			toCommit = (eol - lineBuffer);
		} else {
			toCommit = 1 + (eol - lineBuffer);
		}

		if (!toCommit)
			return POLL_CLOSED;

		*eol = 0;
		char *startOfArgs = lineBuffer;
		while (startOfArgs < eol && *startOfArgs != ' ')
			startOfArgs++;

		*startOfArgs = 0;
		if (startOfArgs < eol) {
			startOfArgs++;
		}

		ringbuffer_commit_read_bytes(&buffer_, toCommit);

		if (eol != lineBuffer && lineBuffer[0] != '#') {
			switch (onCommand(lineBuffer, startOfArgs)) {
			case TREAT_ERROR:
				return POLL_ERROR;
			case TREAT_SKIP:
				return POLL_SUCCESS;
			default:
				break;
			}
		}
	}
	return POLL_SUCCESS;
}


CommandChannel::TreatResult CommandChannel::toTreatResult(bool v) {
	return v ? CommandChannel::TREAT_SUCCESS : CommandChannel::TREAT_ERROR;
}

/** parses the quoted argument starting at @p pos (on the opening quote), @p pos ends after the
 * closing quote */
static bool parseQuoted(const std::string &args, size_t &pos, std::string &out, bool textEscapes) {
	out.clear();
	for (pos++; pos < args.size(); pos++) {
		char c = args[pos];
		if (c == '\'') {
			pos++;
			/* the closing quote ends the argument */
			return pos == args.size() || args[pos] == ' ';
		}

		if (c == '\\' && pos + 1 < args.size()) {
			char next = args[pos + 1];
			switch (next) {
			case '\'':
			case '\\':
				out += next;
				pos++;
				continue;
			case 'n':
			case 't':
				if (textEscapes) {
					out += (next == 'n') ? '\n' : '\t';
					pos++;
					continue;
				}
				break;
			default:
				break;
			}
		}
		out += c;
	}

	/* no closing quote */
	return false;
}

bool splitArgs(
	const std::string &args, std::vector<std::string> &out, size_t maxArgs, bool textEscapes) {
	out.clear();

	size_t pos = 0;
	while (true) {
		while (pos < args.size() && args[pos] == ' ')
			pos++;
		if (pos == args.size())
			return true;

		if (maxArgs && out.size() == maxArgs)
			return false;

		std::string arg;
		if (args[pos] == '\'') {
			if (!parseQuoted(args, pos, arg, textEscapes))
				return false;

			/* a quoted rest of the line must be alone */
			if (maxArgs && out.size() + 1 == maxArgs &&
				args.find_first_not_of(' ', pos) != std::string::npos)
				return false;
		} else if (maxArgs && out.size() + 1 == maxArgs) {
			arg = args.substr(pos);
			pos = args.size();
		} else {
			size_t end = args.find(' ', pos);
			if (end == std::string::npos)
				end = args.size();
			arg = args.substr(pos, end - pos);
			pos = end;
		}
		out.push_back(arg);
	}
}

bool unquoteArg(const std::string &args, std::string &out, bool textEscapes) {
	/* not through splitArgs() as an unquoted value is kept as is, leading spaces included */
	size_t start = args.find_first_not_of(' ');
	if (start == std::string::npos || args[start] != '\'') {
		out = args;
		return true;
	}

	size_t pos = start;
	return parseQuoted(args, pos, out, textEscapes) &&
		args.find_first_not_of(' ', pos) == std::string::npos;
}
