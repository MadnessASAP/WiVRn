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

// Monado includes first, they set up the Vulkan platform defines
#include "util/comp_layer_accum.h"
#include "util/comp_swapchain.h"
#include "util/u_logging.h"

#include "frame_synthesis.h"

#include "driver/configuration.h"
#include "driver/xrt_cast.h"
#include "frame_synthesis/annexb_sei.h"
#include "frame_synthesis/aux_codec.h"
#include "utils/wivrn_trace.h"
#include "utils/wivrn_vk_bundle.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string_view>
#include <utility>
#include <vulkan/vulkan_format_traits.hpp>

namespace
{
using namespace wivrn::frame_synthesis;

struct push_constants
{
	int32_t motion_offset[2];
	int32_t motion_extent[2];
	int32_t depth_offset[2];
	int32_t depth_extent[2];
	int32_t dst_extent[2];
	uint32_t motion_words;
	uint32_t depth_words_offset;
	uint32_t depth_half;
	uint32_t closest_is_max;
};

// Largest wire image, see frame_synthesis::max_image_dimension
constexpr uint32_t max_dimension = 2048;

// Size written by the shader for a view: motion (1 or 2 words) and depth (1 word) per pixel
size_t view_buffer_size(const view_header & h)
{
	const size_t words = h.motion_encoding == motion_format::rg16_sfloat ? 2 : 3;
	return size_t(h.depth.width) * h.depth.height * words * sizeof(uint32_t);
}

bool is_float_color_format(vk::Format format)
{
	if (format == vk::Format::eUndefined or vk::componentCount(format) < 2)
		return false;
	if (std::string_view(vk::componentName(format, 0)) != "R")
		return false;
	std::string_view numeric = vk::componentNumericFormat(format, 0);
	return numeric == "SFLOAT" or numeric == "UNORM" or numeric == "SNORM" or numeric == "UFLOAT";
}

bool is_depth_format(vk::Format format)
{
	return format != vk::Format::eUndefined and std::string_view(vk::componentName(format, 0)) == "D";
}

vk::raii::Sampler make_sampler(wivrn::vk_bundle & vk)
{
	vk::raii::Sampler res(
	        vk.device,
	        vk::SamplerCreateInfo{
	                .magFilter = vk::Filter::eNearest,
	                .minFilter = vk::Filter::eNearest,
	                .mipmapMode = vk::SamplerMipmapMode::eNearest,
	                .addressModeU = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeV = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeW = vk::SamplerAddressMode::eClampToEdge,
	        });
	vk.name(res, "frame synthesis sampler");
	return res;
}

vk::raii::DescriptorSetLayout make_ds_layout(wivrn::vk_bundle & vk)
{
	std::array bindings{
	        vk::DescriptorSetLayoutBinding{
	                .binding = 0,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 2,
	                .descriptorType = vk::DescriptorType::eStorageBuffer,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	};
	vk::raii::DescriptorSetLayout res{
	        vk.device,
	        vk::DescriptorSetLayoutCreateInfo{
	                .bindingCount = bindings.size(),
	                .pBindings = bindings.data(),
	        },
	};
	vk.name(*res, "frame synthesis descriptor set layout");
	return res;
}

vk::raii::PipelineLayout make_layout(wivrn::vk_bundle & vk, vk::DescriptorSetLayout ds_layout)
{
	vk::PushConstantRange range{
	        .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        .size = sizeof(push_constants),
	};
	vk::raii::PipelineLayout res(vk.device,
	                             vk::PipelineLayoutCreateInfo{
	                                     .setLayoutCount = 1,
	                                     .pSetLayouts = &ds_layout,
	                                     .pushConstantRangeCount = 1,
	                                     .pPushConstantRanges = &range,
	                             });
	vk.name(*res, "frame synthesis pipeline layout");
	return res;
}

vk::raii::Pipeline make_pipeline(wivrn::vk_bundle & vk, vk::PipelineLayout layout)
{
	auto shader = vk.load_shader("frame_synthesis_pack");
	vk::raii::Pipeline res{
	        vk.device,
	        nullptr,
	        vk::ComputePipelineCreateInfo{
	                .stage = {
	                        .stage = vk::ShaderStageFlagBits::eCompute,
	                        .module = *shader,
	                        .pName = "main",
	                },
	                .layout = layout,
	        },
	};
	vk.name(*res, "frame synthesis pipeline");
	return res;
}

vk::raii::DescriptorPool make_ds_pool(wivrn::vk_bundle & vk, size_t count)
{
	std::array pool_sizes{
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = uint32_t(2 * count),
	        },
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eStorageBuffer,
	                .descriptorCount = uint32_t(count),
	        },
	};
	vk::raii::DescriptorPool res{
	        vk.device,
	        vk::DescriptorPoolCreateInfo{
	                .maxSets = uint32_t(count),
	                .poolSizeCount = pool_sizes.size(),
	                .pPoolSizes = pool_sizes.data(),
	        },
	};
	vk.name(*res, "frame synthesis descriptor pool");
	return res;
}

uint32_t divide_and_round_up(uint32_t a, uint32_t b)
{
	return (a + (b - 1)) / b;
}

const comp_swapchain_image & get_image(xrt_swapchain * xsc, uint32_t image_index)
{
	return comp_swapchain(xsc)->images[image_index];
}

vk::Format get_format(xrt_swapchain * xsc)
{
	return vk::Format(comp_swapchain(xsc)->vkic.info.format);
}

image_info source_info(xrt_swapchain * xsc, const xrt_sub_image & sub)
{
	return {
	        .source_x = sub.rect.offset.w,
	        .source_y = sub.rect.offset.h,
	        .source_width = uint32_t(sub.rect.extent.w),
	        .source_height = uint32_t(sub.rect.extent.h),
	        .source_array_index = sub.array_index,
	        .source_format = comp_swapchain(xsc)->vkic.info.format,
	};
}
} // namespace

namespace wivrn
{

frame_synthesis_capture::frame_synthesis_capture(vk_bundle & vk, const settings & config, size_t slot_count) :
        vk(vk),
        config(config),
        sampler(make_sampler(vk)),
        ds_layout(make_ds_layout(vk)),
        layout(make_layout(vk, *ds_layout)),
        pipeline(make_pipeline(vk, *layout)),
        descriptor_pool(make_ds_pool(vk, slot_count * 2))
{
	std::vector<vk::DescriptorSetLayout> layouts(slot_count * 2, *ds_layout);
	// Descriptor sets are freed with the pool
	for (auto & ds: vk.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
	             .descriptorPool = *descriptor_pool,
	             .descriptorSetCount = uint32_t(layouts.size()),
	             .pSetLayouts = layouts.data(),
	     }))
		descriptor_sets.push_back(ds.release());

	// Buffers are allocated on first use, with the size of the transmitted images
	slots.resize(slot_count);

	timestamps = gpu_timestamp_pool(vk, vk.queue.family_index, slot_count, "frame synthesis capture");
}

void frame_synthesis_capture::reject(const std::string & reason)
{
	++stats.rejected_frames;
	if (wivrn::trace::is_enabled())
		wivrn::trace::cpu_instant(wivrn::trace::cpu_track::compositor, std::format("frame synthesis rejected: {}", reason).c_str(), recording_frame, -1);
	if (reason != last_rejection)
	{
		U_LOG_W("Frame synthesis data not transmitted: %s", reason.c_str());
		last_rejection = reason;
	}
}

void frame_synthesis_capture::clear(size_t slot_index)
{
	slots[slot_index].views = {};
	slots[slot_index].gpu_time_pending = false;
}

bool frame_synthesis_capture::record(vk::raii::CommandBuffer & cmd,
                                     size_t slot_index,
                                     const comp_layer & layer,
                                     uint64_t frame_index,
                                     XrTime display_time,
                                     const source_frames & source,
                                     bool color_foveated)
{
	auto & slot = slots.at(slot_index);
	slot.views = {};
	slot.gpu_time_pending = false;
	recording_frame = frame_index;

	if (not layer.has_frame_synthesis)
		return false;

	wivrn::trace::scope trace(wivrn::trace::cpu_track::compositor, -1, frame_index, "frame synthesis record");

	if (layer.fs.view_count != 2 or layer.data.view_count != 2)
	{
		reject("only stereo projection layers are supported");
		return false;
	}

	if (layer.data.flip_y)
	{
		reject("flipped images are not supported");
		return false;
	}

	const auto & proj = layer.data.type == XRT_LAYER_PROJECTION ? layer.data.proj.v : layer.data.depth.v;

	std::array<frame_synthesis::view_header, 2> headers;
	std::array<push_constants, 2> params;
	std::array<std::array<vk::ImageView, 2>, 2> views;

	for (uint32_t i = 0; i < 2; ++i)
	{
		const auto & fs = layer.fs.v[i];
		auto * mv_sc = comp_layer_get_frame_synthesis_motion_vector_swapchain(&layer, i);
		auto * depth_sc = comp_layer_get_frame_synthesis_depth_swapchain(&layer, i);

		const auto mv_format = get_format(mv_sc);
		const auto depth_format = get_format(depth_sc);
		if (not is_float_color_format(mv_format) or (comp_swapchain(mv_sc)->vkic.info.create & XRT_SWAPCHAIN_CREATE_SAMPLE_AS_SRGB))
		{
			reject(std::format("unsupported motion vector format {}", vk::to_string(mv_format)));
			return false;
		}
		if (not is_depth_format(depth_format))
		{
			reject(std::format("unsupported depth format {}", vk::to_string(depth_format)));
			return false;
		}

		const auto & mv_sub = fs.motion_vector_sub;
		const auto & depth_sub = fs.depth_sub;
		if (mv_sub.rect.extent.w <= 0 or mv_sub.rect.extent.h <= 0 or depth_sub.rect.extent.w <= 0 or depth_sub.rect.extent.h <= 0)
		{
			reject("empty frame synthesis image");
			return false;
		}

		const bool use_2d = fs.flags & XRT_FRAME_SYNTHESIS_USE_2D_MOTION_VECTOR_BIT;
		const auto motion_encoding = use_2d ? motion_format::rg16_sfloat : motion_format::rgba16_sfloat;

		// The wire images have the resolution of the scaled depth image
		const uint32_t width = std::clamp<uint32_t>(std::lround(depth_sub.rect.extent.w * config.scale), 1, max_dimension);
		const uint32_t height = std::clamp<uint32_t>(std::lround(depth_sub.rect.extent.h * config.scale), 1, max_dimension);

		auto & h = headers[i];
		h = {
		        .version = frame_synthesis::format_version,
		        .frame_index = frame_index,
		        .view_index = uint8_t(i),
		        .display_time = display_time,
		        .flags = uint8_t(
		                (color_foveated ? frame_synthesis::color_foveated : 0) |
		                (width < uint32_t(depth_sub.rect.extent.w) or height < uint32_t(depth_sub.rect.extent.h) ? frame_synthesis::downsampled_closest_depth : 0)),
		        .app_display_time = source.app_display_time,
		        .previous_app_display_time = source.previous_app_display_time,
		        .previous_frame_index = source.previous_frame_index,
		        .layer_flags = fs.flags,
		        .motion_vector_scale = {fs.motion_vector_scale.x, fs.motion_vector_scale.y, fs.motion_vector_scale.z},
		        .motion_vector_offset = {fs.motion_vector_offset.x, fs.motion_vector_offset.y, fs.motion_vector_offset.z},
		        .app_space_delta_pose = xrt_cast(fs.app_space_delta_pose),
		        .layer_space_pose = xrt_cast(fs.layer_space_pose),
		        .min_depth = fs.min_depth,
		        .max_depth = fs.max_depth,
		        .near_z = fs.near_z,
		        .far_z = fs.far_z,
		        .color_x = proj[i].sub.rect.offset.w,
		        .color_y = proj[i].sub.rect.offset.h,
		        .color_width = uint32_t(proj[i].sub.rect.extent.w),
		        .color_height = uint32_t(proj[i].sub.rect.extent.h),
		        .motion_encoding = motion_encoding,
		        .motion = source_info(mv_sc, mv_sub),
		        .depth_encoding = config.depth,
		        .depth = source_info(depth_sc, depth_sub),
		};
		for (auto * info: {&h.motion, &h.depth})
		{
			info->width = width;
			info->height = height;
			info->filter_mode = filter::delta_byte_planes;
			info->compression_mode = compression::zstd;
		}
		h.motion.uncompressed_size = width * height * bytes_per_pixel(motion_encoding);
		h.depth.uncompressed_size = width * height * bytes_per_pixel(config.depth);

		// depth value of minDepth is at distance nearZ
		const bool reversed = fs.near_z > fs.far_z;
		params[i] = {
		        .motion_offset = {mv_sub.rect.offset.w, mv_sub.rect.offset.h},
		        .motion_extent = {mv_sub.rect.extent.w, mv_sub.rect.extent.h},
		        .depth_offset = {depth_sub.rect.offset.w, depth_sub.rect.offset.h},
		        .depth_extent = {depth_sub.rect.extent.w, depth_sub.rect.extent.h},
		        .dst_extent = {int32_t(width), int32_t(height)},
		        .motion_words = use_2d ? 1u : 2u,
		        .depth_words_offset = width * height * (use_2d ? 1u : 2u),
		        .depth_half = config.depth == depth_format::r16_sfloat,
		        .closest_is_max = reversed,
		};

		views[i] = {
		        get_image(mv_sc, mv_sub.image_index).views.no_alpha[mv_sub.array_index],
		        get_image(depth_sc, depth_sub.image_index).views.no_alpha[depth_sub.array_index],
		};
	}

	// Both views share the buffer, at offsets aligned for storage buffers
	const vk::DeviceSize alignment = std::max<vk::DeviceSize>(vk.physical_device.getProperties().limits.minStorageBufferOffsetAlignment, 16);
	const std::array<vk::DeviceSize, 2> view_sizes{view_buffer_size(headers[0]), view_buffer_size(headers[1])};
	const std::array<vk::DeviceSize, 2> offsets{0, (view_sizes[0] + alignment - 1) / alignment * alignment};
	const vk::DeviceSize buffer_size = offsets[1] + view_sizes[1];
	if (not slot.buffer or slot.buffer.info().size < buffer_size)
	{
		slot.buffer = buffer_allocation(
		        vk.device,
		        {
		                .size = buffer_size,
		                .usage = vk::BufferUsageFlagBits::eStorageBuffer,
		        },
		        {
		                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
		                .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
		        },
		        std::format("frame synthesis buffer {}", slot_index));
	}

	timestamps.cmd_begin(cmd, slot_index, frame_index, vk::PipelineStageFlagBits2::eComputeShader);
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
	for (uint32_t i = 0; i < 2; ++i)
	{
		auto ds = descriptor_sets[slot_index * 2 + i];
		std::array images{
		        vk::DescriptorImageInfo{
		                .sampler = *sampler,
		                .imageView = views[i][0],
		                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        },
		        vk::DescriptorImageInfo{
		                .sampler = *sampler,
		                .imageView = views[i][1],
		                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        },
		};
		vk::DescriptorBufferInfo buffer{
		        .buffer = slot.buffer,
		        .offset = offsets[i],
		        .range = view_sizes[i],
		};
		vk.device.updateDescriptorSets(
		        {
		                vk::WriteDescriptorSet{
		                        .dstSet = ds,
		                        .dstBinding = 0,
		                        .descriptorCount = 1,
		                        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                        .pImageInfo = &images[0],
		                },
		                vk::WriteDescriptorSet{
		                        .dstSet = ds,
		                        .dstBinding = 1,
		                        .descriptorCount = 1,
		                        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                        .pImageInfo = &images[1],
		                },
		                vk::WriteDescriptorSet{
		                        .dstSet = ds,
		                        .dstBinding = 2,
		                        .descriptorCount = 1,
		                        .descriptorType = vk::DescriptorType::eStorageBuffer,
		                        .pBufferInfo = &buffer,
		                },
		        },
		        {});

		cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *layout, 0, ds, {});
		cmd.pushConstants<push_constants>(*layout, vk::ShaderStageFlagBits::eCompute, 0, params[i]);
		cmd.dispatch(divide_and_round_up(params[i].dst_extent[0], 8), divide_and_round_up(params[i].dst_extent[1], 8), 1);
	}
	timestamps.cmd_end(cmd, slot_index, vk::PipelineStageFlagBits2::eComputeShader);
	slot.gpu_time_pending = true;

	// Make the results visible to the CPU once the submission completed
	vk::MemoryBarrier2 barrier{
	        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
	        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
	};
	cmd.pipelineBarrier2({
	        .memoryBarrierCount = 1,
	        .pMemoryBarriers = &barrier,
	});

	for (uint32_t i = 0; i < 2; ++i)
		slot.views[i] = view{
		        .header = headers[i],
		        .buffer_offset = uint32_t(offsets[i]),
		};

	++stats.captured_frames;
	last_rejection.clear();
	return true;
}

std::optional<std::vector<uint8_t>> frame_synthesis_capture::serialize(size_t slot_index, size_t view_index)
{
	auto & slot = slots.at(slot_index);
	if (view_index >= slot.views.size() or not slot.views[view_index])
		return std::nullopt;

	auto & v = *slot.views[view_index];
	auto header = v.header;

	// The GPU work is complete: both views were captured by the same pass
	if (std::exchange(slot.gpu_time_pending, false))
	{
		if (auto s = timestamps.collect(slot_index))
			wivrn::trace::gpu_slice(wivrn::trace::gpu_track::frame_synthesis_pack, "frame synthesis pack", s->begin_ns, s->end_ns, s->frame_index, -1);
	}

	wivrn::trace::scope trace(wivrn::trace::cpu_track::encoder, view_index, header.frame_index, "frame synthesis compress");

	vmaInvalidateAllocation(vk_allocator::instance(), slot.buffer, v.buffer_offset, view_buffer_size(header));
	const uint8_t * data = slot.buffer.data() + v.buffer_offset;

	const uint32_t pixels = uint32_t(header.depth.width) * header.depth.height;
	const uint32_t motion_words = header.motion_encoding == motion_format::rg16_sfloat ? 1 : 2;
	std::span<const uint8_t> motion(data, pixels * motion_words * 4);
	std::span<const uint8_t> depth(data + pixels * motion_words * 4, pixels * 4);

	try
	{
		std::vector<uint8_t> motion_payload, depth_payload;
		{
			wivrn::trace::scope trace(wivrn::trace::cpu_track::encoder, view_index, header.frame_index, "compress motion vectors");
			header.motion.compressed_size = compress(
			        {header.motion.width, header.motion.height, motion_words * 2, 2, motion_words * 4},
			        motion,
			        header.motion.filter_mode,
			        header.motion.compression_mode,
			        config.compression_level,
			        motion_payload);
		}
		{
			wivrn::trace::scope trace(wivrn::trace::cpu_track::encoder, view_index, header.frame_index, "compress depth");
			header.depth.compressed_size = compress(
			        {header.depth.width, header.depth.height, 1, uint32_t(bytes_per_pixel(header.depth_encoding)), 4},
			        depth,
			        header.depth.filter_mode,
			        header.depth.compression_mode,
			        config.compression_level,
			        depth_payload);
		}

		if (wivrn::trace::is_enabled())
		{
			wivrn::trace::counter(wivrn::trace::cpu_track::encoder, std::format("frame synthesis motion bytes {}", view_index).c_str(), motion_payload.size());
			wivrn::trace::counter(wivrn::trace::cpu_track::encoder, std::format("frame synthesis depth bytes {}", view_index).c_str(), depth_payload.size());
		}

		if (motion_payload.size() + depth_payload.size() > config.max_view_size)
		{
			++stats.oversized_views;
			wivrn::trace::cpu_instant(wivrn::trace::cpu_track::encoder, "frame synthesis data too large", header.frame_index, view_index);
			U_LOG_D("Frame synthesis data for view %zu too large: %zu bytes", view_index, motion_payload.size() + depth_payload.size());
			return std::nullopt;
		}

		auto blob = frame_synthesis::serialize(header, motion_payload, depth_payload);
		stats.bytes += blob.size();
		return blob;
	}
	catch (std::exception & e)
	{
		wivrn::trace::cpu_instant(wivrn::trace::cpu_track::encoder, "frame synthesis compression failed", header.frame_index, view_index);
		U_LOG_W("Failed to compress frame synthesis data: %s", e.what());
		return std::nullopt;
	}
}

frame_synthesis_capture::settings frame_synthesis_settings_from_config()
{
	frame_synthesis_capture::settings res;
	auto config = configuration().frame_synthesis_transport;
	res.enabled = config.enabled;
	res.scale = std::clamp(config.scale, 0.25f, 1.f);
	res.depth = config.depth_16bit ? depth_format::r16_sfloat : depth_format::r32_sfloat;
	res.compression_level = config.compression_level;
	// The blob also contains the header, keep it below the SEI reassembly limit
	res.max_view_size = std::min<size_t>(config.max_view_kib * 1024, frame_synthesis::max_blob_size - 4096);
	return res;
}

} // namespace wivrn
