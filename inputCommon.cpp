/**
 * rdpmocks: keyboard and mouse names, shared by both mocks
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

#include "inputCommon.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <winpr/input.h>

#include <freerdp/input.h>

namespace {

struct KeyAlias {
	const char *name;
	DWORD vkcode;
};

/* the short names, the left key for the modifiers */
const KeyAlias keyAliases[] = {
	{ "ctrl", VK_LCONTROL },
	{ "lctrl", VK_LCONTROL },
	{ "rctrl", VK_RCONTROL },
	{ "alt", VK_LMENU },
	{ "lalt", VK_LMENU },
	{ "ralt", VK_RMENU },
	{ "altgr", VK_RMENU },
	{ "shift", VK_LSHIFT },
	{ "lshift", VK_LSHIFT },
	{ "rshift", VK_RSHIFT },
	{ "win", VK_LWIN },
	{ "lwin", VK_LWIN },
	{ "rwin", VK_RWIN },
	{ "menu", VK_APPS },
	{ "enter", VK_RETURN },
	{ "return", VK_RETURN },
	{ "esc", VK_ESCAPE },
	{ "escape", VK_ESCAPE },
	{ "tab", VK_TAB },
	{ "backspace", VK_BACK },
	{ "space", VK_SPACE },
	{ "delete", VK_DELETE },
	{ "del", VK_DELETE },
	{ "insert", VK_INSERT },
	{ "ins", VK_INSERT },
	{ "home", VK_HOME },
	{ "end", VK_END },
	{ "pageup", VK_PRIOR },
	{ "pagedown", VK_NEXT },
	{ "up", VK_UP },
	{ "down", VK_DOWN },
	{ "left", VK_LEFT },
	{ "right", VK_RIGHT },
	{ "capslock", VK_CAPITAL },
	{ "numlock", VK_NUMLOCK },
	{ "scrolllock", VK_SCROLL },
	{ "printscreen", VK_SNAPSHOT },
	{ "pause", VK_PAUSE },
	{ "plus", VK_OEM_PLUS },
	{ "minus", VK_OEM_MINUS },
};

struct LockName {
	const char *name;
	UINT32 flag;
};

/* in the order of their flags */
const LockName lockNames[] = {
	{ "scrolllock", KBD_SYNC_SCROLL_LOCK },
	{ "numlock", KBD_SYNC_NUM_LOCK },
	{ "capslock", KBD_SYNC_CAPS_LOCK },
	{ "kanalock", KBD_SYNC_KANA_LOCK },
};

/** the virtual key code of a key name, 0 if unknown */
DWORD vkcodeFromName(const std::string &name) {
	if (name.compare(0, 3, "VK_") == 0) {
		DWORD vkcode = GetVirtualKeyCodeFromName(name.c_str());
		return (vkcode == VK_NONE) ? 0 : vkcode;
	}

	std::string lower = name;
	std::transform(lower.begin(), lower.end(), lower.begin(),
		[](unsigned char c) { return (char)std::tolower(c); });

	if (lower.size() == 1 && std::isalpha((unsigned char)lower[0]))
		return VK_KEY_A + (lower[0] - 'a');
	if (lower.size() == 1 && std::isdigit((unsigned char)lower[0]))
		return VK_KEY_0 + (lower[0] - '0');

	/* f1 to f24 */
	if (lower.size() >= 2 && lower[0] == 'f' &&
		std::all_of(
			lower.begin() + 1, lower.end(), [](unsigned char c) { return std::isdigit(c); })) {
		int n = atoi(lower.c_str() + 1);
		return (n >= 1 && n <= 24) ? (DWORD)(VK_F1 + n - 1) : 0;
	}

	for (const auto &alias : keyAliases) {
		if (lower == alias.name)
			return alias.vkcode;
	}
	return 0;
}

} // namespace

bool parseKey(const std::string &name, KeySpec &key) {
	key = {};

	/* raw scancode: 0x1C, or 0xE01D for an extended key */
	if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
		char *end = nullptr;
		unsigned long value = strtoul(name.c_str(), &end, 16);
		if (!end || *end)
			return false;
		if (value >= 0x01 && value <= 0xFF)
			key.scancode = (UINT32)value;
		else if ((value & 0xFF00) == 0xE000 && (value & 0xFF) && value <= 0xFFFF)
			key.scancode = (UINT32)(value & 0xFF) | KBDEXT;
		else
			return false;
		return true;
	}

	DWORD vkcode = vkcodeFromName(name);
	if (!vkcode)
		return false;
	if (vkcode == VK_PAUSE) {
		key.pause = true;
		return true;
	}

	/* the virtual key alone doesn't tell if the key is extended (VK_RETURN is both the main and
	 * the keypad enter): look in the plain scancodes first, then in the extended ones, except
	 * for print screen whose plain scancode is SysRq */
	const DWORD plain = GetVirtualScanCodeFromVirtualKeyCode(vkcode, WINPR_KBD_TYPE_IBM_ENHANCED);
	const DWORD extended =
		GetVirtualScanCodeFromVirtualKeyCode(vkcode | KBDEXT, WINPR_KBD_TYPE_IBM_ENHANCED);
	if (vkcode == VK_SNAPSHOT)
		key.scancode = (extended & 0xFF) ? extended : plain;
	else
		key.scancode = (plain & 0xFF) ? plain : extended;
	return (key.scancode & 0xFF) != 0;
}

std::string keyName(UINT8 code, bool extended) {
	DWORD vkcode = GetVirtualKeyCodeFromVirtualScanCode(
		code | (extended ? KBDEXT : 0), WINPR_KBD_TYPE_IBM_ENHANCED);
	if (!vkcode || vkcode == VK_NONE)
		return "";
	const char *name = GetVirtualKeyName(vkcode);
	return (name && *name && std::string(name) != "VK_NONE") ? name : "";
}

bool parseLockFlags(const std::string &args, UINT32 &flags) {
	flags = 0;
	if (args == "none")
		return true;

	size_t start = 0;
	while (true) {
		size_t comma = args.find(',', start);
		std::string token =
			args.substr(start, comma == std::string::npos ? std::string::npos : comma - start);

		bool found = false;
		for (const auto &lock : lockNames) {
			if (token == lock.name) {
				flags |= lock.flag;
				found = true;
			}
		}
		if (!found)
			return false;

		if (comma == std::string::npos)
			return true;
		start = comma + 1;
	}
}

std::string lockFlagsName(UINT32 flags) {
	std::string ret;
	for (const auto &lock : lockNames) {
		if (flags & lock.flag)
			ret += (ret.empty() ? "" : ",") + std::string(lock.name);
	}
	return ret.empty() ? "none" : ret;
}

bool parseMouseButton(const std::string &name, MouseButtonSpec &button) {
	struct ButtonName {
		const char *name;
		UINT16 flags;
		bool extended;
	};

	static const ButtonName buttonNames[] = {
		{ "left", PTR_FLAGS_BUTTON1, false },
		{ "right", PTR_FLAGS_BUTTON2, false },
		{ "middle", PTR_FLAGS_BUTTON3, false },
		{ "x1", PTR_XFLAGS_BUTTON1, true },
		{ "x2", PTR_XFLAGS_BUTTON2, true },
	};

	for (const auto &entry : buttonNames) {
		if (name == entry.name) {
			button.flags = entry.flags;
			button.extended = entry.extended;
			return true;
		}
	}
	return false;
}
