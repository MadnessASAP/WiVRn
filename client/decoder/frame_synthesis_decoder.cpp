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

#include "frame_synthesis_decoder.h"

#include "application.h"
#include "frame_synthesis/aux_codec.h"
#include "utils/atrace.h"
#include "utils/named_thread.h"

#include <chrono>
#include <format>
#include <spdlog/spdlog.h>

namespace wivrn
{

namespace
{
using namespace frame_synthesis;

vk::Format vk_format(motion_format f)
{
	switch (f)
	{
		case motion_format::rg16_sfloat:
			return vk::Format::eR16G16Sfloat;
		case motion_format::rgba16_sfloat:
			return vk::Format::eR16G16B16A16Sfloat;
	}
	throw parse_error("invalid motion format");
}

vk::Format vk_format(depth_format f)
{
	switch (f)
	{
		case depth_format::r32_sfloat:
			return vk::Format::eR32Sfloat;
		case depth_format::r16_sfloat:
			return vk::Format::eR16Sfloat;
	}
	throw parse_error("invalid depth format");
}

std::pair<image_allocation, vk::raii::ImageView> make_image(vk::raii::Device & device, vk::Extent2D extent, vk::Format format, const char * name)
{
	image_allocation image{
	        device,
	        vk::ImageCreateInfo{
	                .imageType = vk::ImageType::e2D,
	                .format = format,
	                .extent = {extent.width, extent.height, 1},
	                .mipLevels = 1,
	                .arrayLayers = 1,
	                .samples = vk::SampleCountFlagBits::e1,
	                .tiling = vk::ImageTiling::eOptimal,
	                .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
	        },
	        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
	        name,
	};
	vk::raii::ImageView view{
	        device,
	        vk::ImageViewCreateInfo{
	                .image = image,
	                .viewType = vk::ImageViewType::e2D,
	                .format = format,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = 1,
	                },
	        },
	};
	return {std::move(image), std::move(view)};
}
} // namespace

frame_synthesis_decoder::frame_synthesis_decoder(vk::raii::Device & device, uint32_t queue_family_index, uint8_t view_index) :
        device(device),
        view_index(view_index),
        trace_latency(std::format("frame synthesis latency {}", view_index)),
        trace_bytes(std::format("frame synthesis bytes {}", view_index)),
        command_pool(device, vk::CommandPoolCreateInfo{
                                     .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                     .queueFamilyIndex = queue_family_index,
                             }),
        cmd(std::move(device.allocateCommandBuffers({
                .commandPool = *command_pool,
                .commandBufferCount = 1,
        })[0])),
        fence(device, vk::FenceCreateInfo{}),
        pool(std::make_shared<resource_pool>())
{
	pool->semaphore = vk::raii::Semaphore(device, vk::StructureChain{
	                                                      vk::SemaphoreCreateInfo{},
	                                                      vk::SemaphoreTypeCreateInfo{
	                                                              .semaphoreType = vk::SemaphoreType::eTimeline,
	                                                      },
	                                              }
	                                                      .get());
	worker = utils::named_thread("frame-synthesis-" + std::to_string(view_index), [this]() { run(); });
}

frame_synthesis_decoder::~frame_synthesis_decoder()
{
	jobs.close();
	if (worker.joinable())
		worker.join();
}

void frame_synthesis_decoder::push(uint64_t frame_index, std::vector<uint8_t> && blob)
{
	++stats.received;
	stats.bytes += blob.size();
	// From the reception of the data to the end of the upload
	atrace::async_begin(trace_latency.c_str(), int32_t(frame_index));
	atrace::counter(trace_bytes.c_str(), blob.size());

	auto f = std::make_shared<frame>(frame_index, view_index);
	{
		std::unique_lock lock(recent_mutex);
		recent[frame_index % recent.size()] = f;
	}
	jobs.push(job{
	        .target = std::move(f),
	        .blob = std::move(blob),
	});
}

std::shared_ptr<frame_synthesis_decoder::frame> frame_synthesis_decoder::get(uint64_t frame_index)
{
	std::unique_lock lock(recent_mutex);
	auto & f = recent[frame_index % recent.size()];
	if (f and f->frame_index == frame_index)
		return f;
	return nullptr;
}

void frame_synthesis_decoder::run()
{
	while (true)
	{
		job j;
		try
		{
			j = jobs.pop();
		}
		catch (utils::sync_queue_closed &)
		{
			return;
		}

		try
		{
			atrace::section trace("frame synthesis decode");
			decode(j);
		}
		catch (std::exception & e)
		{
			++stats.errors;
			j.target->status_.store(frame::state::failed, std::memory_order_release);
			spdlog::warn("Invalid frame synthesis data for frame {} view {}: {}", j.target->frame_index, view_index, e.what());
		}
		atrace::async_end(trace_latency.c_str(), int32_t(j.target->frame_index));
	}
}

std::shared_ptr<frame_synthesis_decoder::slot> frame_synthesis_decoder::acquire_slot()
{
	for (auto & s: pool->slots)
	{
		if (not s.in_use.exchange(true))
			return std::shared_ptr<slot>(&s, [pool = pool](slot * s) { s->in_use = false; });
	}
	return nullptr;
}

void frame_synthesis_decoder::prepare_slot(slot & s, vk::Extent2D extent, vk::Format motion, vk::Format depth)
{
	if (s.extent == extent and s.motion_format == motion and s.depth_format == depth)
		return;

	// A frame using the previous images may still be rendered
	if (s.motion)
	{
		pool->retired.push_back({decode_count + 2 * pool->slots.size(), std::move(s.motion), std::move(s.motion_view)});
		pool->retired.push_back({decode_count + 2 * pool->slots.size(), std::move(s.depth), std::move(s.depth_view)});
	}

	std::tie(s.motion, s.motion_view) = make_image(device, extent, motion, "frame synthesis motion vectors");
	std::tie(s.depth, s.depth_view) = make_image(device, extent, depth, "frame synthesis depth");
	s.extent = extent;
	s.motion_format = motion;
	s.depth_format = depth;
}

void frame_synthesis_decoder::decode(job & j)
{
	auto begin = std::chrono::steady_clock::now();
	auto & target = *j.target;

	++decode_count;
	std::erase_if(pool->retired, [this](const retired_image & r) { return r.until < decode_count; });

	auto parsed = [&] {
		atrace::section trace("frame synthesis parse");
		return parse(j.blob);
	}();
	const auto & h = parsed.header;

	// Never pair data with another frame or view than the one it came with
	if (h.frame_index != target.frame_index or h.view_index != view_index)
		throw parse_error(std::format("frame synthesis data for frame {} view {} received in frame {} view {}",
		                              h.frame_index,
		                              h.view_index,
		                              target.frame_index,
		                              view_index));

	if (h.motion.width != h.depth.width or h.motion.height != h.depth.height)
		throw parse_error("motion vector and depth images have different sizes");

	auto owner = acquire_slot();
	if (not owner)
	{
		++stats.dropped;
		target.status_.store(frame::state::failed, std::memory_order_release);
		return;
	}

	const vk::Extent2D extent{h.depth.width, h.depth.height};
	const auto motion_format = vk_format(h.motion_encoding);
	const auto depth_format = vk_format(h.depth_encoding);
	prepare_slot(*owner, extent, motion_format, depth_format);

	const size_t motion_size = h.motion.uncompressed_size;
	const size_t depth_size = h.depth.uncompressed_size;
	if (not staging or staging.info().size < motion_size + depth_size)
	{
		staging = buffer_allocation(
		        device,
		        {
		                .size = motion_size + depth_size,
		                .usage = vk::BufferUsageFlagBits::eTransferSrc,
		        },
		        {
		                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
		                .usage = VMA_MEMORY_USAGE_AUTO,
		        },
		        "frame synthesis staging buffer");
	}

	uint8_t * mapped = staging.data();
	{
		atrace::section trace("frame synthesis decompress");
		decompress({h.motion.width, h.motion.height, uint32_t(components(h.motion_encoding)), 2, uint32_t(bytes_per_pixel(h.motion_encoding))},
		           parsed.motion_payload,
		           h.motion.filter_mode,
		           h.motion.compression_mode,
		           std::span(mapped, motion_size));
		decompress({h.depth.width, h.depth.height, 1, uint32_t(bytes_per_pixel(h.depth_encoding)), uint32_t(bytes_per_pixel(h.depth_encoding))},
		           parsed.depth_payload,
		           h.depth.filter_mode,
		           h.depth.compression_mode,
		           std::span(mapped + motion_size, depth_size));
		vmaFlushAllocation(vk_allocator::instance(), staging, 0, motion_size + depth_size);
	}

	atrace::section trace_upload("frame synthesis upload");
	cmd.reset();
	cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

	auto barrier = [&](vk::ImageLayout from, vk::ImageLayout to) {
		std::array barriers{
		        vk::ImageMemoryBarrier2{
		                .srcStageMask = from == vk::ImageLayout::eUndefined ? vk::PipelineStageFlagBits2::eNone : vk::PipelineStageFlagBits2::eTransfer,
		                .srcAccessMask = from == vk::ImageLayout::eUndefined ? vk::AccessFlagBits2::eNone : vk::AccessFlagBits2::eTransferWrite,
		                .dstStageMask = to == vk::ImageLayout::eTransferDstOptimal ? vk::PipelineStageFlagBits2::eTransfer : vk::PipelineStageFlagBits2::eAllCommands,
		                .dstAccessMask = to == vk::ImageLayout::eTransferDstOptimal ? vk::AccessFlagBits2::eTransferWrite : vk::AccessFlagBits2::eShaderSampledRead,
		                .oldLayout = from,
		                .newLayout = to,
		                .image = vk::Image(owner->motion),
		                .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1},
		        },
		        vk::ImageMemoryBarrier2{},
		};
		barriers[1] = barriers[0];
		barriers[1].image = vk::Image(owner->depth);
		cmd.pipelineBarrier2({
		        .imageMemoryBarrierCount = barriers.size(),
		        .pImageMemoryBarriers = barriers.data(),
		});
	};

	barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
	cmd.copyBufferToImage(staging,
	                      owner->motion,
	                      vk::ImageLayout::eTransferDstOptimal,
	                      vk::BufferImageCopy{
	                              .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
	                              .imageExtent = {extent.width, extent.height, 1},
	                      });
	cmd.copyBufferToImage(staging,
	                      owner->depth,
	                      vk::ImageLayout::eTransferDstOptimal,
	                      vk::BufferImageCopy{
	                              .bufferOffset = motion_size,
	                              .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
	                              .imageExtent = {extent.width, extent.height, 1},
	                      });
	barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
	cmd.end();

	const uint64_t value = ++semaphore_value;
	application::get_queue().lock()->submit(
	        vk::StructureChain{
	                vk::SubmitInfo{
	                        .commandBufferCount = 1,
	                        .pCommandBuffers = &*cmd,
	                        .signalSemaphoreCount = 1,
	                        .pSignalSemaphores = &*pool->semaphore,
	                },
	                vk::TimelineSemaphoreSubmitInfo{
	                        .signalSemaphoreValueCount = 1,
	                        .pSignalSemaphoreValues = &value,
	                },
	        }
	                .get(),
	        *fence);

	// The staging buffer, command buffer and fence are reused for the next
	// frame: wait until the copy is really done, even if it is slow
	if (device.waitForFences(*fence, true, 1'000'000'000) == vk::Result::eTimeout)
	{
		spdlog::warn("Slow frame synthesis data upload for frame {} view {}", target.frame_index, view_index);
		while (device.waitForFences(*fence, true, 1'000'000'000) == vk::Result::eTimeout)
		{
		}
	}
	device.resetFences(*fence);

	target.header = h;
	target.extent = extent;
	target.motion_format = motion_format;
	target.motion_image = vk::Image(owner->motion);
	target.motion_view = *owner->motion_view;
	target.depth_format = depth_format;
	target.depth_image = vk::Image(owner->depth);
	target.depth_view = *owner->depth_view;
	target.semaphore = *pool->semaphore;
	target.semaphore_value = value;
	target.owner = std::move(owner);
	target.status_.store(frame::state::ready, std::memory_order_release);

	++stats.decoded;
	float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - begin).count();
	stats.decode_ms = stats.decode_ms * 0.9f + ms * 0.1f;
}

} // namespace wivrn
