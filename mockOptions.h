/**
 * rdpmocks: command line argument parsing shared by the mock RDP client and server
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

/** @brief */
struct MockOptions {
	MockOptions();

	int parseArgs(int argc, char *argv[]);

	//virtual bool parseUnknownArg(char *arg) = 0;

	int cmdFd;
	int outFd;
	bool debugMode;
	bool jsonOutput;
};
