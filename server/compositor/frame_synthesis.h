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

// Capture of XR_EXT_frame_synthesis motion vector and depth images submitted
// by the application, for in-band transport with the video stream.

#pragma once

#include "frame_synthesis/aux_frame.h"
#include "utils/gpu_timestamp_pool.h"
#include "vk/allocation.h"

#include <array>
#include <atomic>
#include <optional>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

struct comp_layer;

namespace wivrn
{
struct vk_bundle;

class frame_synthesis_capture
{
public:
	struct settings
	{
		bool enabled = true;
		// Size of the transmitted images relative to the application images
		float scale = 0.5;
		frame_synthesis::depth_format depth = frame_synthesis::depth_format::r32_sfloat;
		int compression_level = 1;
		// Maximum size of the auxiliary data of a view, larger data is dropped
		size_t max_view_size = 2 * 1024 * 1024;
	};

	// Result of the capture of one view, valid once the GPU work is complete
	struct view
	{
		frame_synthesis::view_header header;
		uint32_t buffer_offset; // bytes
	};

	struct slot
	{
		buffer_allocation buffer;
		std::array<std::optional<view>, 2> views;
		// GPU timestamps of the capture are recorded and not yet collected
		bool gpu_time_pending = false;
	};

	struct statistics
	{
		std::atomic<uint64_t> captured_frames = 0;
		std::atomic<uint64_t> rejected_frames = 0;
		std::atomic<uint64_t> oversized_views = 0;
		std::atomic<uint64_t> bytes = 0;
	};

private:
	vk_bundle & vk;
	const settings config;

	vk::raii::Sampler sampler;
	vk::raii::DescriptorSetLayout ds_layout;
	vk::raii::PipelineLayout layout;
	vk::raii::Pipeline pipeline;
	vk::raii::DescriptorPool descriptor_pool;
	std::vector<vk::DescriptorSet> descriptor_sets; // per slot and view

	std::vector<slot> slots;

	// Duration of the capture compute pass, for tracing
	gpu_timestamp_pool timestamps;

	// Last rejection reason, to avoid spamming the log
	std::string last_rejection;
	// Frame being recorded, for tracing
	uint64_t recording_frame = 0;
	void reject(const std::string & reason);

public:
	statistics stats;

	frame_synthesis_capture(vk_bundle &, const settings &, size_t slot_count);

	bool enabled() const
	{
		return config.enabled;
	}

	// Record the copy of the frame synthesis images of layer into the slot
	// buffer. Clears the slot and returns false when the layer cannot be
	// transported (no data, unsupported formats...).
	// Images are expected in shader read only optimal layout.
	bool record(vk::raii::CommandBuffer & cmd,
	            size_t slot_index,
	            const comp_layer & layer,
	            uint64_t frame_index,
	            XrTime display_time,
	            bool color_foveated);

	void clear(size_t slot_index);

	// Whether record() captured data for this view of the slot
	bool has_data(size_t slot_index, size_t view_index) const
	{
		const auto & views = slots.at(slot_index).views;
		return view_index < views.size() and views[view_index];
	}

	// After the GPU work recorded by record() completed, filter, compress
	// and serialize the data of a view. Returns nullopt if there is no data
	// or it exceeds the size limit.
	std::optional<std::vector<uint8_t>> serialize(size_t slot_index, size_t view_index);
};

frame_synthesis_capture::settings frame_synthesis_settings_from_config();

} // namespace wivrn
