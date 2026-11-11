/**
 * rdpmocks: image loading of rdp-server-mock
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
#include <vector>

#include <winpr/wtypes.h>

/** @brief an image loaded and decoded once, shared by every *_update path and the window icons
 * so none of them has to run Magick++ itself -- BGRA/32bpp, top-down, tightly packed
 * (stride == width * 4), matching gdi_get_pixel_format(32) == PIXEL_FORMAT_BGRA32 */
struct LoadedImage {
	UINT32 width = 0;
	UINT32 height = 0;
	std::vector<BYTE> pixels;
};

/** loads the image file at @p path (any format Magick++ reads) */
BOOL loadImagePixels(const std::string &path, LoadedImage &out);
