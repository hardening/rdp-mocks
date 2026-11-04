/**
 * rdpmocks: text/JSON output channel shared by the mock RDP client and server
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

#include <fcntl.h>
#include <unistd.h>

#include "outputChannel.h"

OutputChannel::OutputChannel(int outFd)
: outFd_(outFd) {}

OutputChannel::~OutputChannel() {
	close(outFd_);
}

bool OutputChannel::writeLine(const std::string &line) {
	std::lock_guard<std::mutex> lock(mutex_);

	size_t written = 0;
	while (written < line.size()) {
		ssize_t ret = write(outFd_, line.c_str() + written, line.size() - written);
		if (ret <= 0)
			return false;
		written += (size_t)ret;
	}
	return true;
}

bool OutputChannel::sendResult(bool success, const std::string &extra) {
	std::string line = std::string("RESULT:") + (success ? "SUCCESS" : "FAILURE");
	if (!extra.empty())
		line += ":" + extra;
	return writeLine(line + "\n");
}

bool OutputChannel::sendNotification(const std::string &category, const std::string &msg) {
	return writeLine("NOTIFICATION:" + category + ":" + msg + "\n");
}

JsonOutputChannel::JsonOutputChannel(int outFd)
: OutputChannel(outFd) {}

bool JsonOutputChannel::sendLine(const Json::Value &root) {
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	return writeLine(Json::writeString(builder, root) + "\n");
}

bool JsonOutputChannel::sendResult(bool success, const std::string &extra) {
	Json::Value root;
	root["type"] = "result";
	root["success"] = success;
	if (!extra.empty())
		root["extra"] = extra;
	return sendLine(root);
}

bool JsonOutputChannel::sendNotification(const std::string &category, const std::string &msg) {
	Json::Value root;
	root["type"] = "notification";
	root["category"] = category;
	root["message"] = msg;
	return sendLine(root);
}
