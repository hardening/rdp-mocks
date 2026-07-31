/**
 * rdpmocks: end-to-end test driving a basic rdp-client-mock to rdp-server-mock connection
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

#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

#include "testUtils.h"

namespace {

bool testBasicConnect(const char *serverPath, const char *clientPath) {
	char certDirTemplate[] = "/tmp/rdpmocks-test-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return false;

	/* spread out the port a bit to reduce the chance of clashing with a concurrent test run */
	int port = 30000 + (int)(getpid() % 10000);

	MockProcess server;
	MockProcess client;
	bool ok = spawnMock(serverPath, server) && spawnMock(clientPath, client);

	/* tls (rather than legacy "rdp" security) avoids a FreeRDP quirk that silently disables
	 * encryption for loopback peers under standard rdp security, and avoids NLA's credential
	 * exchange entirely -- the client mock always ignores certificate trust, see _client_preconnect */
	ok = ok && sendCommand(server, "authType tls");
	ok = ok && sendCommand(server, std::string("autoCert ") + certDir);
	ok = ok && sendCommand(server, "monitor mouse");
	ok = ok && sendCommand(server, "monitor states");
	ok = ok && sendCommand(server, "listen 127.0.0.1:" + std::to_string(port));

	std::string line;
	/* wait for the server to actually confirm it's bound and listening rather than guessing a
	 * fixed delay -- autoCert's RSA keygen can take a while under CPU contention (e.g. on a
	 * loaded CI runner), and a fixed sleep that's fine locally can be too short there, making
	 * the client's connect race the server's listen() and fail with ECONNREFUSED */
	if (ok) {
		ok = readLine(server, line, 5000) && line == "RESULT:SUCCESS:listen";
		if (!ok)
			fprintf(stderr, "server did not confirm it is listening (got '%s')\n", line.c_str());
	}

	/* the client mock ignores certificate trust (see _client_preconnect) but still needs
	 * credentials set, or it blocks trying to prompt for them interactively on stdin */
	ok = ok && sendCommand(client, "authType tls");
	ok = ok && sendCommand(client, "user testuser");
	ok = ok && sendCommand(client, "password testpass");
	ok = ok && sendCommand(client, "connect 127.0.0.1:" + std::to_string(port));

	if (ok) {
		ok = readLine(client, line, 5000) && line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(
				stderr, "client did not report a successful connection (got '%s')\n", line.c_str());
	}

	if (ok) {
		ok = readLine(server, line, 5000) && line == "RESULT:SUCCESS:accepted";
		if (!ok)
			fprintf(
				stderr, "server did not report a successful connection (got '%s')\n", line.c_str());
	}

	/* the handshake drives the peer's ReachedState callback through several CONNECTION_STATE
	 * values; with "monitor states" on, drain all the NOTIFICATION:states: lines already queued
	 * up by the time the connection succeeded above (there's normally more than one) */
	if (ok) {
		int stateNotifications = 0;
		const char *statesPrefix = "NOTIFICATION:states:";
		while (
			readLine(server, line, 500) && line.compare(0, strlen(statesPrefix), statesPrefix) == 0)
			stateNotifications++;
		ok = stateNotifications > 0;
		if (!ok)
			fprintf(stderr, "server did not report any state change\n");
	}

	/* drive a mouse move from the client and check the server reports it via its "monitor mouse"
	 * notification (NOTIFICATION:mouse:x=<x> y=<y> flags=<flags>) */
	if (ok) {
		ok = sendCommand(client, "mouse 42 84") && readLine(server, line, 5000);
		if (ok) {
			ok = line.compare(0, strlen("NOTIFICATION:mouse:"), "NOTIFICATION:mouse:") == 0 &&
				line.find("x=42") != std::string::npos && line.find("y=84") != std::string::npos;
		}
		if (!ok)
			fprintf(stderr, "server did not report the mouse move (got '%s')\n", line.c_str());
	}

	/* the server's "debug <msg>" command should unconditionally emit a
	 * NOTIFICATION:debug:<msg> line, regardless of any "monitor" setting */
	if (ok) {
		ok = sendCommand(server, "debug hello from test") && readLine(server, line, 5000);
		if (ok)
			ok = line == "NOTIFICATION:debug:hello from test";
		if (!ok)
			fprintf(
				stderr, "server did not report the debug notification (got '%s')\n", line.c_str());
	}

	sendCommand(client, "quit");
	sendCommand(server, "quit");

	/* drain the quit acknowledgement first: closing the read end of the output pipe before the
	 * child is done writing to it would kill the child with SIGPIPE */
	readLine(client, line, 5000);
	readLine(server, line, 5000);

	bool clientOk = waitMock(client);
	bool serverOk = waitMock(server);

	closeMock(client);
	closeMock(server);

	rmCertDir(certDir);

	if (!ok || !clientOk || !serverOk) {
		fprintf(stderr, "basic connect TEST FAILED (handshake=%d clientExit=%d serverExit=%d)\n",
			ok, clientOk, serverOk);
		return false;
	}

	return true;
}

} // namespace

int main(int argc, char *argv[]) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <rdp-server-mock path> <rdp-client-mock path>\n", argv[0]);
		return 1;
	}
	const char *serverPath = argv[1];
	const char *clientPath = argv[2];

	if (!testBasicConnect(serverPath, clientPath)) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
