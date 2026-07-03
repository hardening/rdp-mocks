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

#pragma once

#include <string>

#include <json/json.h>

/** @brief */
class OutputChannel {
public:
	OutputChannel(int outFd);
	virtual ~OutputChannel();

	virtual bool sendResult(bool success, const std::string &extra = "");
	virtual bool sendNotification(const std::string &category, const std::string &msg);
protected:
	int outFd_;
};

/** @brief emits one JSON object per line (NDJSON), e.g.:
 *   {"type":"result","success":true,"extra":"listen"}
 *   {"type":"notification","category":"states","message":"CONNECTION_STATE_NEGO"}
 */
class JsonOutputChannel : public OutputChannel {
public:
	JsonOutputChannel(int outFd);

	bool sendResult(bool success, const std::string &extra = "") override;
	bool sendNotification(const std::string &category, const std::string &msg) override;

protected:
	bool sendLine(const class Json::Value &root);
};
