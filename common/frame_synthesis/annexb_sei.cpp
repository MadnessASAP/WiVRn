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

#include "annexb_sei.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace wivrn::frame_synthesis
{

namespace
{
constexpr uint8_t sei_payload_type_user_data_unregistered = 5;
constexpr uint8_t fragment_header_version = 1;
// version (1), fragment index (2), fragment count (2), total size (4)
constexpr size_t fragment_header_size = 9;

std::span<const uint8_t> nal_header(video_codec codec)
{
	static const uint8_t h264[] = {0x06};       // nal_ref_idc 0, SEI
	static const uint8_t h265[] = {0x4e, 0x01}; // PREFIX_SEI_NUT, layer 0, tid 1
	switch (codec)
	{
		case video_codec::h264:
			return h264;
		case video_codec::h265:
			return h265;
		case video_codec::av1:
		case video_codec::raw:
			break;
	}
	throw std::invalid_argument("codec does not support SEI");
}

void write_sei_value(std::vector<uint8_t> & out, size_t value)
{
	while (value >= 255)
	{
		out.push_back(0xff);
		value -= 255;
	}
	out.push_back(value);
}

// Insert emulation prevention bytes
void escape(std::span<const uint8_t> rbsp, std::vector<uint8_t> & out)
{
	int zeros = 0;
	for (uint8_t b: rbsp)
	{
		if (zeros >= 2 and b <= 3)
		{
			out.push_back(0x03);
			zeros = 0;
		}
		out.push_back(b);
		zeros = b == 0 ? zeros + 1 : 0;
	}
}

// Remove emulation prevention bytes
std::vector<uint8_t> unescape(std::span<const uint8_t> ebsp)
{
	std::vector<uint8_t> out;
	out.reserve(ebsp.size());
	int zeros = 0;
	for (uint8_t b: ebsp)
	{
		if (zeros >= 2 and b == 0x03)
		{
			zeros = 0;
			continue;
		}
		out.push_back(b);
		zeros = b == 0 ? zeros + 1 : 0;
	}
	return out;
}

size_t start_code_size(std::span<const uint8_t> data)
{
	if (data.size() >= 3 and data[0] == 0 and data[1] == 0 and data[2] == 1)
		return 3;
	if (data.size() >= 4 and data[0] == 0 and data[1] == 0 and data[2] == 0 and data[3] == 1)
		return 4;
	return 0;
}

std::optional<size_t> read_sei_value(std::span<const uint8_t> data, size_t & pos)
{
	size_t value = 0;
	while (pos < data.size())
	{
		uint8_t b = data[pos++];
		value += b;
		if (b != 0xff)
			return value;
	}
	return std::nullopt;
}

template <typename T>
T read_le(const uint8_t * data)
{
	T value = 0;
	for (size_t i = 0; i < sizeof(T); ++i)
		value |= T(data[i]) << (8 * i);
	return value;
}

template <typename T>
void write_le(std::vector<uint8_t> & out, T value)
{
	for (size_t i = 0; i < sizeof(T); ++i)
		out.push_back(uint8_t(value >> (8 * i)));
}
} // namespace

bool supports_sei(video_codec codec)
{
	return codec == video_codec::h264 or codec == video_codec::h265;
}

std::vector<std::vector<uint8_t>> make_sei_nals(video_codec codec, std::span<const uint8_t> blob)
{
	if (blob.size() > max_blob_size)
		throw std::invalid_argument("auxiliary data too large");

	const auto header = nal_header(codec);
	const size_t fragment_count = std::max<size_t>(1, (blob.size() + max_fragment_size - 1) / max_fragment_size);

	std::vector<std::vector<uint8_t>> nals;
	nals.reserve(fragment_count);
	std::vector<uint8_t> rbsp;
	for (size_t i = 0; i < fragment_count; ++i)
	{
		auto fragment = blob.subspan(i * max_fragment_size, std::min(max_fragment_size, blob.size() - i * max_fragment_size));

		rbsp.clear();
		rbsp.push_back(sei_payload_type_user_data_unregistered);
		write_sei_value(rbsp, sei_uuid.size() + fragment_header_size + fragment.size());
		rbsp.insert(rbsp.end(), sei_uuid.begin(), sei_uuid.end());
		rbsp.push_back(fragment_header_version);
		write_le<uint16_t>(rbsp, i);
		write_le<uint16_t>(rbsp, fragment_count);
		write_le<uint32_t>(rbsp, blob.size());
		rbsp.insert(rbsp.end(), fragment.begin(), fragment.end());
		rbsp.push_back(0x80); // rbsp_trailing_bits

		auto & nal = nals.emplace_back();
		nal.reserve(4 + header.size() + rbsp.size() + rbsp.size() / 64);
		nal.insert(nal.end(), {0, 0, 0, 1});
		nal.insert(nal.end(), header.begin(), header.end());
		escape(rbsp, nal);
	}
	return nals;
}

bool starts_with_start_code(std::span<const uint8_t> data)
{
	return start_code_size(data) > 0;
}

bool is_aux_sei_start(video_codec codec, std::span<const uint8_t> data)
{
	if (not supports_sei(codec))
		return false;

	size_t pos = start_code_size(data);
	if (pos == 0)
		return false;

	const auto header = nal_header(codec);
	if (data.size() < pos + header.size() or not std::equal(header.begin(), header.end(), data.begin() + pos))
		return false;
	pos += header.size();

	// payloadType, payloadSize and UUID never need emulation prevention
	auto type = read_sei_value(data, pos);
	if (type != sei_payload_type_user_data_unregistered)
		return false;
	if (not read_sei_value(data, pos))
		return false;

	return data.size() >= pos + sei_uuid.size() and
	       std::equal(sei_uuid.begin(), sei_uuid.end(), data.begin() + pos);
}

void sei_reassembler::reset()
{
	nal.clear();
	blob.clear();
	total_size = 0;
	fragment_count = 0;
	next_fragment = 0;
	failed = false;
}

void sei_reassembler::push(std::span<const uint8_t> data)
{
	if (failed or data.empty())
		return;

	if (starts_with_start_code(data))
		finish_nal();
	else if (nal.empty())
	{
		// Continuation without a start
		failed = true;
		return;
	}

	if (nal.size() + data.size() > max_blob_size + max_blob_size / 2)
	{
		failed = true;
		return;
	}
	nal.insert(nal.end(), data.begin(), data.end());
}

void sei_reassembler::finish_nal()
{
	if (nal.empty() or failed)
		return;

	std::span<const uint8_t> data = nal;
	if (not is_aux_sei_start(codec, data))
	{
		failed = true;
		return;
	}

	size_t pos = start_code_size(data) + nal_header(codec).size();
	auto rbsp = unescape(data.subspan(pos));
	nal.clear();

	pos = 0;
	read_sei_value(rbsp, pos); // payloadType, already checked
	auto payload_size = read_sei_value(rbsp, pos);
	// payload followed by at least the trailing bits
	if (not payload_size or *payload_size < sei_uuid.size() + fragment_header_size or pos + *payload_size >= rbsp.size())
	{
		failed = true;
		return;
	}

	const uint8_t * fragment = rbsp.data() + pos + sei_uuid.size();
	const size_t fragment_size = *payload_size - sei_uuid.size() - fragment_header_size;

	uint8_t version = fragment[0];
	uint16_t index = read_le<uint16_t>(fragment + 1);
	uint16_t count = read_le<uint16_t>(fragment + 3);
	uint32_t total = read_le<uint32_t>(fragment + 5);

	if (version != fragment_header_version or count == 0 or total > max_blob_size)
	{
		failed = true;
		return;
	}

	if (next_fragment == 0)
	{
		fragment_count = count;
		total_size = total;
		blob.reserve(total);
	}

	if (index != next_fragment or count != fragment_count or total != total_size or
	    blob.size() + fragment_size > total_size)
	{
		failed = true;
		return;
	}

	const uint8_t * fragment_data = fragment + fragment_header_size;
	blob.insert(blob.end(), fragment_data, fragment_data + fragment_size);
	++next_fragment;
}

std::optional<std::vector<uint8_t>> sei_reassembler::finish()
{
	finish_nal();

	std::optional<std::vector<uint8_t>> result;
	if (not failed and fragment_count > 0 and next_fragment == fragment_count and blob.size() == total_size)
		result = std::move(blob);

	reset();
	return result;
}

} // namespace wivrn::frame_synthesis
