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

// Forwarding of the received XR_EXT_frame_synthesis images (depth and motion
// vectors) to the headset runtime's XR_EXT_frame_synthesis: the images are
// copied into runtime swapchains and described by an XrFrameSynthesisInfoEXT
// chained to each view of the stream projection layer.

#pragma once

#include "decoder/frame_synthesis_decoder.h"
#include "xr/swapchain.h"

#include <array>
#include <memory>
#include <optional>
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include <openxr/openxr.h>

namespace xr
{
class instance;
class session;
class system;
} // namespace xr

class stream_frame_synthesis
{
public:
	static constexpr uint32_t view_count = 2;
	using frame = wivrn::frame_synthesis_decoder::frame;

	// Why the data of a submitted frame was or was not forwarded
	enum class result
	{
		forwarded,
		no_data,   // a view has no frame synthesis data
		not_ready, // decoding is not complete or failed
		mismatch,  // the views belong to different frames
		repeated,  // the colour image was already submitted
	};
	static constexpr size_t result_count = 5;

private:
	vk::raii::Device & device;

	xr::swapchain motion_swapchain;
	xr::swapchain depth_swapchain;
	vk::Extent2D extent;

	vk::raii::Sampler sampler = nullptr;
	vk::raii::RenderPass renderpass = nullptr;
	vk::raii::DescriptorSetLayout descriptor_set_layout = nullptr;
	vk::raii::DescriptorPool ds_pool = nullptr;
	std::array<vk::DescriptorSet, view_count> ds;
	vk::raii::PipelineLayout layout = nullptr;
	vk::raii::Pipeline pipeline = nullptr;

	// Per swapchain image and view
	std::vector<vk::raii::ImageView> image_views;
	std::vector<vk::raii::Framebuffer> framebuffers;

	std::array<XrFrameSynthesisInfoEXT, view_count> infos{};
	bool acquired = false;

	stream_frame_synthesis(xr::instance &, xr::session &, vk::raii::Device &, vk::Extent2D extent, vk::Format depth_format);

public:
	// nullptr if the runtime does not support the required formats
	static std::unique_ptr<stream_frame_synthesis> create(
	        xr::instance &,
	        xr::session &,
	        xr::system &,
	        vk::raii::Device &,
	        XrViewConfigurationType);

	stream_frame_synthesis(const stream_frame_synthesis &) = delete;
	stream_frame_synthesis & operator=(const stream_frame_synthesis &) = delete;

	// Whether the data of both views can be forwarded with a colour image
	// submitted for the first time (repeated = false)
	static result check(const std::array<std::shared_ptr<frame>, view_count> & frames, bool repeated);

	// Acquire the swapchain images and record the copy of both views, the
	// frames must have passed check(). The submission must wait for the
	// frames' semaphores.
	void record(vk::raii::CommandBuffer &, const std::array<std::shared_ptr<frame>, view_count> & frames);

	// Release the swapchain images, after the submission
	void release();

	// Fill the frame synthesis information of a view from its header, valid
	// until the next call. The stream layer must be submitted in the space of
	// view_info_t::pose. flip_y negates the Y component of the motion vectors.
	XrFrameSynthesisInfoEXT * info(uint32_t view, const frame &, bool flip_y);

	vk::Extent2D size() const
	{
		return extent;
	}
};
