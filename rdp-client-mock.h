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
#include <freerdp/client/disp.h>

/** @brief */
class ClientCommandChannel : public CommandChannel {
public:
	ClientCommandChannel(int fd, RdpClientMock *mock);
	virtual ~ClientCommandChannel() = default;

	CommandChannel::TreatResult onCommand(const std::string &cmd, const std::string &args) override;

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
	static BOOL _client_redirect(freerdp *instance);
	static BOOL _client_bitmap_update(rdpContext *context, const BITMAP_UPDATE *bitmap);
	static BOOL _client_surface_bits(rdpContext *context, const SURFACE_BITS_COMMAND *cmd);
	static void _on_channel_connected(void *context, const ChannelConnectedEventArgs *e);
	static void _on_channel_disconnected(void *context, const ChannelDisconnectedEventArgs *e);
	static UINT _on_disp_caps(DispClientContext *context, UINT32 MaxNumMonitors,
		UINT32 MaxMonitorAreaFactorA, UINT32 MaxMonitorAreaFactorB);

#ifdef HAVE_CLIENT_MONITOR_STATES
	static void _on_state_changed(void *context, const StateChangedEventArgs *e);
	static void _on_connection_state_change(void *context, const ConnectionStateChangeEventArgs *e);
#endif
	static void _on_connection_result(void *context, const ConnectionResultEventArgs *e);

protected:
	int cmdFd_;
	OutputChannel *output_;
	bool monitorStates_;
	bool monitorConnectionState_;
	bool monitorGraphicsUpdates_;
	bool monitorChannels_;
	bool doRun_;
	ClientCommandChannel commandChannel_;

	rdpSettings *settings_;
	RdpClientMockContext *rdpClient_;
	bool connectionEstablished_;
	UINT64 pollCmdChannelStartDate_;
	DispClientContext *disp_;
};
