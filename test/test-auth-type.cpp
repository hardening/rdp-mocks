/**
 * rdpmocks: authType protocol lists test
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

const char *securityPrefix = "NOTIFICATION:security:";

/* reads lines until one starts with @p prefix (skipping the others), false on timeout/EOF */
bool waitForPrefix(MockProcess &proc, const char *prefix, std::string &line, int timeoutMs) {
	while (readLine(proc, line, timeoutMs)) {
		if (line.compare(0, strlen(prefix), prefix) == 0)
			return true;
	}
	return false;
}

/* sends quit and drains everything up to its acknowledgement: closing the read end of the output
 * pipe before the child is done writing to it would kill the child with SIGPIPE */
void quitMock(MockProcess &proc) {
	std::string line;
	sendCommand(proc, "quit");
	while (readLine(proc, line, 5000) && line != "RESULT:SUCCESS")
		;
}

/* runs a connection with the given authType lists and checks that both sides report
 * @p expected as the negotiated protocol through "monitor security" */
bool testSession(const char *serverPath, const char *clientPath, const char *certDir, int port,
	const char *serverAuth, const char *clientAuth, const char *expected) {
	MockProcess server;
	MockProcess client;
	bool ok = spawnMock(serverPath, server) && spawnMock(clientPath, client);

	ok = ok && sendCommand(server, std::string("authType ") + serverAuth);
	ok = ok && sendCommand(server, std::string("autoCert ") + certDir);
	ok = ok && sendCommand(server, "monitor security");
	ok = ok && sendCommand(server, "listen 127.0.0.1:" + std::to_string(port));

	std::string line;
	if (ok) {
		ok = readLine(server, line, 5000) && line == "RESULT:SUCCESS:listen";
		if (!ok)
			fprintf(stderr, "server did not confirm it is listening (got '%s')\n", line.c_str());
	}

	/* the client mock still needs credentials set, or it blocks trying to prompt for them */
	ok = ok && sendCommand(client, std::string("authType ") + clientAuth);
	ok = ok && sendCommand(client, "user testuser");
	ok = ok && sendCommand(client, "password testpass");
	ok = ok && sendCommand(client, "monitor security");
	ok = ok && sendCommand(client, "connect 127.0.0.1:" + std::to_string(port));

	std::string expectedLine = std::string(securityPrefix) + expected;
	if (ok) {
		ok = waitForPrefix(client, securityPrefix, line, 5000) && line == expectedLine;
		if (!ok)
			fprintf(
				stderr, "client: expected '%s' (got '%s')\n", expectedLine.c_str(), line.c_str());
	}
	if (ok) {
		ok = waitForPrefix(server, securityPrefix, line, 5000) && line == expectedLine;
		if (!ok)
			fprintf(
				stderr, "server: expected '%s' (got '%s')\n", expectedLine.c_str(), line.c_str());
	}

	quitMock(client);
	quitMock(server);

	bool clientOk = waitMock(client);
	bool serverOk = waitMock(server);

	closeMock(client);
	closeMock(server);

	if (!ok || !clientOk || !serverOk) {
		fprintf(stderr,
			"session server=%s client=%s FAILED (negotiation=%d clientExit=%d serverExit=%d)\n",
			serverAuth, clientAuth, ok, clientOk, serverOk);
		return false;
	}
	return true;
}

/* sends authType @p list followed by quit: a valid list is silently accepted and quit gets
 * acknowledged, an invalid one is a command error which stops the mock before it reads quit */
bool testAuthTypeList(const char *mockPath, const char *list, bool valid) {
	MockProcess mock;
	if (!spawnMock(mockPath, mock))
		return false;

	std::string line;
	bool ok = sendCommand(mock, std::string("authType ") + list) && sendCommand(mock, "quit");
	bool acked = ok && readLine(mock, line, 5000) && line == "RESULT:SUCCESS";
	ok = ok && waitMock(mock) && (acked == valid);
	closeMock(mock);

	if (!ok) {
		fprintf(
			stderr, "%s: authType '%s' was %s\n", mockPath, list, acked ? "accepted" : "rejected");
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

	char certDirTemplate[] = "/tmp/rdpmocks-test-XXXXXX";
	char *certDir = mkCertDir(certDirTemplate);
	if (!certDir)
		return 1;

	/* spread out the port a bit to reduce the chance of clashing with a concurrent test run */
	int port = 30000 + (int)(getpid() % 10000);

	/* the server offers several protocols, the client picks the one it supports */
	bool ok = testSession(serverPath, clientPath, certDir, port, "tls,nla", "tls", "tls");
	/* the client requests several protocols, the server picks the one it supports */
	ok = testSession(serverPath, clientPath, certDir, port + 1, "tls", "ext,nla,tls", "tls") && ok;
	/* standard RDP security among other choices on the server side */
	ok = testSession(serverPath, clientPath, certDir, port + 2, "rdp,tls", "rdp", "rdp") && ok;

	const char *mocks[] = { serverPath, clientPath };
	for (const char *mock : mocks) {
		ok = testAuthTypeList(mock, "rdp,tls,nla,ext,rdstls,aad", true) && ok;
		ok = testAuthTypeList(mock, "tls,bogus", false) && ok;
		ok = testAuthTypeList(mock, "tls,", false) && ok;
		ok = testAuthTypeList(mock, "", false) && ok;
	}

	rmCertDir(certDir);

	if (!ok) {
		fprintf(stderr, "TEST FAILED\n");
		return 1;
	}

	printf("TEST PASSED\n");
	return 0;
}
