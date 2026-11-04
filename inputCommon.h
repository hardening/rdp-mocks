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

#pragma once

#include <string>

#include <winpr/wtypes.h>

/** @brief a key given to the key_* commands */
struct KeySpec {
	/** the RDP scancode, with KBDEXT for an extended key */
	UINT32 scancode;
	/** the pause key, sent with its own sequence (freerdp_input_send_keyboard_pause_event) */
	bool pause;
};

/** @brief parses a key: a short name (ctrl, enter, f1, a, 1...), a winpr virtual key name
 * (VK_RETURN, VK_KEY_A...) or a raw scancode (0x1C, 0xE01D for an extended key)
 * @return false if the key is unknown */
bool parseKey(const std::string &name, KeySpec &key);

/** @brief the virtual key name (e.g. VK_RETURN) of a received scancode, empty if unknown */
std::string keyName(UINT8 code, bool extended);

/** @brief parses a comma separated list of capslock, numlock, scrolllock and kanalock, or none,
 * into KBD_SYNC_* flags (the same values as the keyboard indicators flags) */
bool parseLockFlags(const std::string &args, UINT32 &flags);

/** @brief the lock list of KBD_SYNC_* flags, "none" if no lock */
std::string lockFlagsName(UINT32 flags);

/** @brief a mouse button given to the mouse_* commands */
struct MouseButtonSpec {
	/** PTR_FLAGS_BUTTON* (or PTR_XFLAGS_BUTTON* if extended) */
	UINT16 flags;
	/** x1 and x2, sent as extended mouse events */
	bool extended;
};

/** @brief parses left, right, middle, x1 or x2 */
bool parseMouseButton(const std::string &name, MouseButtonSpec &button);
