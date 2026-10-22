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

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <sys/stat.h>

#include <winpr/file.h>
#include <winpr/string.h>
#include <winpr/user.h>
#include <winpr/wlog.h>

#include <freerdp/utils/cliprdr_utils.h>

#include <Magick++.h>

#include "clipboardHelper.h"

#define TAG "rdpmocks.clipboard"

/* ids of the registered formats we announce, only meaningful in our own Format Lists (the peer
 * refers to them when requesting data) */
#define LOCAL_FORMAT_PNG 0xC0A0
#define LOCAL_FORMAT_FILE_GROUP_DESCRIPTOR 0xC0A1

#define FORMAT_NAME_PNG "PNG"
#define FORMAT_NAME_FILE_GROUP_DESCRIPTOR "FileGroupDescriptorW"

/* size of the chunks requested with FILECONTENTS_RANGE */
#define FILE_RANGE_CHUNK (64 * 1024)

/* seconds between 1601-01-01 (FILETIME epoch) and 1970-01-01 */
#define FILETIME_UNIX_EPOCH_DELTA 11644473600ULL

static bool writeFile(const std::string &path, const BYTE *data, size_t size) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out)
		return false;
	out.write((const char *)data, (std::streamsize)size);
	return (bool)out;
}

/* "\n", "\t" and "\\" escapes, so that multi-line text fits on a command line */
static std::string unescapeText(const std::string &args) {
	std::string ret;
	for (size_t i = 0; i < args.size(); i++) {
		if (args[i] == '\\' && i + 1 < args.size()) {
			switch (args[i + 1]) {
			case 'n':
				ret += '\n';
				i++;
				continue;
			case 't':
				ret += '\t';
				i++;
				continue;
			case '\\':
				ret += '\\';
				i++;
				continue;
			default:
				break;
			}
		}
		ret += args[i];
	}
	return ret;
}

/* a CF_DIB/CF_DIBV5 payload is a BMP file without its BITMAPFILEHEADER: rebuild one so that
 * Magick++ can decode any header version/compression/bit depth */
static bool dibToBmp(const BYTE *data, size_t size, std::vector<BYTE> &bmp) {
	if (size < 40)
		return false;

	UINT32 headerSize = data[0] | (data[1] << 8) | (data[2] << 16) | ((UINT32)data[3] << 24);
	UINT16 bitCount = data[14] | (data[15] << 8);
	UINT32 compression = data[16] | (data[17] << 8) | (data[18] << 16) | ((UINT32)data[19] << 24);
	UINT32 clrUsed = data[32] | (data[33] << 8) | (data[34] << 16) | ((UINT32)data[35] << 24);
	if (headerSize < 40 || headerSize > size)
		return false;

	UINT32 offBits = 14 + headerSize;
	/* BI_BITFIELDS masks follow a plain BITMAPINFOHEADER, later versions embed them */
	if (compression == 3 && headerSize == 40)
		offBits += 12;
	if (clrUsed)
		offBits += clrUsed * 4;
	else if (bitCount <= 8)
		offBits += (1u << bitCount) * 4;

	UINT32 fileSize = (UINT32)(14 + size);
	bmp.resize(14);
	bmp[0] = 'B';
	bmp[1] = 'M';
	for (int i = 0; i < 4; i++) {
		bmp[2 + i] = (BYTE)(fileSize >> (8 * i));
		bmp[6 + i] = 0;
		bmp[10 + i] = (BYTE)(offBits >> (8 * i));
	}
	bmp.insert(bmp.end(), data, data + size);
	return true;
}

/* rejects absolute paths and ".." components: the names come from the peer */
static bool sanitizeRemotePath(const std::string &name, std::string &path) {
	path.clear();
	std::string component;
	std::istringstream iss(name);
	bool first = true;
	while (std::getline(iss, component, '\\')) {
		if (component.empty() || component == "." || component == ".." ||
			component.find('/') != std::string::npos)
			return false;
		if (!first)
			path += '/';
		path += component;
		first = false;
	}
	return !path.empty();
}

ClipboardHelper::ClipboardHelper(OutputChannel *output)
: output_(output)
, enabled_(true)
, monitor_(false)
, ready_(false)
, content_(CONTENT_NONE)
, pending_(PENDING_NONE)
, pendingFormatId_(0)
, streamId_(0)
, fetchedFiles_(0)
, currentFile_(nullptr)
, currentSize_(0)
, currentOffset_(0) {}

ClipboardHelper::~ClipboardHelper() {
	if (currentFile_)
		fclose(currentFile_);
}

void ClipboardHelper::setMonitor(bool monitor) {
	std::lock_guard<std::mutex> lock(mutex_);
	monitor_ = monitor;
}

void ClipboardHelper::setEnabled(bool enabled) {
	std::lock_guard<std::mutex> lock(mutex_);
	enabled_ = enabled;
}

bool ClipboardHelper::isEnabled() {
	std::lock_guard<std::mutex> lock(mutex_);
	return enabled_;
}

void ClipboardHelper::onReady() {
	std::lock_guard<std::mutex> lock(mutex_);
	sendLocalFormatList();
	if (!ready_) {
		ready_ = true;
		if (monitor_)
			output_->sendNotification("clipboard", "ready");
	}
}

void ClipboardHelper::reset() {
	std::lock_guard<std::mutex> lock(mutex_);
	ready_ = false;
	peerFormats_.clear();
	if (pending_ != PENDING_NONE)
		finishPending(false, "disconnected");
}

UINT ClipboardHelper::sendLocalFormatList() {
	std::vector<CLIPRDR_FORMAT> formats;
	switch (content_) {
	case CONTENT_TEXT:
		formats.push_back({ CF_UNICODETEXT, nullptr });
		formats.push_back({ CF_TEXT, nullptr });
		break;
	case CONTENT_IMAGE:
		formats.push_back({ CF_DIB, nullptr });
		formats.push_back({ LOCAL_FORMAT_PNG, (char *)FORMAT_NAME_PNG });
		break;
	case CONTENT_FILES:
		formats.push_back(
			{ LOCAL_FORMAT_FILE_GROUP_DESCRIPTOR, (char *)FORMAT_NAME_FILE_GROUP_DESCRIPTOR });
		break;
	case CONTENT_NONE:
		break;
	}

	CLIPRDR_FORMAT_LIST formatList = {};
	formatList.common.msgType = CB_FORMAT_LIST;
	formatList.numFormats = (UINT32)formats.size();
	formatList.formats = formats.empty() ? nullptr : formats.data();
	return sendFormatList(&formatList);
}

UINT ClipboardHelper::sendDataResponse(const BYTE *data, size_t size) {
	CLIPRDR_FORMAT_DATA_RESPONSE response = {};
	response.common.msgType = CB_FORMAT_DATA_RESPONSE;
	response.common.msgFlags = data ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
	response.common.dataLen = data ? (UINT32)size : 0;
	response.requestedFormatData = data;
	return sendFormatDataResponse(&response);
}

UINT ClipboardHelper::sendFileContentsFailure(UINT32 streamId) {
	CLIPRDR_FILE_CONTENTS_RESPONSE response = {};
	response.common.msgType = CB_FILECONTENTS_RESPONSE;
	response.common.msgFlags = CB_RESPONSE_FAIL;
	response.streamId = streamId;
	return sendFileContentsResponse(&response);
}

UINT32 ClipboardHelper::peerFormatByName(const std::string &name) const {
	for (const auto &format : peerFormats_) {
		if (format.second == name)
			return format.first;
	}
	return 0;
}

bool ClipboardHelper::peerHasFormat(UINT32 formatId) const {
	for (const auto &format : peerFormats_) {
		if (format.first == formatId)
			return true;
	}
	return false;
}

void ClipboardHelper::finishPending(bool success, const std::string &extra) {
	if (currentFile_) {
		fclose(currentFile_);
		currentFile_ = nullptr;
	}
	remoteFiles_.clear();
	pending_ = PENDING_NONE;
	output_->sendResult(success, extra);
}

bool ClipboardHelper::requestFormat(PendingKind kind, UINT32 formatId, const std::string &target) {
	CLIPRDR_FORMAT_DATA_REQUEST request = {};
	request.common.msgType = CB_FORMAT_DATA_REQUEST;
	request.common.dataLen = 4;
	request.requestedFormatId = formatId;

	pending_ = kind;
	pendingFormatId_ = formatId;
	pendingTarget_ = target;
	if (sendFormatDataRequest(&request) != CHANNEL_RC_OK) {
		pending_ = PENDING_NONE;
		return false;
	}
	return true;
}

bool ClipboardHelper::setText(const std::string &args, std::string &error) {
	text_ = unescapeText(args);
	content_ = CONTENT_TEXT;
	return true;
}

bool ClipboardHelper::setImage(const std::string &path, std::string &error) {
	Magick::Image image;
	Magick::Blob pngBlob;
	std::vector<BYTE> pixels;
	try {
		image.read(path);
		pixels.resize(image.columns() * image.rows() * 4);
		image.write(0, 0, image.columns(), image.rows(), "BGRA", Magick::CharPixel, pixels.data());
		image.magick("PNG");
		image.write(&pngBlob);
	} catch (Magick::Exception &e) {
		WLog_ERR(TAG, "unable to load image %s: %s", path.c_str(), e.what());
		error = "invalid file";
		return false;
	}

	const UINT32 width = (UINT32)image.columns();
	const UINT32 height = (UINT32)image.rows();
	const size_t lineSize = (size_t)width * 4;

	/* BITMAPINFOHEADER, 32bpp BI_RGB, bottom-up rows */
	BYTE header[40] = {};
	const UINT32 imageSize = (UINT32)(lineSize * height);
	const UINT32 fields[] = { 40, width, height };
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 4; j++)
			header[i * 4 + j] = (BYTE)(fields[i] >> (8 * j));
	header[12] = 1; /* biPlanes */
	header[14] = 32; /* biBitCount */
	for (int j = 0; j < 4; j++)
		header[20 + j] = (BYTE)(imageSize >> (8 * j));

	dib_.assign(header, header + sizeof(header));
	for (UINT32 y = height; y > 0; y--) {
		const BYTE *line = &pixels[(y - 1) * lineSize];
		dib_.insert(dib_.end(), line, line + lineSize);
	}

	const BYTE *pngData = (const BYTE *)pngBlob.data();
	png_.assign(pngData, pngData + pngBlob.length());
	content_ = CONTENT_IMAGE;
	return true;
}

bool ClipboardHelper::setFiles(const std::string &args, std::string &error) {
	namespace fs = std::filesystem;

	std::vector<LocalFile> files;
	std::vector<std::string> names;
	std::istringstream iss(args);
	std::string arg;
	std::error_code ec;

	auto addEntry = [&](const fs::path &path, const std::string &name) {
		LocalFile file;
		file.path = path.string();
		file.isDirectory = fs::is_directory(path, ec);
		file.size = file.isDirectory ? 0 : (UINT64)fs::file_size(path, ec);
		files.push_back(file);
		names.push_back(name);
		return !ec;
	};

	while (iss >> arg) {
		fs::path top = fs::path(arg).lexically_normal();
		if (top.filename().empty())
			top = top.parent_path();
		if (!fs::exists(top, ec) || !addEntry(top, top.filename().string())) {
			error = "invalid file";
			return false;
		}

		if (files.back().isDirectory) {
			/* directories come before their content, as the peer recreates the tree in order */
			for (auto it = fs::recursive_directory_iterator(top, ec);
				 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
				std::string relative = fs::relative(it->path(), top.parent_path(), ec).string();
				std::replace(relative.begin(), relative.end(), '/', '\\');
				if (!addEntry(it->path(), relative)) {
					error = "invalid file";
					return false;
				}
			}
			if (ec) {
				error = "invalid file";
				return false;
			}
		}
	}

	if (files.empty()) {
		error = "missing file name";
		return false;
	}

	std::vector<FILEDESCRIPTORW> descriptors(files.size());
	for (size_t i = 0; i < files.size(); i++) {
		FILEDESCRIPTORW &descriptor = descriptors[i];
		descriptor = FILEDESCRIPTORW{};
		descriptor.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI;
		descriptor.dwFileAttributes =
			files[i].isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
		descriptor.nFileSizeHigh = (DWORD)(files[i].size >> 32);
		descriptor.nFileSizeLow = (DWORD)(files[i].size & 0xFFFFFFFF);

		struct stat st = {};
		if (stat(files[i].path.c_str(), &st) == 0) {
			UINT64 writeTime = ((UINT64)st.st_mtime + FILETIME_UNIX_EPOCH_DELTA) * 10000000ULL;
			descriptor.ftLastWriteTime.dwLowDateTime = (DWORD)(writeTime & 0xFFFFFFFF);
			descriptor.ftLastWriteTime.dwHighDateTime = (DWORD)(writeTime >> 32);
		}

		if (ConvertUtf8ToWChar(
				names[i].c_str(), descriptor.cFileName, ARRAYSIZE(descriptor.cFileName)) < 0) {
			error = "invalid file";
			return false;
		}
	}

	files_ = std::move(files);
	descriptors_ = std::move(descriptors);
	content_ = CONTENT_FILES;
	return true;
}

bool ClipboardHelper::handleCommand(const std::string &cmd, const std::string &args) {
	if (cmd.compare(0, 10, "clipboard_") != 0)
		return false;

	std::lock_guard<std::mutex> lock(mutex_);

	bool isSet = (cmd == "clipboard_set_text" || cmd == "clipboard_set_image" ||
		cmd == "clipboard_set_files" || cmd == "clipboard_clear");
	bool isGet = (cmd == "clipboard_get_text" || cmd == "clipboard_get_image" ||
		cmd == "clipboard_get_files" || cmd == "clipboard_get");
	if (!isSet && !isGet)
		return false;

	if (!enabled_) {
		output_->sendResult(false, "clipboard disabled");
		return true;
	}
	if (!ready_) {
		output_->sendResult(false, "channel not ready");
		return true;
	}

	std::string error;
	if (isSet) {
		bool ok = true;
		if (cmd == "clipboard_set_text")
			ok = setText(args, error);
		else if (cmd == "clipboard_set_image")
			ok = setImage(args, error);
		else if (cmd == "clipboard_set_files")
			ok = setFiles(args, error);
		else
			content_ = CONTENT_NONE;

		if (ok && sendLocalFormatList() != CHANNEL_RC_OK)
			error = "send failed";
		output_->sendResult(error.empty(), error);
		return true;
	}

	if (pending_ != PENDING_NONE) {
		output_->sendResult(false, "request pending");
		return true;
	}

	std::string target = args;
	PendingKind kind = PENDING_RAW;
	UINT32 formatId = 0;
	if (cmd == "clipboard_get_text") {
		kind = PENDING_TEXT;
		if (peerHasFormat(CF_UNICODETEXT))
			formatId = CF_UNICODETEXT;
		else if (peerHasFormat(CF_TEXT))
			formatId = CF_TEXT;
	} else if (cmd == "clipboard_get_image") {
		/* PNG first since it keeps the alpha channel */
		kind = PENDING_IMAGE;
		formatId = peerFormatByName(FORMAT_NAME_PNG);
		if (!formatId && peerHasFormat(CF_DIBV5))
			formatId = CF_DIBV5;
		if (!formatId && peerHasFormat(CF_DIB))
			formatId = CF_DIB;
	} else if (cmd == "clipboard_get_files") {
		kind = PENDING_FILE_LIST;
		formatId = peerFormatByName(FORMAT_NAME_FILE_GROUP_DESCRIPTOR);
	} else {
		/* clipboard_get <formatId|formatName> <file> */
		size_t space = args.find(' ');
		std::string format = args.substr(0, space);
		target = (space == std::string::npos) ? "" : args.substr(space + 1);

		char *end = nullptr;
		unsigned long id = strtoul(format.c_str(), &end, 0);
		if (!format.empty() && end && *end == 0)
			formatId = peerHasFormat((UINT32)id) ? (UINT32)id : 0;
		else
			formatId = peerFormatByName(format);
	}

	if (target.empty()) {
		output_->sendResult(false, "missing file name");
		return true;
	}
	if (!formatId) {
		output_->sendResult(false, "format not available");
		return true;
	}

	if (kind == PENDING_FILE_LIST) {
		std::error_code ec;
		std::filesystem::create_directories(target, ec);
		if (!std::filesystem::is_directory(target, ec)) {
			output_->sendResult(false, "write failed");
			return true;
		}
	}

	if (!requestFormat(kind, formatId, target))
		output_->sendResult(false, "send failed");
	return true;
}

UINT ClipboardHelper::onPeerFormatList(const CLIPRDR_FORMAT_LIST *formatList) {
	std::lock_guard<std::mutex> lock(mutex_);

	peerFormats_.clear();
	std::string formats;
	for (UINT32 i = 0; i < formatList->numFormats; i++) {
		const CLIPRDR_FORMAT &format = formatList->formats[i];
		std::string name = format.formatName ? format.formatName : "";
		peerFormats_.emplace_back(format.formatId, name);
		if (i)
			formats += ",";
		formats += std::to_string(format.formatId) + ":" + name;
	}

	CLIPRDR_FORMAT_LIST_RESPONSE response = {};
	response.common.msgType = CB_FORMAT_LIST_RESPONSE;
	response.common.msgFlags = CB_RESPONSE_OK;
	UINT rc = sendFormatListResponse(&response);

	/* server side: the client's initial Format List, mandatory after Monitor Ready, is what tells
	 * the channel is usable */
	if (!ready_) {
		ready_ = true;
		if (monitor_)
			output_->sendNotification("clipboard", "ready");
	}

	if (monitor_)
		output_->sendNotification("clipboard", "formats " + formats);
	return rc;
}

UINT ClipboardHelper::onPeerFormatDataRequest(const CLIPRDR_FORMAT_DATA_REQUEST *request) {
	std::lock_guard<std::mutex> lock(mutex_);

	const UINT32 formatId = request->requestedFormatId;
	if (monitor_)
		output_->sendNotification("clipboard", "dataRequest format=" + std::to_string(formatId));

	switch (content_) {
	case CONTENT_TEXT:
		if (formatId == CF_UNICODETEXT) {
			size_t len = 0;
			WCHAR *wtext = ConvertUtf8ToWCharAlloc(text_.c_str(), &len);
			if (!wtext)
				return sendDataResponse(nullptr, 0);
			UINT rc = sendDataResponse((const BYTE *)wtext, (len + 1) * sizeof(WCHAR));
			free(wtext);
			return rc;
		}
		if (formatId == CF_TEXT)
			return sendDataResponse((const BYTE *)text_.c_str(), text_.size() + 1);
		break;

	case CONTENT_IMAGE:
		if (formatId == CF_DIB)
			return sendDataResponse(dib_.data(), dib_.size());
		if (formatId == LOCAL_FORMAT_PNG)
			return sendDataResponse(png_.data(), png_.size());
		break;

	case CONTENT_FILES:
		if (formatId == LOCAL_FORMAT_FILE_GROUP_DESCRIPTOR) {
			BYTE *data = nullptr;
			UINT32 size = 0;
			if (cliprdr_serialize_file_list_ex(GENERAL_FLAGS, descriptors_.data(),
					(UINT32)descriptors_.size(), &data, &size) != CHANNEL_RC_OK)
				return sendDataResponse(nullptr, 0);
			UINT rc = sendDataResponse(data, size);
			free(data);
			return rc;
		}
		break;

	case CONTENT_NONE:
		break;
	}

	return sendDataResponse(nullptr, 0);
}

UINT ClipboardHelper::onPeerFileContentsRequest(const CLIPRDR_FILE_CONTENTS_REQUEST *request) {
	std::lock_guard<std::mutex> lock(mutex_);

	const bool isSize = (request->dwFlags & FILECONTENTS_SIZE) != 0;
	if (monitor_)
		output_->sendNotification("clipboard",
			"fileContentsRequest stream=" + std::to_string(request->streamId) + " index=" +
				std::to_string(request->listIndex) + " flags=" + (isSize ? "size" : "range"));

	if (content_ != CONTENT_FILES || request->listIndex >= files_.size() ||
		files_[request->listIndex].isDirectory)
		return sendFileContentsFailure(request->streamId);

	const LocalFile &file = files_[request->listIndex];
	CLIPRDR_FILE_CONTENTS_RESPONSE response = {};
	response.common.msgType = CB_FILECONTENTS_RESPONSE;
	response.common.msgFlags = CB_RESPONSE_OK;
	response.streamId = request->streamId;

	if (isSize) {
		BYTE size[8];
		for (int i = 0; i < 8; i++)
			size[i] = (BYTE)(file.size >> (8 * i));
		response.cbRequested = sizeof(size);
		response.common.dataLen = 4 + sizeof(size);
		response.requestedData = size;
		return sendFileContentsResponse(&response);
	}

	const UINT64 offset = ((UINT64)request->nPositionHigh << 32) | request->nPositionLow;
	std::vector<BYTE> data;
	if (offset < file.size) {
		data.resize((size_t)std::min<UINT64>(request->cbRequested, file.size - offset));
		std::ifstream in(file.path, std::ios::binary);
		in.seekg((std::streamoff)offset);
		if (!in.read((char *)data.data(), (std::streamsize)data.size()))
			return sendFileContentsFailure(request->streamId);
	}

	response.cbRequested = (UINT32)data.size();
	response.common.dataLen = 4 + response.cbRequested;
	response.requestedData = data.data();
	return sendFileContentsResponse(&response);
}

UINT ClipboardHelper::onPeerFormatDataResponse(const CLIPRDR_FORMAT_DATA_RESPONSE *response) {
	std::lock_guard<std::mutex> lock(mutex_);

	if (pending_ != PENDING_TEXT && pending_ != PENDING_IMAGE && pending_ != PENDING_RAW &&
		pending_ != PENDING_FILE_LIST)
		return CHANNEL_RC_OK;

	if (response->common.msgFlags & CB_RESPONSE_FAIL) {
		finishPending(false, "request failed");
		return CHANNEL_RC_OK;
	}

	const BYTE *data = response->requestedFormatData;
	const UINT32 size = response->common.dataLen;
	std::string error;

	switch (pending_) {
	case PENDING_TEXT: {
		std::string text;
		if (pendingFormatId_ == CF_UNICODETEXT) {
			size_t len = 0;
			char *utf8 = ConvertWCharNToUtf8Alloc((const WCHAR *)data, size / sizeof(WCHAR), &len);
			if (utf8) {
				text = utf8;
				free(utf8);
			}
		} else {
			text.assign((const char *)data, strnlen((const char *)data, size));
		}
		if (!writeFile(pendingTarget_, (const BYTE *)text.data(), text.size()))
			error = "write failed";
		break;
	}

	case PENDING_IMAGE:
		try {
			Magick::Image image;
			if (pendingFormatId_ == CF_DIB || pendingFormatId_ == CF_DIBV5) {
				std::vector<BYTE> bmp;
				if (!dibToBmp(data, size, bmp))
					throw Magick::Exception("invalid DIB");
				image.read(Magick::Blob(bmp.data(), bmp.size()));
			} else {
				image.read(Magick::Blob(data, size));
			}
			image.magick("PNG");
			image.write(pendingTarget_);
		} catch (Magick::Exception &e) {
			WLog_ERR(TAG, "unable to convert clipboard image: %s", e.what());
			error = "write failed";
		}
		break;

	case PENDING_RAW:
		if (!writeFile(pendingTarget_, data, size))
			error = "write failed";
		break;

	case PENDING_FILE_LIST:
		if (!treatFileList(data, size, error))
			break;
		/* the RESULT comes once all the files have been fetched */
		fetchNextFile();
		return CHANNEL_RC_OK;

	default:
		break;
	}

	finishPending(error.empty(), error);
	return CHANNEL_RC_OK;
}

bool ClipboardHelper::treatFileList(const BYTE *data, UINT32 size, std::string &error) {
	FILEDESCRIPTORW *descriptors = nullptr;
	UINT32 count = 0;
	if (cliprdr_parse_file_list(data, size, &descriptors, &count) != CHANNEL_RC_OK) {
		error = "invalid file list";
		return false;
	}

	fetchedFiles_ = 0;
	remoteFiles_.clear();
	for (UINT32 i = 0; i < count && error.empty(); i++) {
		const FILEDESCRIPTORW &descriptor = descriptors[i];
		size_t len = 0;
		char *name = ConvertWCharNToUtf8Alloc(descriptor.cFileName,
			_wcsnlen(descriptor.cFileName, ARRAYSIZE(descriptor.cFileName)), &len);
		std::string relative;
		if (!name || !sanitizeRemotePath(name, relative)) {
			error = "invalid file name";
		} else {
			std::string path = pendingTarget_ + "/" + relative;
			std::error_code ec;
			if (descriptor.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				std::filesystem::create_directories(path, ec);
				if (ec)
					error = "write failed";
			} else {
				remoteFiles_.push_back({ i, path });
			}
		}
		free(name);
	}
	free(descriptors);
	return error.empty();
}

void ClipboardHelper::fetchNextFile() {
	if (remoteFiles_.empty()) {
		finishPending(true, std::to_string(fetchedFiles_));
		return;
	}

	const RemoteFile &file = remoteFiles_.front();
	currentFile_ = fopen(file.path.c_str(), "wb");
	if (!currentFile_) {
		finishPending(false, "write failed");
		return;
	}
	currentSize_ = 0;
	currentOffset_ = 0;

	CLIPRDR_FILE_CONTENTS_REQUEST request = {};
	request.common.msgType = CB_FILECONTENTS_REQUEST;
	request.common.dataLen = 24;
	request.streamId = ++streamId_;
	request.listIndex = file.listIndex;
	request.dwFlags = FILECONTENTS_SIZE;
	request.cbRequested = 8;
	pending_ = PENDING_FILE_SIZE;
	if (sendFileContentsRequest(&request) != CHANNEL_RC_OK)
		finishPending(false, "send failed");
}

void ClipboardHelper::requestNextRange() {
	const UINT64 remaining = currentSize_ - currentOffset_;

	CLIPRDR_FILE_CONTENTS_REQUEST request = {};
	request.common.msgType = CB_FILECONTENTS_REQUEST;
	request.common.dataLen = 24;
	request.streamId = ++streamId_;
	request.listIndex = remoteFiles_.front().listIndex;
	request.dwFlags = FILECONTENTS_RANGE;
	request.nPositionLow = (UINT32)(currentOffset_ & 0xFFFFFFFF);
	request.nPositionHigh = (UINT32)(currentOffset_ >> 32);
	request.cbRequested = (UINT32)std::min<UINT64>(remaining, FILE_RANGE_CHUNK);
	pending_ = PENDING_FILE_RANGE;
	if (sendFileContentsRequest(&request) != CHANNEL_RC_OK)
		finishPending(false, "send failed");
}

UINT ClipboardHelper::onPeerFileContentsResponse(const CLIPRDR_FILE_CONTENTS_RESPONSE *response) {
	std::lock_guard<std::mutex> lock(mutex_);

	if ((pending_ != PENDING_FILE_SIZE && pending_ != PENDING_FILE_RANGE) ||
		response->streamId != streamId_)
		return CHANNEL_RC_OK;

	if (response->common.msgFlags & CB_RESPONSE_FAIL) {
		finishPending(false, "request failed");
		return CHANNEL_RC_OK;
	}

	const BYTE *data = response->requestedData;
	const UINT32 size = response->cbRequested;
	bool fileDone = false;

	if (pending_ == PENDING_FILE_SIZE) {
		if (size < 8) {
			finishPending(false, "request failed");
			return CHANNEL_RC_OK;
		}
		currentSize_ = 0;
		for (int i = 0; i < 8; i++)
			currentSize_ |= (UINT64)data[i] << (8 * i);
		fileDone = (currentSize_ == 0);
	} else {
		/* an empty chunk before the announced size would loop forever */
		if (!size) {
			finishPending(false, "request failed");
			return CHANNEL_RC_OK;
		}
		if (fwrite(data, 1, size, currentFile_) != size) {
			finishPending(false, "write failed");
			return CHANNEL_RC_OK;
		}
		currentOffset_ += size;
		fileDone = (currentOffset_ >= currentSize_);
	}

	if (fileDone) {
		fclose(currentFile_);
		currentFile_ = nullptr;
		remoteFiles_.pop_front();
		fetchedFiles_++;
		fetchNextFile();
	} else {
		requestNextRange();
	}
	return CHANNEL_RC_OK;
}
