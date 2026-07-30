/**
 * rdpmocks: end-to-end test driving rdp-server-mock's "redirect" command
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

/* exercises rdp-server-mock's "redirect <host>" command: a "front" server accepts the initial
 * connection and, once negotiation completes (RdpServerMock::_peer_activate), redirects the
 * client to a "target" server. 127.0.0.0/8 all route to loopback on Linux, so front and target
 * can both bind the same port on different addresses without clashing; since server-side
 * redirection only changes the target host (not the port, see rdp_client_redirect in FreeRDP's
 * connection.c), the client mock reconnects to target on that same port. */
bool testRedirect(const char *serverPath, const char *clientPath) {
	char frontCertDirTemplate[] = "/tmp/rdpmocks-test-front-XXXXXX";
	char targetCertDirTemplate[] = "/tmp/rdpmocks-test-target-XXXXXX";
	char *frontCertDir = mkCertDir(frontCertDirTemplate);
	char *targetCertDir = frontCertDir ? mkCertDir(targetCertDirTemplate) : nullptr;
	if (!frontCertDir || !targetCertDir)
		return false;

	const char *frontHost = "127.0.0.1";
	const char *targetHost = "127.0.0.2";
	int port = 40000 + (int)(getpid() % 10000);

	MockProcess front;
	MockProcess target;
	MockProcess client;
	bool ok = spawnMock(serverPath, front) && spawnMock(serverPath, target) &&
		spawnMock(clientPath, client);

	ok = ok && sendCommand(front, "authType tls");
	ok = ok && sendCommand(front, std::string("autoCert ") + frontCertDir);
	ok = ok && sendCommand(front, std::string("redirect ") + targetHost);
	ok = ok && sendCommand(front, std::string("listen ") + frontHost + ":" + std::to_string(port));

	ok = ok && sendCommand(target, "authType tls");
	ok = ok && sendCommand(target, std::string("autoCert ") + targetCertDir);
	ok = ok && sendCommand(target, "monitor mouse");
	ok =
		ok && sendCommand(target, std::string("listen ") + targetHost + ":" + std::to_string(port));

	std::string line;
	/* wait for both servers to actually confirm they're bound and listening rather than
	 * guessing a fixed delay -- autoCert's RSA keygen can take a while under CPU contention
	 * (e.g. on a loaded CI runner), and a fixed sleep that's fine locally can be too short
	 * there, making the client's connect race the servers' listen() and fail with ECONNREFUSED */
	if (ok) {
		ok = readLine(front, line, 5000) && line == "RESULT:SUCCESS:listen";
		if (!ok)
			fprintf(stderr, "front did not confirm it is listening (got '%s')\n", line.c_str());
	}

	if (ok) {
		ok = readLine(target, line, 5000) && line == "RESULT:SUCCESS:listen";
		if (!ok)
			fprintf(stderr, "target did not confirm it is listening (got '%s')\n", line.c_str());
	}

	ok = ok && sendCommand(client, "authType tls");
	ok = ok && sendCommand(client, "user testuser");
	ok = ok && sendCommand(client, "password testpass");
	ok =
		ok && sendCommand(client, std::string("connect ") + frontHost + ":" + std::to_string(port));

	if (ok) {
		ok = readLine(front, line, 5000) && line == "RESULT:SUCCESS:accepted";
		if (!ok)
			fprintf(
				stderr, "front did not report a successful connection (got '%s')\n", line.c_str());
	}

	if (ok) {
		ok = readLine(target, line, 5000) && line == "RESULT:SUCCESS:accepted";
		if (!ok)
			fprintf(
				stderr, "target did not report a successful connection (got '%s')\n", line.c_str());
	}

	/* the client should transparently follow the redirect inside its blocking connect() call
	 * and end up fully connected to target, not front */
	if (ok) {
		ok = readLine(client, line, 5000) && line == "RESULT:SUCCESS";
		if (!ok)
			fprintf(stderr,
				"client did not report a successful (redirected) connection (got '%s')\n",
				line.c_str());
	}

	/* confirm the live session is really with target by driving a mouse move through it */
	if (ok) {
		ok = sendCommand(client, "mouse 10 20") && readLine(target, line, 5000);
		if (ok) {
			ok = line.compare(0, strlen("NOTIFICATION:mouse:"), "NOTIFICATION:mouse:") == 0 &&
				line.find("x=10") != std::string::npos && line.find("y=20") != std::string::npos;
		}
		if (!ok)
			fprintf(stderr, "target did not report the mouse move (got '%s')\n", line.c_str());
	}

	sendCommand(client, "quit");
	sendCommand(target, "quit");
	/* on the happy path front already exited on its own once the client disconnected to follow
	 * the redirect, so this is normally a no-op write to an already-closed pipe (harmless now
	 * that SIGPIPE is ignored, see spawnMock()); but if an earlier step above failed/timed out,
	 * front may still be sitting there waiting for a peer, and it must be told to quit too or
	 * waitMock(front) below would block forever */
	sendCommand(front, "quit");

	readLine(client, line, 5000);
	readLine(target, line, 5000);

	bool clientOk = waitMock(client);
	bool targetOk = waitMock(target);
	bool frontOk = waitMock(front);

	closeMock(client);
	closeMock(target);
	closeMock(front);

	rmCertDir(frontCertDir);
	rmCertDir(targetCertDir);

	if (!ok || !clientOk || !targetOk || !frontOk) {
		fprintf(stderr,
			"redirect TEST FAILED (handshake=%d clientExit=%d targetExit=%d frontExit=%d)\n", ok,
			clientOk, targetOk, frontOk);
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

	if (!testRedirect(serverPath, clientPath)) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
