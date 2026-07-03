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
#include <string.h>

#include "outputChannel.h"

OutputChannel::OutputChannel(int outFd)
: outFd_(outFd)
{
}

OutputChannel::~OutputChannel()
{
	close(outFd_);
}

bool OutputChannel::sendResult(bool success, const std::string &extra)
{
	const char *status = success ? "SUCCESS" : "FAILURE";
	if (write(outFd_, "RESULT:", strlen("RESULT:")) <= 0 ||
		write(outFd_, status, strlen(status)) <= 0)
		return false;

	if (extra.size() &&
		(write(outFd_, ":", 1) <= 0 || write(outFd_, extra.c_str(), extra.size()) <= 0))
		return false;

	return write(outFd_, "\n", 1) > 0;
}

bool OutputChannel::sendNotification(const std::string &category, const std::string &msg)
{
	return write(outFd_, "NOTIFICATION:", strlen("NOTIFICATION:")) > 0 &&
		write(outFd_, category.c_str(), category.size()) > 0 &&
		write(outFd_, ":", 1) > 0 &&
		write(outFd_, msg.c_str(), msg.size()) > 0 &&
		write(outFd_, "\n", 1) > 0;
}

JsonOutputChannel::JsonOutputChannel(int outFd)
: OutputChannel(outFd)
{
}

bool JsonOutputChannel::sendLine(const Json::Value &root)
{
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	std::string line = Json::writeString(builder, root) + "\n";
	return write(outFd_, line.c_str(), line.size()) == (ssize_t)line.size();
}

bool JsonOutputChannel::sendResult(bool success, const std::string &extra)
{
	Json::Value root;
	root["type"] = "result";
	root["success"] = success;
	if (!extra.empty())
		root["extra"] = extra;
	return sendLine(root);
}

bool JsonOutputChannel::sendNotification(const std::string &category, const std::string &msg)
{
	Json::Value root;
	root["type"] = "notification";
	root["category"] = category;
	root["message"] = msg;
	return sendLine(root);
}
