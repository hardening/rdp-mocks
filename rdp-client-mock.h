/**
 * rdpmocks: mock RDP client
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

class RdpClientMock;

#include "commandChannel.h"
#include "outputChannel.h"

#include <freerdp/freerdp.h>
#include <freerdp/client.h>

/** @brief */
class ClientCommandChannel : public CommandChannel {
public:
	ClientCommandChannel(int fd, RdpClientMock *mock);
	virtual ~ClientCommandChannel() = default;

	BOOL onCommand(const std::string &cmd, const std::string &args) override;

protected:
	RdpClientMock *mock_;
};

/** @brief */
struct RdpClientMockContext {
	rdpContext context_;
	RdpClientMock *mock_;
};

/** @brief */
class RdpClientMock {
	friend class ClientCommandChannel;
public:
	RdpClientMock(int inFd, OutputChannel *output);
	~RdpClientMock();

	BOOL connectClient();
	int run();

protected:
	static BOOL _client_new(freerdp *instance, rdpContext *context);
	static void _client_free(freerdp *instance, rdpContext *context);
	static int _client_start(rdpContext *context);
	static int _client_stop(rdpContext *context);
	static BOOL _client_preconnect(freerdp *instance);
	static BOOL _client_postconnect(freerdp *instance);

	static void _on_state_changed(void *context, const StateChangedEventArgs *e);
	static void _on_connection_state_change(void *context, const ConnectionStateChangeEventArgs *e);
	static void _on_connection_result(void *context, const ConnectionResultEventArgs *e);

protected:
	int cmdFd_;
	OutputChannel *output_;
	bool monitorStates_;
	bool monitorConnectionState_;
	bool doRun_;
	ClientCommandChannel commandChannel_;

	rdpSettings *settings_;
	RdpClientMockContext *rdpClient_;
	bool connectionEstablished_;
	UINT64 pollCmdChannelStartDate_;
};
