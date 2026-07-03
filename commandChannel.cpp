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
{
	if (!ringbuffer_init(&buffer_, 30))
		throw std::bad_alloc();
}

CommandChannel::~CommandChannel()
{
	ringbuffer_destroy(&buffer_);
}

CommandChannel::PollResult CommandChannel::poll()
{
	BYTE *ptr = ringbuffer_ensure_linear_write(&buffer_, 1000);
	BOOL closed = FALSE;
	int ret = read(fd_, ptr, 1000);
	if (!ret) {
		closed = TRUE;
	} else if (ret < 0)
		return POLL_ERROR;

	if (ret && !ringbuffer_commit_written_bytes(&buffer_, ret))
		return POLL_ERROR;

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
		if (!eol) {
			if (avail > MAX_LINE_BUFFER) {
				WLog_ERR(TAG, "provided command doesn't feet on a buffer line (max supported=%d)", MAX_LINE_BUFFER);
				return POLL_ERROR;
			}
			if (!closed)
				return POLL_SUCCESS;

			eol = lineBuffer + strlen(lineBuffer);
		}
		*eol = 0;
		char *startOfArgs = lineBuffer;
		while (startOfArgs < eol && *startOfArgs != ' ')
			startOfArgs++;

		*startOfArgs = 0;
		if (startOfArgs < eol) {
			startOfArgs++;
		}

		size_t toCommit = (eol - lineBuffer);
		if (!closed)
			toCommit++;
		ringbuffer_commit_read_bytes(&buffer_, toCommit);

		if (eol != lineBuffer && lineBuffer[0] != '#') {
			if (!onCommand(lineBuffer, startOfArgs))
				return POLL_ERROR;
		}
	}
	return closed ? POLL_CLOSED : POLL_SUCCESS;

}
