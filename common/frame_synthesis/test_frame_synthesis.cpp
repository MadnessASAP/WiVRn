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

// Round-trip tests for the auxiliary frame transport.
// Usage: test_frame_synthesis [output.h264]
// When given a file name, writes the SEI NAL units of a sample frame so that
// they can be spliced into a real bitstream for conformance checks.

#include "frame_synthesis/annexb_sei.h"
#include "frame_synthesis/aux_codec.h"
#include "frame_synthesis/aux_frame.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <vector>

using namespace wivrn;
using namespace wivrn::frame_synthesis;

static int failures = 0;

#define CHECK(x)                                                                              \
	do                                                                                    \
	{                                                                                     \
		if (not(x))                                                                   \
		{                                                                             \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
			++failures;                                                           \
		}                                                                             \
	} while (false)

static uint16_t to_half(float f)
{
	uint32_t x;
	memcpy(&x, &f, 4);
	uint32_t sign = (x >> 16) & 0x8000;
	int32_t exp = int32_t((x >> 23) & 0xff) - 127 + 15;
	uint32_t mant = (x >> 13) & 0x3ff;
	if (exp <= 0)
		return sign;
	if (exp >= 31)
		return sign | 0x7c00;
	return sign | (exp << 10) | mant;
}

struct sample_images
{
	uint32_t w, h;
	std::vector<uint8_t> motion; // rg16, 4 bytes per pixel
	std::vector<uint8_t> depth;  // r32 in 32-bit words
};

static sample_images make_images(uint32_t w, uint32_t h, bool noisy)
{
	sample_images s{w, h};
	s.motion.resize(w * h * 4);
	s.depth.resize(w * h * 4);
	std::mt19937 rng(42);
	std::uniform_real_distribution<float> noise(-0.001, 0.001);
	for (uint32_t y = 0; y < h; ++y)
	{
		for (uint32_t x = 0; x < w; ++x)
		{
			size_t i = y * w + x;
			// a sphere in front of a plane
			float dx = (x - w / 2.f) / w, dy = (y - h / 2.f) / h;
			bool sphere = dx * dx + dy * dy < 0.05;
			float depth = sphere ? 0.3f + dx * dx : 0.9f + 0.01f * dy;
			float mx = sphere ? 0.01f : 0.002f, my = sphere ? -0.003f : 0.f;
			if (noisy)
			{
				depth += noise(rng);
				mx += noise(rng);
			}
			uint16_t m[2] = {to_half(mx), to_half(my)};
			memcpy(&s.motion[i * 4], m, 4);
			memcpy(&s.depth[i * 4], &depth, 4);
		}
	}
	return s;
}

static view_header make_header(const sample_images & s, depth_format df)
{
	view_header h{};
	h.version = format_version;
	h.frame_index = 1234;
	h.view_index = 1;
	h.display_time = 987654321;
	h.app_display_time = 987000000;
	h.previous_app_display_time = 975888889;
	h.previous_frame_index = 1233;
	h.layer_flags = 1;
	h.motion_vector_scale = {1, 1, 1};
	h.motion_vector_offset = {0, 0, 0};
	h.app_space_delta_pose = {{0, 0, 0, 1}, {0, 0, 0}};
	h.layer_space_pose = {{0, 0, 0, 1}, {0, 1.6, 0}};
	h.min_depth = 0;
	h.max_depth = 1;
	h.near_z = 0.1;
	h.far_z = INFINITY;
	h.color_width = s.w * 2;
	h.color_height = s.h * 2;
	h.motion_encoding = motion_format::rg16_sfloat;
	h.depth_encoding = df;
	h.motion = {
	        .source_width = s.w * 2,
	        .source_height = s.h * 2,
	        .source_format = 97, // VK_FORMAT_R16G16B16A16_SFLOAT
	        .width = uint16_t(s.w),
	        .height = uint16_t(s.h),
	        .filter_mode = filter::delta_byte_planes,
	        .compression_mode = compression::zstd,
	        .uncompressed_size = s.w * s.h * 4,
	};
	h.depth = h.motion;
	h.depth.source_format = 126; // VK_FORMAT_D32_SFLOAT
	h.depth.uncompressed_size = uint32_t(s.w * s.h * bytes_per_pixel(df));
	return h;
}

static void test_codec(const sample_images & s)
{
	for (auto f: {filter::none, filter::delta_byte_planes})
	{
		for (auto c: {compression::none, compression::zstd})
		{
			for (uint32_t component_size: {2u, 4u})
			{
				// depth as r32 (4 bytes) or r16 (2 low bytes of each 32-bit word)
				image_layout layout{s.w, s.h, 1, component_size, 4};
				std::vector<uint8_t> out;
				size_t size = compress(layout, s.depth, f, c, 1, out);
				CHECK(size == out.size());

				std::vector<uint8_t> decoded(layout.packed_size());
				image_layout packed = layout;
				packed.pixel_stride = component_size;
				decompress(packed, out, f, c, decoded);

				bool equal = true;
				for (size_t i = 0; i < size_t(s.w) * s.h; ++i)
					equal &= memcmp(&decoded[i * component_size], &s.depth[i * 4], component_size) == 0;
				CHECK(equal);

				if (c == compression::zstd and component_size == 4)
					printf("depth r32 filter %d: %zu -> %zu bytes (%.1fx)\n", int(f), layout.packed_size(), size, double(layout.packed_size()) / size);
			}

			image_layout layout{s.w, s.h, 2, 2, 4};
			std::vector<uint8_t> out;
			size_t size = compress(layout, s.motion, f, c, 1, out);
			std::vector<uint8_t> decoded(layout.packed_size());
			decompress(layout, out, f, c, decoded);
			CHECK(decoded == s.motion);
			if (c == compression::zstd)
				printf("motion rg16 filter %d: %zu -> %zu bytes (%.1fx)\n", int(f), layout.packed_size(), size, double(layout.packed_size()) / size);

			// corrupted input must throw parse_error, not crash
			if (c == compression::zstd and out.size() > 16)
			{
				auto bad = out;
				bad.resize(bad.size() / 2);
				bool threw = false;
				try
				{
					decompress(layout, bad, f, c, decoded);
				}
				catch (parse_error &)
				{
					threw = true;
				}
				CHECK(threw);
			}
		}
	}
}

static std::vector<uint8_t> make_blob(const sample_images & s, depth_format df)
{
	auto h = make_header(s, df);
	std::vector<uint8_t> motion, depth;
	h.motion.compressed_size = compress({s.w, s.h, 2, 2, 4}, s.motion, h.motion.filter_mode, h.motion.compression_mode, 1, motion);
	h.depth.compressed_size = compress({s.w, s.h, 1, uint32_t(bytes_per_pixel(df)), 4}, s.depth, h.depth.filter_mode, h.depth.compression_mode, 1, depth);
	return serialize(h, motion, depth);
}

static void test_frame(const sample_images & s)
{
	for (auto df: {depth_format::r32_sfloat, depth_format::r16_sfloat})
	{
		auto blob = make_blob(s, df);
		auto parsed = parse(blob);
		CHECK(parsed.header.frame_index == 1234);
		CHECK(parsed.header.view_index == 1);
		CHECK(parsed.header.app_display_time == 987000000);
		CHECK(parsed.header.previous_app_display_time == 975888889);
		CHECK(parsed.header.previous_frame_index == 1233);
		CHECK(std::isinf(parsed.header.far_z));
		CHECK(parsed.header.depth_encoding == df);

		std::vector<uint8_t> motion(parsed.header.motion.uncompressed_size);
		decompress({s.w, s.h, 2, 2, 4}, parsed.motion_payload, parsed.header.motion.filter_mode, parsed.header.motion.compression_mode, motion);
		CHECK(motion == s.motion);

		// truncations must be rejected
		for (size_t len: {size_t(0), size_t(10), blob.size() / 2, blob.size() - 1})
		{
			bool threw = false;
			try
			{
				parse(std::span(blob).first(len));
			}
			catch (parse_error &)
			{
				threw = true;
			}
			CHECK(threw);
		}
	}
}

// Split NAL units in shards like video_encoder::SendData: each NAL unit is
// sent by a separate call, so that it starts a new shard.
static std::vector<std::vector<uint8_t>> shard(const std::vector<std::vector<uint8_t>> & nals, size_t shard_size)
{
	std::vector<std::vector<uint8_t>> shards;
	for (const auto & nal: nals)
		for (size_t i = 0; i < nal.size(); i += shard_size)
			shards.emplace_back(nal.begin() + i, nal.begin() + std::min(nal.size(), i + shard_size));
	return shards;
}

static void test_sei(const sample_images & s, video_codec codec)
{
	auto blob = make_blob(s, depth_format::r32_sfloat);
	auto nals = make_sei_nals(codec, blob);
	CHECK(nals.size() == (blob.size() + max_fragment_size - 1) / max_fragment_size);

	for (const auto & nal: nals)
	{
		CHECK(is_aux_sei_start(codec, nal));
		// no start code emulation inside the NAL unit
		for (size_t i = 4; i + 2 < nal.size(); ++i)
			CHECK(not(nal[i] == 0 and nal[i + 1] == 0 and nal[i + 2] <= 3 and nal[i + 2] != 3));
	}

	// Fake VCL NAL after the SEI
	std::vector<uint8_t> vcl = {0, 0, 0, 1, uint8_t(codec == video_codec::h264 ? 0x65 : 0x26), 0x88, 0x84, 0x00};
	auto all = nals;
	all.push_back(vcl);

	for (size_t shard_size: {size_t(1100), size_t(1400), size_t(65536)})
	{
		sei_reassembler r(codec);
		std::vector<uint8_t> forwarded;
		bool in_aux = false;
		for (const auto & data: shard(all, shard_size))
		{
			if (starts_with_start_code(data))
				in_aux = is_aux_sei_start(codec, data);
			if (in_aux)
				r.push(data);
			else
				forwarded.insert(forwarded.end(), data.begin(), data.end());
		}
		auto result = r.finish();
		CHECK(result and *result == blob);
		CHECK(forwarded == vcl);
	}

	// Missing fragment
	if (nals.size() > 1)
	{
		sei_reassembler r(codec);
		for (size_t i = 1; i < nals.size(); ++i)
			r.push(nals[i]);
		CHECK(not r.finish());
	}

	// Corrupted bytes must not crash
	std::mt19937 rng(1);
	for (int iter = 0; iter < 200; ++iter)
	{
		sei_reassembler r(codec);
		for (auto nal: nals)
		{
			for (int k = 0; k < 4; ++k)
				nal[4 + rng() % (nal.size() - 4)] ^= uint8_t(1 + rng() % 255);
			r.push(nal);
		}
		auto result = r.finish();
		if (result)
		{
			try
			{
				parse(*result);
			}
			catch (parse_error &)
			{}
		}
	}

	// Wrong codec is not recognised
	auto other = codec == video_codec::h264 ? video_codec::h265 : video_codec::h264;
	CHECK(not is_aux_sei_start(other, nals[0]));
}

int main(int argc, char ** argv)
{
	CHECK(not supports_sei(video_codec::av1));
	CHECK(supports_sei(video_codec::h264));

	for (bool noisy: {false, true})
	{
		auto s = make_images(720, 768, noisy);
		printf("%s images %ux%u\n", noisy ? "noisy" : "smooth", s.w, s.h);
		test_codec(s);
		test_frame(s);
		test_sei(s, video_codec::h264);
		test_sei(s, video_codec::h265);
	}

	if (argc > 1)
	{
		auto s = make_images(256, 256, true);
		std::ofstream out(argv[1], std::ios::binary);
		for (const auto & nal: make_sei_nals(video_codec::h264, make_blob(s, depth_format::r16_sfloat)))
			out.write((const char *)nal.data(), nal.size());
	}

	if (failures)
	{
		fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
