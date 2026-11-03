/**
 * rdpmocks: remote applications (rail) helpers shared by both mocks
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

#include "railCommon.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include <winpr/string.h>

namespace {

struct NamedValue {
	const char *name;
	UINT16 value;
};

const NamedValue execResults[] = {
	{ "ok", RAIL_EXEC_S_OK },
	{ "hook_not_loaded", RAIL_EXEC_E_HOOK_NOT_LOADED },
	{ "decode_failed", RAIL_EXEC_E_DECODE_FAILED },
	{ "not_in_allowlist", RAIL_EXEC_E_NOT_IN_ALLOWLIST },
	{ "file_not_found", RAIL_EXEC_E_FILE_NOT_FOUND },
	{ "fail", RAIL_EXEC_E_FAIL },
	{ "session_locked", RAIL_EXEC_E_SESSION_LOCKED },
};

const NamedValue syscommands[] = {
	{ "close", SC_CLOSE },
	{ "minimize", SC_MINIMIZE },
	{ "maximize", SC_MAXIMIZE },
	{ "restore", SC_RESTORE },
};

const NamedValue moveSizeTypes[] = {
	{ "left", RAIL_WMSZ_LEFT },
	{ "right", RAIL_WMSZ_RIGHT },
	{ "top", RAIL_WMSZ_TOP },
	{ "topleft", RAIL_WMSZ_TOPLEFT },
	{ "topright", RAIL_WMSZ_TOPRIGHT },
	{ "bottom", RAIL_WMSZ_BOTTOM },
	{ "bottomleft", RAIL_WMSZ_BOTTOMLEFT },
	{ "bottomright", RAIL_WMSZ_BOTTOMRIGHT },
	{ "move", RAIL_WMSZ_MOVE },
	{ "keymove", RAIL_WMSZ_KEYMOVE },
	{ "keysize", RAIL_WMSZ_KEYSIZE },
};

std::string hexValue(UINT16 value) {
	char buffer[16];
	snprintf(buffer, sizeof(buffer), "0x%04x", value);
	return buffer;
}

template <size_t N> std::string nameOf(const NamedValue (&table)[N], UINT16 value) {
	for (const auto &entry : table) {
		if (entry.value == value)
			return entry.name;
	}
	return hexValue(value);
}

template <size_t N>
bool valueOf(const NamedValue (&table)[N], const std::string &name, UINT16 &value) {
	for (const auto &entry : table) {
		if (name == entry.name) {
			value = entry.value;
			return true;
		}
	}
	return false;
}

} // namespace

std::string railExecResultName(UINT16 execResult) {
	return nameOf(execResults, execResult);
}

bool railExecResultFromName(const std::string &name, UINT16 &execResult) {
	return valueOf(execResults, name, execResult);
}

std::string railSyscommandName(UINT16 command) {
	return nameOf(syscommands, command);
}

bool railSyscommandFromName(const std::string &name, UINT16 &command) {
	return valueOf(syscommands, name, command);
}

std::string railMoveSizeTypeName(UINT16 moveSizeType) {
	return nameOf(moveSizeTypes, moveSizeType);
}

bool railMoveSizeTypeFromName(const std::string &name, UINT16 &moveSizeType) {
	return valueOf(moveSizeTypes, name, moveSizeType);
}

std::string railUnicodeStringToUtf8(const RAIL_UNICODE_STRING &str) {
	if (!str.string || str.length < sizeof(WCHAR))
		return "";

	size_t size = 0;
	char *utf8 = ConvertWCharNToUtf8Alloc(str.string, str.length / sizeof(WCHAR), &size);
	if (!utf8)
		return "";

	std::string ret(utf8, size);
	free(utf8);
	return ret;
}

bool railParseWindowId(const std::string &str, UINT32 &windowId) {
	if (str.empty())
		return false;

	char *end = nullptr;
	errno = 0;
	unsigned long value = strtoul(str.c_str(), &end, 0);
	if (errno || *end || value > UINT32_MAX)
		return false;

	windowId = (UINT32)value;
	return true;
}
