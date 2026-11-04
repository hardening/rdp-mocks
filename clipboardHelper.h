/**
 * rdpmocks: clipboard redirection (cliprdr) logic shared by both mocks
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

#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <winpr/shell.h>

#include <freerdp/channels/cliprdr.h>

#include "outputChannel.h"

/** @brief clipboard state and the clipboard_* commands, independent of the side of the
 * connection: the client and server mocks subclass it to send the PDUs through their own cliprdr
 * channel context, and forward the PDUs they receive to the onPeer*() methods.
 *
 * The PDU callbacks may run on a channel thread (the client cliprdr addin has its own) while
 * commands are treated from the main thread, so everything is serialized by a mutex. */
class ClipboardHelper {
public:
	ClipboardHelper(OutputChannel *output);
	virtual ~ClipboardHelper();

	/** treats the clipboard_* commands, emitting their RESULT (possibly later, when the peer
	 * answers) -- returns false if @p cmd isn't a clipboard command */
	bool handleCommand(const std::string &cmd, const std::string &args);

	/** toggles the "clipboard" notifications kind of the monitor command */
	void setMonitor(bool monitor);

	/** the "clipboard on|off" command, the clipboard_* commands fail when disabled */
	void setEnabled(bool enabled);
	bool isEnabled();

	/** the channel is usable, sends the format list of the current local content (client side:
	 * this is the mandatory initial Format List following Monitor Ready) */
	void onReady();

	/** the channel went away, fails any pending request */
	void reset();

	UINT onPeerFormatList(const CLIPRDR_FORMAT_LIST *formatList);
	UINT onPeerFormatDataRequest(const CLIPRDR_FORMAT_DATA_REQUEST *request);
	UINT onPeerFormatDataResponse(const CLIPRDR_FORMAT_DATA_RESPONSE *response);
	UINT onPeerFileContentsRequest(const CLIPRDR_FILE_CONTENTS_REQUEST *request);
	UINT onPeerFileContentsResponse(const CLIPRDR_FILE_CONTENTS_RESPONSE *response);

	/** general capability flags both mocks advertise */
	static const UINT32 GENERAL_FLAGS = CB_USE_LONG_FORMAT_NAMES | CB_STREAM_FILECLIP_ENABLED |
		CB_FILECLIP_NO_FILE_PATHS | CB_HUGE_FILE_SUPPORT_ENABLED;

protected:
	virtual UINT sendFormatList(const CLIPRDR_FORMAT_LIST *formatList) = 0;
	virtual UINT sendFormatListResponse(const CLIPRDR_FORMAT_LIST_RESPONSE *response) = 0;
	virtual UINT sendFormatDataRequest(const CLIPRDR_FORMAT_DATA_REQUEST *request) = 0;
	virtual UINT sendFormatDataResponse(const CLIPRDR_FORMAT_DATA_RESPONSE *response) = 0;
	virtual UINT sendFileContentsRequest(const CLIPRDR_FILE_CONTENTS_REQUEST *request) = 0;
	virtual UINT sendFileContentsResponse(const CLIPRDR_FILE_CONTENTS_RESPONSE *response) = 0;

private:
	enum ContentKind {
		CONTENT_NONE,
		CONTENT_TEXT,
		CONTENT_IMAGE,
		CONTENT_FILES
	};

	enum PendingKind {
		PENDING_NONE,
		PENDING_TEXT,
		PENDING_IMAGE,
		PENDING_RAW,
		PENDING_FILE_LIST,
		PENDING_FILE_SIZE,
		PENDING_FILE_RANGE,
	};

	/** a file or directory announced by clipboard_set_files */
	struct LocalFile {
		std::string path;
		bool isDirectory;
		UINT64 size;
	};

	/** a file of the peer's list still to be fetched by clipboard_get_files */
	struct RemoteFile {
		UINT32 listIndex;
		std::string path;
	};

	UINT sendLocalFormatList();
	UINT sendDataResponse(const BYTE *data, size_t size);
	UINT sendFileContentsFailure(UINT32 streamId);
	bool requestFormat(PendingKind kind, UINT32 formatId, const std::string &target);
	UINT32 peerFormatByName(const std::string &name) const;
	bool peerHasFormat(UINT32 formatId) const;
	void finishPending(bool success, const std::string &extra = "");
	void fetchNextFile();
	void requestNextRange();
	bool treatFileList(const BYTE *data, UINT32 size, std::string &error);

	bool setText(const std::string &args, std::string &error);
	bool setImage(const std::string &path, std::string &error);
	bool setFiles(const std::string &args, std::string &error);

	std::mutex mutex_;
	OutputChannel *output_;
	bool enabled_;
	bool monitor_;
	bool ready_;

	/* local content, served on demand to the peer */
	ContentKind content_;
	std::string text_;
	std::vector<BYTE> dib_;
	std::vector<BYTE> png_;
	std::vector<LocalFile> files_;
	std::vector<FILEDESCRIPTORW> descriptors_;

	/* last Format List received from the peer */
	std::vector<std::pair<UINT32, std::string>> peerFormats_;

	/* the single request in flight to the peer */
	PendingKind pending_;
	UINT32 pendingFormatId_;
	std::string pendingTarget_;
	UINT32 streamId_;
	std::deque<RemoteFile> remoteFiles_;
	size_t fetchedFiles_;
	FILE *currentFile_;
	UINT64 currentSize_;
	UINT64 currentOffset_;
};
