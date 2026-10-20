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

// Codec-neutral description of the auxiliary images (depth and motion
// vectors) submitted by an application with XR_EXT_frame_synthesis.
//
// One auxiliary frame is attached to each eye's coded video frame. It is
// serialized into a blob that is carried in-band in the video bitstream, see
// annexb_sei.h for the H.264/H.265 encapsulation.
//
// See docs/frame_synthesis_transport.md for the format specification.

#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>
#include <openxr/openxr.h>

namespace wivrn::frame_synthesis
{

// Bump when the meaning of a field changes, in addition to the automatic
// hash of the structure layout, see format_hash().
inline constexpr uint16_t format_version = 2;

// Upper bounds enforced by the parser, for both the header fields and the
// decompressed data.
inline constexpr uint32_t max_image_dimension = 4096;
inline constexpr uint32_t max_payload_size = 8 * 1024 * 1024;

// Pixel encoding of the motion vector image on the wire.
// Values are the raw texel values of the application image, as IEEE 754
// half floats, before motion_vector_scale and motion_vector_offset are
// applied.
enum class motion_format : uint8_t
{
	rg16_sfloat,   // 2D motion vectors, 4 bytes per pixel
	rgba16_sfloat, // 3D motion vectors (alpha is 0), 8 bytes per pixel
};

// Pixel encoding of the depth image on the wire.
// Values are the raw depth values of the application image (in
// [min_depth, max_depth]), not linearized.
enum class depth_format : uint8_t
{
	r32_sfloat, // 4 bytes per pixel, lossless for D32 and D24 sources
	r16_sfloat, // 2 bytes per pixel
};

// Reversible filter applied before compression.
enum class filter : uint8_t
{
	none,
	// Delta of each component bit pattern with the same component of the
	// pixel on the left (above for the first column), with wrap-around, then
	// split into byte planes. See aux_codec.cpp.
	delta_byte_planes,
};

enum class compression : uint8_t
{
	none,
	zstd,
};

struct image_info
{
	// Sub image of the application swapchain the data was taken from
	int32_t source_x;
	int32_t source_y;
	uint32_t source_width;
	uint32_t source_height;
	uint32_t source_array_index;
	int64_t source_format; // VkFormat

	// Image on the wire
	uint16_t width;
	uint16_t height;
	filter filter_mode;
	compression compression_mode;
	uint32_t uncompressed_size;
	uint32_t compressed_size; // bytes in the payload
};

enum header_flags : uint8_t
{
	// The colour image of this frame was foveated: its pixels do not map
	// linearly to the auxiliary images.
	color_foveated = 1 << 0,
	// The auxiliary images were downsampled, keeping the closest depth sample
	// and its motion vector in each footprint.
	downsampled_closest_depth = 1 << 1,
};

// Per-view metadata, mirrors XrFrameSynthesisInfoEXT semantics.
inline constexpr uint64_t no_previous_frame = UINT64_MAX;

struct view_header
{
	uint16_t version;
	uint64_t frame_index; // WiVRn frame index, same as the video shards
	uint8_t view_index;   // 0: left, 1: right
	XrTime display_time;  // headset clock, same as view_info_t::display_time
	uint8_t flags;        // header_flags

	// Source application frames, headset clock: the display time of the
	// application frame shown by this frame, and of the application frame
	// the motion vectors and app_space_delta_pose are relative to (0: none).
	XrTime app_display_time;
	XrTime previous_app_display_time;
	// WiVRn frame index of the previous frame sent to the headset if it
	// showed that previous application frame, no_previous_frame otherwise:
	// the motion describes the change from that frame to this one.
	uint64_t previous_frame_index;

	// XrFrameSynthesisInfoEXT
	uint64_t layer_flags; // XrFrameSynthesisInfoFlagsEXT
	XrVector3f motion_vector_scale;
	XrVector3f motion_vector_offset;
	XrPosef app_space_delta_pose; // in the projection layer space
	XrPosef layer_space_pose;     // projection layer space, in the same space as view_info_t::pose
	float min_depth;
	float max_depth;
	float near_z; // may be infinite, greater than far_z for reversed depth
	float far_z;  // may be infinite

	// Sub image of the application colour swapchain for this view
	int32_t color_x;
	int32_t color_y;
	uint32_t color_width;
	uint32_t color_height;

	motion_format motion_encoding;
	image_info motion;
	depth_format depth_encoding;
	image_info depth;
};

class parse_error : public std::runtime_error
{
public:
	using std::runtime_error::runtime_error;
};

// Hash of the structure layout and version, used for capability negotiation.
uint64_t format_hash();

size_t bytes_per_pixel(motion_format);
size_t components(motion_format);
size_t bytes_per_pixel(depth_format);

// Serialize header followed by the (already compressed) motion and depth
// payloads. header.motion.compressed_size and header.depth.compressed_size
// must match the payload sizes.
std::vector<uint8_t> serialize(const view_header & header,
                               std::span<const uint8_t> motion_payload,
                               std::span<const uint8_t> depth_payload);

struct parsed_view
{
	view_header header;
	std::span<const uint8_t> motion_payload;
	std::span<const uint8_t> depth_payload;
};

// Parse and validate a blob produced by serialize(). The returned spans
// point into data. Throws parse_error.
parsed_view parse(std::span<const uint8_t> data);

} // namespace wivrn::frame_synthesis
