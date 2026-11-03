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

#pragma once

#include <string>

#include <freerdp/rail.h>

/** @brief name of an Execute Result code (RAIL_EXEC_*), e.g. "file_not_found", or its hex value
 * if unknown */
std::string railExecResultName(UINT16 execResult);

/** @brief parses an Execute Result name as returned by railExecResultName() */
bool railExecResultFromName(const std::string &name, UINT16 &execResult);

/** @brief name of a System Command (SC_*): close, minimize, maximize, restore, or its hex value */
std::string railSyscommandName(UINT16 command);

/** @brief parses a System Command name as returned by railSyscommandName() */
bool railSyscommandFromName(const std::string &name, UINT16 &command);

/** @brief name of a Local Move/Size type (RAIL_WMSZ_*): left, right, top, topleft, topright,
 * bottom, bottomleft, bottomright, move, keymove, keysize, or its hex value */
std::string railMoveSizeTypeName(UINT16 moveSizeType);

/** @brief parses a Local Move/Size type name as returned by railMoveSizeTypeName() */
bool railMoveSizeTypeFromName(const std::string &name, UINT16 &moveSizeType);

/** @brief converts a RAIL_UNICODE_STRING (UTF-16, length in bytes) to UTF-8 */
std::string railUnicodeStringToUtf8(const RAIL_UNICODE_STRING &str);

/** @brief parses a decimal or 0x prefixed window id, false if @p str isn't a number */
bool railParseWindowId(const std::string &str, UINT32 &windowId);
