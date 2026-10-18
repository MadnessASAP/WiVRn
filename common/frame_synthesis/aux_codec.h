/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Michael Weinrich <michael@a5ap.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Lossless filtering and compression of auxiliary images.

#pragma once

#include "aux_frame.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace wivrn::frame_synthesis
{

struct image_layout
{
	uint32_t width;
	uint32_t height;
	// Number of components per pixel
	uint32_t components;
	// Size of each component in bytes: 2 or 4
	uint32_t component_size;
	// Distance between two pixels in the source buffer, in bytes.
	// May be larger than components * component_size, extra bytes are
	// dropped (e.g. a 16-bit depth value stored in a 32-bit word).
	uint32_t pixel_stride;

	size_t packed_size() const
	{
		return size_t(width) * height * components * component_size;
	}
};

// Filter and compress an image.
// src must hold width * height * pixel_stride bytes.
// The result is appended to out, returns the number of bytes appended.
size_t compress(const image_layout & layout,
                std::span<const uint8_t> src,
                filter,
                compression,
                int compression_level,
                std::vector<uint8_t> & out);

// Decompress and unfilter an image into dst, packed (pixel stride is
// components * component_size). dst.size() must be layout.packed_size().
// Throws parse_error on corrupted input.
void decompress(const image_layout & layout,
                std::span<const uint8_t> src,
                filter,
                compression,
                std::span<uint8_t> dst);

} // namespace wivrn::frame_synthesis
