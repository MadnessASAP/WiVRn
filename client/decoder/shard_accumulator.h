/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#pragma once

#include "decoder.h"
#include "frame_synthesis/annexb_sei.h"
#include "frame_synthesis_decoder.h"
#include "wivrn_packets.h"

#include <memory>
#include <optional>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

namespace xr
{
class instance;
}

namespace scenes
{
class stream;
}

namespace wivrn
{

class shard_accumulator
{
	std::shared_ptr<decoder> decoder_;

public:
	using data_shard = wivrn::to_headset::video_stream_data_shard;
	struct shard_set
	{
		size_t min_for_reconstruction = -1;
		std::vector<std::optional<data_shard>> data;
		void reset(uint64_t frame_index);
		bool empty() const;

		std::optional<uint16_t> insert(data_shard &&, xr::instance & instance);

		wivrn::from_headset::feedback feedback{};

		// In-band XR_EXT_frame_synthesis data of the frame
		frame_synthesis::sei_reassembler aux;
		// Whether the last shard pushed to the decoder belongs to an
		// auxiliary data SEI NAL unit
		bool in_aux = false;

		explicit shard_set(uint8_t stream_index);
		shard_set(const shard_set &) = default;
		shard_set(shard_set &&) = default;
		shard_set & operator=(const shard_set &) = default;
		shard_set & operator=(shard_set &&) = default;

		uint64_t frame_index() const
		{
			return feedback.frame_index;
		}
	};

private:
	const video_codec codec;
	shard_set current;
	shard_set next;
	std::weak_ptr<scenes::stream> weak_scene;
	xr::instance & instance;
	std::unique_ptr<frame_synthesis_decoder> fs_decoder;

public:
	explicit shard_accumulator(
	        vk::raii::Device & device,
	        vk::raii::PhysicalDevice & physical_device,
	        xr::instance & instance,
	        uint32_t vk_queue_family_index,
	        const wivrn::to_headset::video_stream_description & description,
	        std::weak_ptr<scenes::stream> scene,
	        uint8_t stream_index) :
	        decoder_(decoder::make(device, physical_device, vk_queue_family_index, description, stream_index, scene, this)),
	        codec(description.codec[stream_index]),
	        current(stream_index),
	        next(stream_index),
	        weak_scene(scene),
	        instance(instance)
	{
		current.aux = frame_synthesis::sei_reassembler(codec);
		next.aux = frame_synthesis::sei_reassembler(codec);
		next.reset(1);
		if (stream_index < 2 and frame_synthesis::supports_sei(codec))
			fs_decoder = std::make_unique<frame_synthesis_decoder>(device, vk_queue_family_index, stream_index);
	}

	void push_shard(wivrn::to_headset::video_stream_data_shard &&);

	vk::Sampler sampler()
	{
		return decoder_->sampler();
	}

	// XR_EXT_frame_synthesis images received with a frame, if any
	std::shared_ptr<frame_synthesis_decoder::frame> frame_synthesis(uint64_t frame_index)
	{
		return fs_decoder ? fs_decoder->get(frame_index) : nullptr;
	}

	const frame_synthesis_decoder::statistics * frame_synthesis_stats() const
	{
		return fs_decoder ? &fs_decoder->stats : nullptr;
	}

	using blit_handle = decoder::blit_handle;

private:
	void try_submit_frame(std::optional<uint16_t> shard_idx);
	void try_submit_frame(uint16_t shard_idx);
	void send_feedback(wivrn::from_headset::feedback & feedback);
	void advance();
};
} // namespace wivrn
