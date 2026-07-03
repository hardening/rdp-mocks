/**
 * rdpmocks: mock RDP server
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

class RdpServerMock;

#include "commandChannel.h"
#include "outputChannel.h"

#include <freerdp/freerdp.h>
#include <freerdp/client.h>
#include <freerdp/peer.h>
#include <freerdp/listener.h>

/** @brief */
class ServerCommandChannel : public CommandChannel {
public:
	ServerCommandChannel(int fd, RdpServerMock *mock);
	virtual ~ServerCommandChannel() = default;

	BOOL onCommand(const std::string &cmd, const std::string &args) override;

protected:
	RdpServerMock *mock_;
};

/** @brief */
struct RdpServerMockContext {
	rdpContext context_;
	RdpServerMock *mock_;
};

/** @brief */
class RdpServerMock {
	friend class ServerCommandChannel;
public:
	RdpServerMock(int inFd, OutputChannel *output);
	~RdpServerMock();

	int run();

protected:
	static BOOL _client_preconnect(freerdp *instance);
	static BOOL _client_postconnect(freerdp *instance);

	static BOOL _peer_accepted(freerdp_listener *instance, freerdp_peer *client);
	static BOOL _peer_post_connect(freerdp_peer *client);
	static BOOL _peer_activate(freerdp_peer *client);
	static BOOL _peer_reached_state(freerdp_peer *client, CONNECTION_STATE state);
	static BOOL _peer_keyboard_event(rdpInput* input, UINT16 flags, UINT8 code);
	static BOOL _peer_mouse_event(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y);

protected:
	int cmdFd_;
	OutputChannel *output_;
	bool monitorStates_;
	bool monitorKeyEvents_;
	bool monitorMouseEvents_;
	bool doRun_;
	ServerCommandChannel commandChannel_;

	rdpSettings *settings_;
	freerdp_peer *peer_;
	bool connectionEstablished_;
	UINT64 pollCmdChannelStartDate_;
	CONNECTION_STATE lastReachedState_;
};
