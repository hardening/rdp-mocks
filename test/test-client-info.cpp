/**
 * rdpmocks: client info (user, domain, alternateShell, shellDirectory) test
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

/* sends quit and drains everything up to its acknowledgement: closing the read end of the output
 * pipe before the child is done writing to it would kill the child with SIGPIPE */
void quitMock(MockProcess &proc) {
	std::string line;
	sendCommand(proc, "quit");
	while (readLine(proc, line, 5000) && line != "RESULT:SUCCESS")
		;
}

/* the client sets the info packet fields, the server reports them through "monitor clientInfo" */
bool testClientInfo(const char *serverPath, const char *clientPath) {
	char certDirTemplate[] = "/tmp/rdpmocks-test-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return false;

	/* spread out the port a bit to reduce the chance of clashing with a concurrent test run */
	int port = 30000 + (int)(getpid() % 10000);

	MockProcess server;
	MockProcess client;
	bool ok = spawnMock(serverPath, server) && spawnMock(clientPath, client);

	ok = ok && sendCommand(server, "authType tls");
	ok = ok && sendCommand(server, std::string("autoCert ") + certDir);
	ok = ok && sendCommand(server, "monitor clientInfo");
	ok = ok && sendCommand(server, "listen 127.0.0.1:" + std::to_string(port));

	std::string line;
	if (ok) {
		ok = readLine(server, line, 5000) && line == "RESULT:SUCCESS:listen";
		if (!ok)
			fprintf(stderr, "server did not confirm it is listening (got '%s')\n", line.c_str());
	}

	/* the values take the rest of the line, spaces included */
	ok = ok && sendCommand(client, "authType tls");
	ok = ok && sendCommand(client, "user testuser");
	ok = ok && sendCommand(client, "password testpass");
	ok = ok && sendCommand(client, "domain TESTDOMAIN");
	ok = ok && sendCommand(client, "alternateShell C:\\Program Files\\app.exe --arg value");
	ok = ok && sendCommand(client, "shellDirectory C:\\Work Dir");
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

	const char *expected[] = {
		"NOTIFICATION:clientInfo:user=testuser",
		"NOTIFICATION:clientInfo:domain=TESTDOMAIN",
		"NOTIFICATION:clientInfo:alternateShell=C:\\Program Files\\app.exe --arg value",
		"NOTIFICATION:clientInfo:shellDirectory=C:\\Work Dir",
	};
	for (const char *expectedLine : expected) {
		if (!ok)
			break;
		ok = readLine(server, line, 5000) && line == expectedLine;
		if (!ok)
			fprintf(stderr, "server: expected '%s' (got '%s')\n", expectedLine, line.c_str());
	}

	quitMock(client);
	quitMock(server);

	bool clientOk = waitMock(client);
	bool serverOk = waitMock(server);

	closeMock(client);
	closeMock(server);

	rmCertDir(certDir);

	if (!ok || !clientOk || !serverOk) {
		fprintf(stderr, "client info TEST FAILED (info=%d clientExit=%d serverExit=%d)\n", ok,
			clientOk, serverOk);
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

	if (!testClientInfo(argv[1], argv[2])) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
