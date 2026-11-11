/**
 * rdpmocks: end-to-end test driving rdp-client-mock's "autoReconnect" command
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
#include <cstdlib>
#include <string>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testUtils.h"

namespace {

/* reads lines until @p expected, false on timeout */
bool expectLine(MockProcess &proc, const char *name, const std::string &expected, int timeoutMs) {
	std::string line;
	while (readLine(proc, line, timeoutMs)) {
		if (line == expected)
			return true;
	}
	fprintf(
		stderr, "%s: did not get '%s' (last line '%s')\n", name, expected.c_str(), line.c_str());
	return false;
}

void killMock(MockProcess &proc) {
	if (proc.pid > 0) {
		kill(proc.pid, SIGKILL);
		waitpid(proc.pid, nullptr, 0);
		proc.pid = -1;
	}
	closeMock(proc);
	proc.cmdWriteFd = proc.outReadFd = -1;
}

/* starts a server listening on @p port (0 lets the system pick it), returns the effective port or
 * 0 on failure */
int startServer(const char *serverPath, const char *certDir, int port, MockProcess &server) {
	const std::string listenPrefix = "RESULT:SUCCESS:listen 127.0.0.1:";
	std::string line;
	bool ok = spawnMock(serverPath, server) && sendCommand(server, "authType tls") &&
		sendCommand(server, std::string("autoCert ") + certDir) &&
		sendCommand(server, "listen 127.0.0.1:" + std::to_string(port)) &&
		readLine(server, line, 5000) && line.compare(0, listenPrefix.size(), listenPrefix) == 0;
	if (!ok) {
		fprintf(stderr, "server did not confirm it is listening (got '%s')\n", line.c_str());
		return 0;
	}
	return atoi(line.c_str() + listenPrefix.size());
}

/* the first server is killed under the client (the connection is lost without any disconnection
 * reason), a new one has taken its port: the client reconnects to it with its credentials */
bool testAutoReconnect(const char *serverPath, const char *clientPath) {
	char certDirTemplate[] = "/tmp/rdpmocks-test-reconnect-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return false;

	MockProcess server;
	MockProcess client;
	int port = startServer(serverPath, certDir, 0, server);
	bool ok = port && spawnMock(clientPath, client);

	ok = ok && sendCommand(client, "authType tls");
	ok = ok && sendCommand(client, "user testuser");
	ok = ok && sendCommand(client, "password testpass");
	ok = ok && sendCommand(client, "autoReconnect");
	ok = ok && sendCommand(client, "connect 127.0.0.1:" + std::to_string(port));
	ok = ok && expectLine(client, "client", "RESULT:SUCCESS", 5000);
	ok = ok && expectLine(server, "server", "RESULT:SUCCESS:accepted", 5000);

	/* the first server doesn't listen anymore once it accepted the client: the new one listens
	 * before the connection is lost, so that the immediate first try of the client reaches it
	 * (the next ones are TcpConnectTimeout later, 15s) */
	MockProcess server2;
	ok = ok && startServer(serverPath, certDir, port, server2) == port;

	/* the connection is lost */
	killMock(server);

	ok = ok && expectLine(server2, "server", "RESULT:SUCCESS:accepted", 10000);
	ok = ok && expectLine(client, "client", "RESULT:SUCCESS:auto-reconnected", 10000);

	/* the client is usable on the new connection */
	ok = ok && sendCommand(server2, "debug still there");
	ok = ok && expectLine(server2, "server", "NOTIFICATION:debug:still there", 5000);

	/* a client still retrying doesn't treat its commands */
	bool clientOk = false;
	bool serverOk = false;
	if (ok) {
		/* the client first: the server quitting first would be a lost connection again */
		std::string line;
		sendCommand(client, "quit");
		while (readLine(client, line, 5000) && line != "RESULT:SUCCESS")
			;
		sendCommand(server2, "quit");
		while (readLine(server2, line, 5000) && line != "RESULT:SUCCESS")
			;
		clientOk = waitMock(client);
		serverOk = waitMock(server2);
	} else {
		killMock(client);
		killMock(server2);
	}

	closeMock(client);
	closeMock(server2);

	rmCertDir(certDir);

	if (!ok || !clientOk || !serverOk) {
		fprintf(stderr, "auto-reconnect TEST FAILED (reconnect=%d clientExit=%d serverExit=%d)\n",
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

	if (!testAutoReconnect(argv[1], argv[2])) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
