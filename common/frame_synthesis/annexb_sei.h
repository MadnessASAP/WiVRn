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

// In-band encapsulation of auxiliary frames in H.264 / H.265 Annex B
// bitstreams, as "user data unregistered" SEI messages (payloadType 5).
//
// An auxiliary blob is split in fragments, each fragment is one SEI NAL unit:
//   start code (00 00 00 01)
//   NAL header (H.264: 0x06, H.265: PREFIX_SEI 0x4e 0x01)
//   payloadType = 5, payloadSize
//   uuid (16 bytes, wivrn_aux_uuid)
//   fragment header: magic version, fragment index, fragment count, total size
//   fragment data
//   rbsp_trailing_bits (0x80)
// with emulation prevention bytes inserted after the NAL header.
//
// Decoders ignore unknown user data SEI, the client still removes them
// before decoding because of their size.

#pragma once

#include "wivrn_packets.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace wivrn::frame_synthesis
{

// Randomly generated, contains no zero byte so that it is never subject
// to emulation prevention and can be matched on the raw bitstream.
inline constexpr std::array<uint8_t, 16> sei_uuid = {
        0x8b,
        0x2f,
        0x5d,
        0xc1,
        0x47,
        0xe6,
        0x4a,
        0x93,
        0xb5,
        0x1c,
        0x6e,
        0xd2,
        0x39,
        0xa7,
        0xf4,
        0x58};

// Maximum size of the data in a single SEI NAL unit. Keeps the payloadSize
// field small enough for the whole SEI header to fit in the first shard.
inline constexpr size_t max_fragment_size = 60000;

// Maximum size of an auxiliary blob accepted by the reassembler.
inline constexpr size_t max_blob_size = 16 * 1024 * 1024;

bool supports_sei(video_codec);

// Build the SEI NAL units (each with a 4 bytes start code) carrying blob.
std::vector<std::vector<uint8_t>> make_sei_nals(video_codec, std::span<const uint8_t> blob);

// Returns true if data starts with an Annex B start code (3 or 4 bytes).
bool starts_with_start_code(std::span<const uint8_t> data);

// Returns true if data starts with a start code followed by a WiVRn
// auxiliary SEI NAL header (up to and including the UUID).
bool is_aux_sei_start(video_codec, std::span<const uint8_t> data);

// Reassembles the fragments of one auxiliary blob from whole SEI NAL units.
class sei_reassembler
{
	video_codec codec = video_codec::h264;
	std::vector<uint8_t> nal;  // current NAL unit, escaped
	std::vector<uint8_t> blob; // reassembled data
	uint32_t total_size = 0;
	uint16_t fragment_count = 0;
	uint16_t next_fragment = 0;
	bool failed = false;

	void finish_nal();

public:
	sei_reassembler() = default;
	explicit sei_reassembler(video_codec codec) :
	        codec(codec) {}

	void reset();

	// Feed bytes belonging to auxiliary SEI NAL units, starting with the
	// start code of a NAL unit, in bitstream order.
	// A call starting with a start code begins a new NAL unit.
	void push(std::span<const uint8_t> data);

	// Returns the reassembled blob if all fragments were received and are
	// consistent, then resets.
	std::optional<std::vector<uint8_t>> finish();

	bool has_data() const
	{
		return not nal.empty() or not blob.empty() or fragment_count != 0;
	}
};

} // namespace wivrn::frame_synthesis
