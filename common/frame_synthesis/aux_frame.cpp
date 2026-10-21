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

#include "aux_frame.h"

#include "wivrn_serialization.h"

#include <bit>
#include <cmath>
#include <format>
#include <magic_enum.hpp>

namespace wivrn::frame_synthesis
{

uint64_t format_hash()
{
	static constexpr uint64_t hash = serialization_type_hash<view_header>(format_version);
	return hash;
}

size_t bytes_per_pixel(motion_format f)
{
	switch (f)
	{
		case motion_format::rg16_sfloat:
			return 4;
		case motion_format::rgba16_sfloat:
			return 8;
	}
	throw parse_error("invalid motion format");
}

size_t components(motion_format f)
{
	switch (f)
	{
		case motion_format::rg16_sfloat:
			return 2;
		case motion_format::rgba16_sfloat:
			return 4;
	}
	throw parse_error("invalid motion format");
}

size_t bytes_per_pixel(depth_format f)
{
	switch (f)
	{
		case depth_format::r32_sfloat:
			return 4;
		case depth_format::r16_sfloat:
			return 2;
	}
	throw parse_error("invalid depth format");
}

std::vector<uint8_t> serialize(const view_header & header,
                               std::span<const uint8_t> motion_payload,
                               std::span<const uint8_t> depth_payload)
{
	if (header.motion.compressed_size != motion_payload.size() or
	    header.depth.compressed_size != depth_payload.size())
		throw std::invalid_argument("frame synthesis payload size mismatch");

	serialization_packet packet;
	packet.serialize(header);
	const std::vector<std::span<uint8_t>> & spans = packet;

	std::vector<uint8_t> result;
	size_t header_size = 0;
	for (const auto & span: spans)
		header_size += span.size();
	result.reserve(header_size + motion_payload.size() + depth_payload.size());
	for (const auto & span: spans)
		result.insert(result.end(), span.begin(), span.end());
	result.insert(result.end(), motion_payload.begin(), motion_payload.end());
	result.insert(result.end(), depth_payload.begin(), depth_payload.end());
	return result;
}

namespace
{
template <typename E>
void check_enum(E value, const char * name)
{
	if (not magic_enum::enum_contains(value))
		throw parse_error(std::format("invalid {} {}", name, int(value)));
}

void check_image(const image_info & info, size_t bpp, const char * name)
{
	check_enum(info.filter_mode, "filter");
	check_enum(info.compression_mode, "compression");

	if (info.width == 0 or info.height == 0 or info.width > max_image_dimension or info.height > max_image_dimension)
		throw parse_error(std::format("invalid {} image size {}x{}", name, info.width, info.height));

	if (info.source_width == 0 or info.source_height == 0 or info.source_width > max_image_dimension or info.source_height > max_image_dimension)
		throw parse_error(std::format("invalid {} source size {}x{}", name, info.source_width, info.source_height));

	if (info.uncompressed_size != size_t(info.width) * info.height * bpp)
		throw parse_error(std::format("invalid {} uncompressed size {}", name, info.uncompressed_size));

	if (info.compressed_size > max_payload_size)
		throw parse_error(std::format("{} payload too large: {}", name, info.compressed_size));

	if (info.compression_mode == compression::none and info.compressed_size != info.uncompressed_size)
		throw parse_error(std::format("invalid {} payload size {}", name, info.compressed_size));
}

// The server and client are built with -ffast-math: the compiler assumes there is no NaN
// or infinity, std::isfinite / std::isnan and NaN-rejecting comparisons can be folded away.
// The header comes from the network, check the bits instead.
bool is_nan(float f)
{
	return (std::bit_cast<uint32_t>(f) & 0x7fffffff) > 0x7f800000;
}

bool finite(float f)
{
	return (std::bit_cast<uint32_t>(f) & 0x7f800000) != 0x7f800000;
}

bool finite(const XrVector3f & v)
{
	return finite(v.x) and finite(v.y) and finite(v.z);
}

bool finite(const XrPosef & p)
{
	return finite(p.orientation.x) and finite(p.orientation.y) and
	       finite(p.orientation.z) and finite(p.orientation.w) and finite(p.position);
}
} // namespace

parsed_view parse(std::span<const uint8_t> data)
{
	// deserialization_packet only reads from the buffer
	deserialization_packet packet(nullptr, std::span(const_cast<uint8_t *>(data.data()), data.size()));

	parsed_view result;
	try
	{
		result.header = packet.deserialize<view_header>();
	}
	catch (deserialization_error &)
	{
		throw parse_error("truncated frame synthesis header");
	}

	const view_header & h = result.header;
	if (h.version != format_version)
		throw parse_error(std::format("unsupported frame synthesis format version {}", h.version));

	if (h.view_index > 1)
		throw parse_error(std::format("invalid view index {}", h.view_index));

	check_enum(h.motion_encoding, "motion format");
	check_enum(h.depth_encoding, "depth format");
	check_image(h.motion, bytes_per_pixel(h.motion_encoding), "motion");
	check_image(h.depth, bytes_per_pixel(h.depth_encoding), "depth");

	if (not finite(h.min_depth) or not finite(h.max_depth) or
	    not(h.min_depth >= 0 and h.max_depth <= 1 and h.min_depth <= h.max_depth))
		throw parse_error(std::format("invalid depth range [{}, {}]", h.min_depth, h.max_depth));

	// Infinity is allowed, NaN is not
	if (is_nan(h.near_z) or is_nan(h.far_z) or not(h.near_z > 0 and h.far_z > 0) or h.near_z == h.far_z)
		throw parse_error(std::format("invalid near/far {}/{}", h.near_z, h.far_z));

	if (not finite(h.motion_vector_scale) or not finite(h.motion_vector_offset) or
	    not finite(h.app_space_delta_pose) or not finite(h.layer_space_pose))
		throw parse_error("non-finite frame synthesis parameters");

	const size_t header_size = serialized_size(h);
	const size_t remaining = data.size() - header_size;
	if (remaining != size_t(h.motion.compressed_size) + h.depth.compressed_size)
		throw parse_error(std::format("frame synthesis payload size mismatch: {} bytes for {} + {}",
		                              remaining,
		                              h.motion.compressed_size,
		                              h.depth.compressed_size));

	result.motion_payload = data.subspan(header_size, h.motion.compressed_size);
	result.depth_payload = data.subspan(header_size + h.motion.compressed_size, h.depth.compressed_size);
	return result;
}

} // namespace wivrn::frame_synthesis
