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

// Decoding of the XR_EXT_frame_synthesis auxiliary frames received in-band
// with the video stream of one eye, into Vulkan images.

#pragma once

#include "frame_synthesis/aux_frame.h"
#include "utils/sync_queue.h"
#include "vk/allocation.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

namespace wivrn
{

class frame_synthesis_decoder
{
	struct slot;

public:
	// Decoded auxiliary images of one view of one frame.
	//
	// The object is created when the frame is received (status pending), and
	// becomes ready once the upload to the GPU has completed. Readers must
	// check status() and, when ready, may sample the images, which are in
	// shader read only optimal layout. semaphore reaches semaphore_value when
	// the upload is complete, readers may wait on it in their submission
	// instead of relying on the CPU side status.
	//
	// The images stay valid and are not overwritten as long as the object is
	// alive.
	struct frame
	{
		enum class state
		{
			pending,
			ready,
			failed,
		};

		const uint64_t frame_index;
		const uint8_t view_index;

		// Valid when ready
		frame_synthesis::view_header header{};
		vk::Extent2D extent{};
		vk::Format motion_format = vk::Format::eUndefined;
		vk::Image motion_image;
		vk::ImageView motion_view;
		vk::Format depth_format = vk::Format::eUndefined;
		vk::Image depth_image;
		vk::ImageView depth_view;
		vk::Semaphore semaphore;
		uint64_t semaphore_value = 0;

		state status() const
		{
			return status_.load(std::memory_order_acquire);
		}

		frame(uint64_t frame_index, uint8_t view_index) :
		        frame_index(frame_index), view_index(view_index) {}

	private:
		friend class frame_synthesis_decoder;
		std::atomic<state> status_ = state::pending;
		std::shared_ptr<slot> owner;
	};

	struct statistics
	{
		std::atomic<uint64_t> received = 0;
		std::atomic<uint64_t> decoded = 0;
		std::atomic<uint64_t> errors = 0;
		std::atomic<uint64_t> dropped = 0;
		std::atomic<uint64_t> bytes = 0;
		// exponential moving average of the CPU decode time
		std::atomic<float> decode_ms = 0;
	};

private:
	struct slot
	{
		std::atomic<bool> in_use = false;
		vk::Extent2D extent{};
		vk::Format motion_format = vk::Format::eUndefined;
		vk::Format depth_format = vk::Format::eUndefined;
		image_allocation motion;
		vk::raii::ImageView motion_view = nullptr;
		image_allocation depth;
		vk::raii::ImageView depth_view = nullptr;
	};

	struct job
	{
		std::shared_ptr<frame> target;
		std::vector<uint8_t> blob;
	};

	vk::raii::Device & device;
	const uint8_t view_index;

	// System trace names, per view
	const std::string trace_latency;
	const std::string trace_bytes;

	// Images replaced after a size or format change, kept until the GPU
	// can no longer use them
	struct retired_image
	{
		uint64_t until;
		image_allocation image;
		vk::raii::ImageView view;
	};

	// Resources referenced by decoded frames. Frames keep the pool alive, so
	// that they stay valid if the decoder is destroyed first (when the stream
	// is reconfigured).
	struct resource_pool
	{
		// Frames are referenced by the recent list until attached to the
		// decoded colour image, then by the stream scene: keep enough slots
		// for both.
		std::array<slot, 8> slots;
		std::vector<retired_image> retired;
		vk::raii::Semaphore semaphore = nullptr;
	};

	vk::raii::CommandPool command_pool;
	vk::raii::CommandBuffer cmd;
	vk::raii::Fence fence;
	uint64_t semaphore_value = 0;
	buffer_allocation staging;
	std::shared_ptr<resource_pool> pool;
	uint64_t decode_count = 0;

	std::mutex recent_mutex;
	std::array<std::shared_ptr<frame>, 4> recent;

	utils::sync_queue<job> jobs;
	std::thread worker;

	void run();
	void decode(job &);
	std::shared_ptr<slot> acquire_slot();
	void prepare_slot(slot &, vk::Extent2D, vk::Format motion, vk::Format depth);

public:
	statistics stats;

	frame_synthesis_decoder(vk::raii::Device & device, uint32_t queue_family_index, uint8_t view_index);
	~frame_synthesis_decoder();

	// Called from the network thread when the auxiliary data of a frame has
	// been received, before the frame is sent to the video decoder.
	void push(uint64_t frame_index, std::vector<uint8_t> && blob);

	// Auxiliary images of a frame, nullptr if none were received
	std::shared_ptr<frame> get(uint64_t frame_index);
};

} // namespace wivrn
