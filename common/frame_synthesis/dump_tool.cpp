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

// Extract the XR_EXT_frame_synthesis auxiliary frames from a video stream
// dumped with WIVRN_DUMP_VIDEO, using the same parser as the headset.
//
// Usage: wivrn-frame-synthesis-dump <h264|h265> <stream file> [output prefix]
//
// Prints the metadata of each auxiliary frame, and when an output prefix is
// given, writes the depth and motion vector images as PFM files
// (<prefix>-<frame>-depth.pfm, <prefix>-<frame>-motion.pfm). Motion vector
// images contain the values after motionVectorScale/motionVectorOffset.

#include "frame_synthesis/annexb_sei.h"
#include "frame_synthesis/aux_codec.h"
#include "frame_synthesis/aux_frame.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace wivrn;
using namespace wivrn::frame_synthesis;

static float half_to_float(uint16_t h)
{
	uint32_t sign = uint32_t(h & 0x8000) << 16;
	uint32_t exp = (h >> 10) & 0x1f;
	uint32_t mant = h & 0x3ff;
	uint32_t bits;
	if (exp == 0)
	{
		if (mant == 0)
			bits = sign;
		else
		{
			// subnormal
			int e = -1;
			do
			{
				mant <<= 1;
				++e;
			} while ((mant & 0x400) == 0);
			bits = sign | uint32_t(127 - 15 - e) << 23 | (mant & 0x3ff) << 13;
		}
	}
	else if (exp == 31)
		bits = sign | 0x7f800000 | mant << 13;
	else
		bits = sign | (exp + 127 - 15) << 23 | mant << 13;
	float f;
	memcpy(&f, &bits, 4);
	return f;
}

static void write_pfm(const std::string & file, uint32_t w, uint32_t h, int channels, const std::vector<float> & data)
{
	std::ofstream out(file, std::ios::binary);
	out << (channels == 3 ? "PF" : "Pf") << "\n"
	    << w << " " << h << "\n-1.0\n";
	// PFM rows are stored bottom to top
	for (uint32_t y = h; y-- > 0;)
		out.write((const char *)&data[size_t(y) * w * channels], size_t(w) * channels * sizeof(float));
}

// Split an Annex B stream in NAL units, each including its start code
static std::vector<std::span<const uint8_t>> split_nals(std::span<const uint8_t> data)
{
	std::vector<size_t> starts;
	for (size_t i = 0; i + 3 <= data.size(); ++i)
	{
		if (data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1)
		{
			starts.push_back(i > 0 and data[i - 1] == 0 ? i - 1 : i);
			i += 2;
		}
	}
	std::vector<std::span<const uint8_t>> nals;
	for (size_t k = 0; k < starts.size(); ++k)
	{
		size_t end = k + 1 < starts.size() ? starts[k + 1] : data.size();
		nals.push_back(data.subspan(starts[k], end - starts[k]));
	}
	return nals;
}

static void process(const std::vector<uint8_t> & blob, const char * prefix, size_t & count)
{
	auto parsed = parse(blob);
	const auto & h = parsed.header;
	const uint32_t w = h.depth.width, ht = h.depth.height;
	const size_t pixels = size_t(w) * ht;

	std::vector<uint8_t> motion(h.motion.uncompressed_size);
	std::vector<uint8_t> depth(h.depth.uncompressed_size);
	const uint32_t mc = components(h.motion_encoding);
	const uint32_t dbpp = bytes_per_pixel(h.depth_encoding);
	decompress({w, ht, mc, 2, mc * 2}, parsed.motion_payload, h.motion.filter_mode, h.motion.compression_mode, motion);
	decompress({w, ht, 1, dbpp, dbpp}, parsed.depth_payload, h.depth.filter_mode, h.depth.compression_mode, depth);

	const std::array<float, 3> scale{h.motion_vector_scale.x, h.motion_vector_scale.y, h.motion_vector_scale.z};
	const std::array<float, 3> offset{h.motion_vector_offset.x, h.motion_vector_offset.y, h.motion_vector_offset.z};

	std::vector<float> d(pixels), m(pixels * 3);
	float dmin = INFINITY, dmax = -INFINITY, mmax = 0;
	for (size_t i = 0; i < pixels; ++i)
	{
		if (dbpp == 4)
			memcpy(&d[i], &depth[i * 4], 4);
		else
		{
			uint16_t v;
			memcpy(&v, &depth[i * 2], 2);
			d[i] = half_to_float(v);
		}
		dmin = std::min(dmin, d[i]);
		dmax = std::max(dmax, d[i]);

		for (uint32_t c = 0; c < 3; ++c)
		{
			float v = 0;
			if (c < mc)
			{
				uint16_t raw;
				memcpy(&raw, &motion[(i * mc + c) * 2], 2);
				v = half_to_float(raw) * scale[c] + offset[c];
			}
			// 2D motion vectors only use rg
			if (c == 2 and (h.layer_flags & XR_FRAME_SYNTHESIS_INFO_USE_2D_MOTION_VECTOR_BIT_EXT))
				v = 0;
			m[i * 3 + c] = v;
		}
		mmax = std::max(mmax, std::hypot(m[i * 3], m[i * 3 + 1]));
	}

	std::cout << std::format(
	        "frame {} view {} {}x{} (source depth {}x{} fmt {}, motion {}x{} fmt {}, color {}x{}) flags {:#x} layer_flags {:#x} "
	        "depth [{}, {}] near {} far {} values [{}, {}] max |mv.xy| {} bytes {} (motion {} depth {})\n",
	        h.frame_index,
	        h.view_index,
	        w,
	        ht,
	        h.depth.source_width,
	        h.depth.source_height,
	        h.depth.source_format,
	        h.motion.source_width,
	        h.motion.source_height,
	        h.motion.source_format,
	        h.color_width,
	        h.color_height,
	        h.flags,
	        h.layer_flags,
	        h.min_depth,
	        h.max_depth,
	        h.near_z,
	        h.far_z,
	        dmin,
	        dmax,
	        mmax,
	        blob.size(),
	        h.motion.compressed_size,
	        h.depth.compressed_size);

	if (prefix)
	{
		write_pfm(std::format("{}-{}-depth.pfm", prefix, h.frame_index), w, ht, 1, d);
		write_pfm(std::format("{}-{}-motion.pfm", prefix, h.frame_index), w, ht, 3, m);
	}
	++count;
}

int main(int argc, char ** argv)
{
	if (argc < 3)
	{
		std::cerr << "Usage: " << argv[0] << " <h264|h265> <stream file> [output prefix]\n";
		return 2;
	}

	video_codec codec;
	if (std::string_view(argv[1]) == "h264")
		codec = video_codec::h264;
	else if (std::string_view(argv[1]) == "h265")
		codec = video_codec::h265;
	else
	{
		std::cerr << "unsupported codec " << argv[1] << "\n";
		return 2;
	}

	std::ifstream in(argv[2], std::ios::binary);
	if (not in)
	{
		std::cerr << "cannot open " << argv[2] << "\n";
		return 2;
	}
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	const char * prefix = argc > 3 ? argv[3] : nullptr;

	sei_reassembler reassembler(codec);
	size_t count = 0, errors = 0, other_nals = 0;
	bool in_aux = false;
	auto flush = [&]() {
		if (not reassembler.has_data())
			return;
		auto blob = reassembler.finish();
		if (not blob)
		{
			++errors;
			std::cout << "incomplete or corrupted auxiliary data\n";
			return;
		}
		try
		{
			process(*blob, prefix, count);
		}
		catch (std::exception & e)
		{
			++errors;
			std::cout << "invalid auxiliary data: " << e.what() << "\n";
		}
	};

	for (auto nal: split_nals(data))
	{
		bool aux = is_aux_sei_start(codec, nal);
		if (aux)
			reassembler.push(nal);
		else
		{
			// the auxiliary data of a frame ends with the first other NAL unit
			if (in_aux)
				flush();
			++other_nals;
		}
		in_aux = aux;
	}
	flush();

	std::cout << std::format("{} auxiliary frames, {} errors, {} other NAL units\n", count, errors, other_nals);
	return errors ? 1 : 0;
}
