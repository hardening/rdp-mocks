/**
 * rdpmocks: helpers shared by the unitary test programs driving rdp-client-mock/rdp-server-mock
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

#include <unistd.h>

struct MockProcess {
	pid_t pid = -1;
	int cmdWriteFd = -1;
	int outReadFd = -1;
};

bool spawnMock(const char *path, MockProcess &proc);
bool sendCommand(MockProcess &proc, const std::string &line);

/* reads a single '\n'-terminated line, false on timeout/EOF/error */
bool readLine(MockProcess &proc, std::string &line, int timeoutMs);

void closeMock(MockProcess &proc);
bool waitMock(MockProcess &proc);

/* creates a fresh temp dir and lets "autoCert" fill it, returns nullptr on failure */
char *mkCertDir(char *tmpl);
void rmCertDir(const char *certDir);
