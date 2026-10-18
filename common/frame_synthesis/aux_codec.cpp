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

#include "aux_codec.h"

#include <cstring>
#include <format>
#include <stdexcept>
#include <zstd.h>

namespace wivrn::frame_synthesis
{

namespace
{
void check_layout(const image_layout & layout)
{
	if (layout.component_size != 2 and layout.component_size != 4)
		throw std::invalid_argument("invalid component size");
	if (layout.components == 0 or layout.pixel_stride < layout.components * layout.component_size)
		throw std::invalid_argument("invalid pixel layout");
}

// Copy to packed layout, dropping padding
void pack(const image_layout & layout, std::span<const uint8_t> src, uint8_t * dst)
{
	const size_t pixels = size_t(layout.width) * layout.height;
	const size_t size = layout.components * layout.component_size;
	if (size == layout.pixel_stride)
	{
		memcpy(dst, src.data(), pixels * size);
		return;
	}
	for (size_t i = 0; i < pixels; ++i)
		memcpy(dst + i * size, src.data() + i * layout.pixel_stride, size);
}
// Delta filter on the packed, component-interleaved element stream: each
// component value is replaced by its difference (with wrap-around, on the bit
// pattern) with the same component of the pixel on the left, or of the pixel
// above for the first column. The result is then split in byte planes: all
// bytes 0 of the elements, then all bytes 1...
// Rows are processed in typed buffers so that the loops can be vectorized.
template <typename T>
void filter_planes(const image_layout & layout, const uint8_t * packed, uint8_t * dst)
{
	const size_t row_size = size_t(layout.width) * layout.components; // elements
	const size_t count = row_size * layout.height;
	const size_t c = layout.components;
	std::vector<T> storage(3 * row_size);
	T * __restrict above = storage.data();
	T * __restrict row = above + row_size;
	T * __restrict deltas = row + row_size;

	for (uint32_t y = 0; y < layout.height; ++y)
	{
		memcpy(row, packed + y * row_size * sizeof(T), row_size * sizeof(T));
		for (size_t k = 0; k < c; ++k)
			deltas[k] = y > 0 ? T(row[k] - above[k]) : row[k];
		for (size_t k = c; k < row_size; ++k)
			deltas[k] = T(row[k] - row[k - c]);

		for (size_t b = 0; b < sizeof(T); ++b)
		{
			uint8_t * __restrict plane = dst + b * count + y * row_size;
			for (size_t k = 0; k < row_size; ++k)
				plane[k] = uint8_t(deltas[k] >> (8 * b));
		}
		std::swap(above, row);
	}
}

template <typename T>
void unfilter_planes(const image_layout & layout, const uint8_t * src, uint8_t * dst)
{
	const size_t row_size = size_t(layout.width) * layout.components; // elements
	const size_t count = row_size * layout.height;
	const size_t c = layout.components;
	std::vector<T> storage(2 * row_size);
	T * __restrict above = storage.data();
	T * __restrict row = above + row_size;

	for (uint32_t y = 0; y < layout.height; ++y)
	{
		const uint8_t * __restrict plane0 = src + y * row_size;
		for (size_t k = 0; k < row_size; ++k)
			row[k] = plane0[k];
		for (size_t b = 1; b < sizeof(T); ++b)
		{
			const uint8_t * __restrict plane = src + b * count + y * row_size;
			for (size_t k = 0; k < row_size; ++k)
				row[k] |= T(T(plane[k]) << (8 * b));
		}

		if (y > 0)
			for (size_t k = 0; k < c; ++k)
				row[k] = T(row[k] + above[k]);
		for (size_t k = c; k < row_size; ++k)
			row[k] = T(row[k] + row[k - c]);

		memcpy(dst + y * row_size * sizeof(T), row, row_size * sizeof(T));
		std::swap(above, row);
	}
}

} // namespace

size_t compress(const image_layout & layout,
                std::span<const uint8_t> src,
                filter f,
                compression c,
                int compression_level,
                std::vector<uint8_t> & out)
{
	check_layout(layout);
	const size_t pixels = size_t(layout.width) * layout.height;
	if (src.size() < pixels * layout.pixel_stride)
		throw std::invalid_argument("source buffer too small");

	const size_t packed_size = layout.packed_size();
	thread_local std::vector<uint8_t> packed;
	thread_local std::vector<uint8_t> filtered;
	packed.resize(packed_size);
	pack(layout, src, packed.data());

	switch (f)
	{
		case filter::none:
			std::swap(packed, filtered);
			break;
		case filter::delta_byte_planes:
			filtered.resize(packed_size);
			if (layout.component_size == 2)
				filter_planes<uint16_t>(layout, packed.data(), filtered.data());
			else
				filter_planes<uint32_t>(layout, packed.data(), filtered.data());
			break;
	}

	const size_t offset = out.size();
	switch (c)
	{
		case compression::none:
			out.insert(out.end(), filtered.begin(), filtered.end());
			return packed_size;
		case compression::zstd: {
			out.resize(offset + ZSTD_compressBound(packed_size));
			size_t size = ZSTD_compress(out.data() + offset, out.size() - offset, filtered.data(), packed_size, compression_level);
			if (ZSTD_isError(size))
			{
				out.resize(offset);
				throw std::runtime_error(std::format("ZSTD_compress failed: {}", ZSTD_getErrorName(size)));
			}
			out.resize(offset + size);
			return size;
		}
	}
	throw std::invalid_argument("invalid compression");
}

void decompress(const image_layout & layout,
                std::span<const uint8_t> src,
                filter f,
                compression c,
                std::span<uint8_t> dst)
{
	check_layout(layout);
	if (layout.pixel_stride != layout.components * layout.component_size)
		throw std::invalid_argument("decompression requires a packed layout");

	const size_t packed_size = layout.packed_size();
	if (dst.size() != packed_size)
		throw std::invalid_argument("invalid destination size");

	thread_local std::vector<uint8_t> filtered;
	std::span<const uint8_t> unpacked;

	switch (c)
	{
		case compression::none:
			if (src.size() != packed_size)
				throw parse_error(std::format("invalid uncompressed size {}, expected {}", src.size(), packed_size));
			unpacked = src;
			break;
		case compression::zstd: {
			unsigned long long content_size = ZSTD_getFrameContentSize(src.data(), src.size());
			if (content_size != packed_size)
				throw parse_error(std::format("invalid zstd content size {}, expected {}", content_size, packed_size));

			uint8_t * target = dst.data();
			if (f != filter::none)
			{
				filtered.resize(packed_size);
				target = filtered.data();
			}
			size_t size = ZSTD_decompress(target, packed_size, src.data(), src.size());
			if (ZSTD_isError(size))
				throw parse_error(std::format("ZSTD_decompress failed: {}", ZSTD_getErrorName(size)));
			if (size != packed_size)
				throw parse_error(std::format("invalid decompressed size {}, expected {}", size, packed_size));
			if (f == filter::none)
				return;
			unpacked = filtered;
			break;
		}
		default:
			throw parse_error("invalid compression");
	}

	switch (f)
	{
		case filter::none:
			memcpy(dst.data(), unpacked.data(), packed_size);
			return;
		case filter::delta_byte_planes:
			if (layout.component_size == 2)
				unfilter_planes<uint16_t>(layout, unpacked.data(), dst.data());
			else
				unfilter_planes<uint32_t>(layout, unpacked.data(), dst.data());
			return;
	}
	throw parse_error("invalid filter");
}

} // namespace wivrn::frame_synthesis
