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

#include "stream_frame_synthesis.h"

#include "application.h"
#include "utils/contains.h"
#include "vk/pipeline.h"
#include "vk/shader.h"
#include "xr/session.h"
#include "xr/system.h"

#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

namespace
{
constexpr vk::Format motion_format = vk::Format::eR16G16B16A16Sfloat;

// In order of preference
constexpr std::array depth_formats{
        vk::Format::eD32Sfloat,
        vk::Format::eD24UnormS8Uint,
        vk::Format::eX8D24UnormPack32,
        vk::Format::eD16Unorm,
};

bool has_stencil(vk::Format format)
{
	switch (format)
	{
		case vk::Format::eD16UnormS8Uint:
		case vk::Format::eD24UnormS8Uint:
		case vk::Format::eD32SfloatS8Uint:
			return true;
		default:
			return false;
	}
}

struct rigid_transform
{
	glm::quat orientation;
	glm::vec3 position;

	rigid_transform(const XrPosef & pose) :
	        orientation(glm::quat::wxyz(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z)),
	        position(pose.position.x, pose.position.y, pose.position.z) {}

	rigid_transform(glm::quat orientation, glm::vec3 position) :
	        orientation(orientation), position(position) {}

	rigid_transform operator*(const rigid_transform & other) const
	{
		return {orientation * other.orientation, position + orientation * other.position};
	}

	rigid_transform inverse() const
	{
		glm::quat inv = glm::inverse(orientation);
		return {inv, -(inv * position)};
	}

	operator XrPosef() const
	{
		return {
		        .orientation = {orientation.x, orientation.y, orientation.z, orientation.w},
		        .position = {position.x, position.y, position.z},
		};
	}
};
} // namespace

std::unique_ptr<stream_frame_synthesis> stream_frame_synthesis::create(
        xr::instance & instance,
        xr::session & session,
        xr::system & system,
        vk::raii::Device & device,
        XrViewConfigurationType viewconfig)
{
	const auto formats = session.get_swapchain_formats();
	if (not utils::contains(formats, motion_format))
	{
		spdlog::warn("Runtime frame synthesis: motion vector format {} not supported", vk::to_string(motion_format));
		return nullptr;
	}

	auto depth_format = std::ranges::find_if(depth_formats, [&](vk::Format f) { return utils::contains(formats, f); });
	if (depth_format == depth_formats.end())
	{
		spdlog::warn("Runtime frame synthesis: no supported depth format");
		return nullptr;
	}

	const auto config_views = system.frame_synthesis_config_views(viewconfig);
	if (config_views.size() != view_count)
	{
		spdlog::warn("Runtime frame synthesis: {} views, expected {}", config_views.size(), view_count);
		return nullptr;
	}

	vk::Extent2D extent{
	        .width = config_views[0].recommendedMotionVectorImageRectWidth,
	        .height = config_views[0].recommendedMotionVectorImageRectHeight,
	};
	if (extent.width == 0 or extent.height == 0)
	{
		auto views = system.view_configuration_views(viewconfig);
		extent = vk::Extent2D{
		        .width = views[0].recommendedImageRectWidth,
		        .height = views[0].recommendedImageRectHeight,
		};
	}

	spdlog::info("Runtime frame synthesis: recommended motion vector size {}x{} (right eye {}x{}), using {}x{} {} and {}",
	             config_views[0].recommendedMotionVectorImageRectWidth,
	             config_views[0].recommendedMotionVectorImageRectHeight,
	             config_views[1].recommendedMotionVectorImageRectWidth,
	             config_views[1].recommendedMotionVectorImageRectHeight,
	             extent.width,
	             extent.height,
	             vk::to_string(motion_format),
	             vk::to_string(*depth_format));

	return std::unique_ptr<stream_frame_synthesis>(new stream_frame_synthesis(instance, session, device, extent, *depth_format));
}

stream_frame_synthesis::stream_frame_synthesis(
        xr::instance & instance,
        xr::session & session,
        vk::raii::Device & device,
        vk::Extent2D extent,
        vk::Format depth_format) :
        device(device),
        motion_swapchain(instance, session, device, motion_format, extent.width, extent.height, 1, view_count),
        depth_swapchain(instance, session, device, depth_format, extent.width, extent.height, 1, view_count),
        extent(extent)
{
	sampler = vk::raii::Sampler(device, vk::SamplerCreateInfo{
	                                            .magFilter = vk::Filter::eNearest,
	                                            .minFilter = vk::Filter::eNearest,
	                                            .mipmapMode = vk::SamplerMipmapMode::eNearest,
	                                            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
	                                            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
	                                            .addressModeW = vk::SamplerAddressMode::eClampToEdge,
	                                            .maxLod = 0,
	                                    });

	const bool stencil = has_stencil(depth_format);

	// The images are entirely overwritten, and left in the layouts required
	// when releasing them to the runtime
	std::array attachments{
	        vk::AttachmentDescription{
	                .format = motion_format,
	                .samples = vk::SampleCountFlagBits::e1,
	                .loadOp = vk::AttachmentLoadOp::eDontCare,
	                .storeOp = vk::AttachmentStoreOp::eStore,
	                .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
	                .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
	                .initialLayout = vk::ImageLayout::eUndefined,
	                .finalLayout = vk::ImageLayout::eColorAttachmentOptimal,
	        },
	        vk::AttachmentDescription{
	                .format = depth_format,
	                .samples = vk::SampleCountFlagBits::e1,
	                .loadOp = vk::AttachmentLoadOp::eDontCare,
	                .storeOp = vk::AttachmentStoreOp::eStore,
	                .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
	                .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
	                .initialLayout = vk::ImageLayout::eUndefined,
	                .finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
	        },
	};

	vk::AttachmentReference color_ref{
	        .attachment = 0,
	        .layout = vk::ImageLayout::eColorAttachmentOptimal,
	};
	vk::AttachmentReference depth_ref{
	        .attachment = 1,
	        .layout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
	};
	vk::SubpassDescription subpass{
	        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
	        .colorAttachmentCount = 1,
	        .pColorAttachments = &color_ref,
	        .pDepthStencilAttachment = &depth_ref,
	};

	renderpass = vk::raii::RenderPass(device, vk::RenderPassCreateInfo{
	                                                  .attachmentCount = attachments.size(),
	                                                  .pAttachments = attachments.data(),
	                                                  .subpassCount = 1,
	                                                  .pSubpasses = &subpass,
	                                          });

	// Descriptor sets: motion vectors and depth of one view
	std::array bindings{
	        vk::DescriptorSetLayoutBinding{
	                .binding = 0,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eFragment,
	                .pImmutableSamplers = &*sampler,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eFragment,
	                .pImmutableSamplers = &*sampler,
	        },
	};

	descriptor_set_layout = device.createDescriptorSetLayout(vk::DescriptorSetLayoutCreateInfo{
	        .bindingCount = bindings.size(),
	        .pBindings = bindings.data(),
	});

	vk::DescriptorPoolSize pool_size{
	        .type = vk::DescriptorType::eCombinedImageSampler,
	        .descriptorCount = 2 * view_count,
	};

	ds_pool = device.createDescriptorPool(vk::DescriptorPoolCreateInfo{
	        .maxSets = view_count,
	        .poolSizeCount = 1,
	        .pPoolSizes = &pool_size,
	});

	std::array<vk::DescriptorSetLayout, view_count> set_layouts;
	set_layouts.fill(*descriptor_set_layout);
	auto sets = (*device).allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
	        .descriptorPool = *ds_pool,
	        .descriptorSetCount = set_layouts.size(),
	        .pSetLayouts = set_layouts.data(),
	});
	std::ranges::copy(sets, ds.begin());

	layout = vk::raii::PipelineLayout(device, vk::PipelineLayoutCreateInfo{
	                                                  .setLayoutCount = 1,
	                                                  .pSetLayouts = &*descriptor_set_layout,
	                                          });

	auto vertex_shader = load_shader(device, "frame_synthesis_copy.vert");
	auto fragment_shader = load_shader(device, "frame_synthesis_copy.frag");

	vk::pipeline_builder pipeline_info{
	        .flags = {},
	        .Stages = {
	                {
	                        .stage = vk::ShaderStageFlagBits::eVertex,
	                        .module = *vertex_shader,
	                        .pName = "main",
	                },
	                {
	                        .stage = vk::ShaderStageFlagBits::eFragment,
	                        .module = *fragment_shader,
	                        .pName = "main",
	                },
	        },
	        .InputAssemblyState = {{
	                .topology = vk::PrimitiveTopology::eTriangleList,
	        }},
	        .Viewports = {{
	                .x = 0,
	                .y = 0,
	                .width = float(extent.width),
	                .height = float(extent.height),
	                .minDepth = 0,
	                .maxDepth = 1,
	        }},
	        .Scissors = {{
	                .offset = {0, 0},
	                .extent = extent,
	        }},
	        .RasterizationState = {{
	                .polygonMode = vk::PolygonMode::eFill,
	                .cullMode = vk::CullModeFlagBits::eNone,
	                .lineWidth = 1,
	        }},
	        .MultisampleState = {{
	                .rasterizationSamples = vk::SampleCountFlagBits::e1,
	        }},
	        .DepthStencilState = {{
	                .depthTestEnable = true,
	                .depthWriteEnable = true,
	                .depthCompareOp = vk::CompareOp::eAlways,
	        }},
	        .ColorBlendState = {.flags = {}},
	        .ColorBlendAttachments = {{
	                .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
	        }},
	        .layout = *layout,
	        .renderPass = *renderpass,
	        .subpass = 0,
	};

	pipeline = device.createGraphicsPipeline(application::get_pipeline_cache(), pipeline_info);

	// Image views: motion images first, then depth images, view_count per image
	auto make_views = [&](const xr::swapchain & swapchain, vk::Format format, vk::ImageAspectFlags aspect) {
		for (vk::Image image: swapchain.images())
		{
			for (uint32_t view = 0; view < view_count; ++view)
			{
				image_views.emplace_back(device, vk::ImageViewCreateInfo{
				                                         .image = image,
				                                         .viewType = vk::ImageViewType::e2D,
				                                         .format = format,
				                                         .subresourceRange = {
				                                                 .aspectMask = aspect,
				                                                 .baseMipLevel = 0,
				                                                 .levelCount = 1,
				                                                 .baseArrayLayer = view,
				                                                 .layerCount = 1,
				                                         },
				                                 });
			}
		}
	};
	const size_t motion_images = motion_swapchain.images().size();
	const size_t depth_images = depth_swapchain.images().size();
	image_views.reserve((motion_images + depth_images) * view_count);
	make_views(motion_swapchain, motion_format, vk::ImageAspectFlagBits::eColor);
	make_views(depth_swapchain, depth_format, stencil ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil : vk::ImageAspectFlagBits::eDepth);

	// Framebuffers for every combination of motion and depth image indices
	framebuffers.reserve(motion_images * depth_images * view_count);
	for (size_t m = 0; m < motion_images; ++m)
	{
		for (size_t d = 0; d < depth_images; ++d)
		{
			for (uint32_t view = 0; view < view_count; ++view)
			{
				std::array fb_attachments{
				        *image_views[m * view_count + view],
				        *image_views[(motion_images + d) * view_count + view],
				};
				framebuffers.emplace_back(device, vk::FramebufferCreateInfo{
				                                          .renderPass = *renderpass,
				                                          .attachmentCount = fb_attachments.size(),
				                                          .pAttachments = fb_attachments.data(),
				                                          .width = extent.width,
				                                          .height = extent.height,
				                                          .layers = 1,
				                                  });
			}
		}
	}
}

stream_frame_synthesis::result stream_frame_synthesis::check(const std::array<std::shared_ptr<frame>, view_count> & frames, bool repeated)
{
	for (const auto & f: frames)
	{
		if (not f)
			return result::no_data;
	}

	if (repeated)
		return result::repeated;

	for (uint32_t view = 0; view < view_count; ++view)
	{
		if (frames[view]->frame_index != frames[0]->frame_index or frames[view]->view_index != view)
			return result::mismatch;
	}

	for (const auto & f: frames)
	{
		if (f->status() != frame::state::ready)
			return result::not_ready;
	}

	return result::forwarded;
}

void stream_frame_synthesis::record(vk::raii::CommandBuffer & command_buffer, const std::array<std::shared_ptr<frame>, view_count> & frames)
{
	assert(not acquired);
	const int motion_index = motion_swapchain.acquire();
	motion_swapchain.wait();
	const int depth_index = depth_swapchain.acquire();
	depth_swapchain.wait();
	acquired = true;

	const size_t depth_images = depth_swapchain.images().size();

	for (uint32_t view = 0; view < view_count; ++view)
	{
		const auto & f = *frames[view];

		std::array image_info{
		        vk::DescriptorImageInfo{
		                .imageView = f.motion_view,
		                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        },
		        vk::DescriptorImageInfo{
		                .imageView = f.depth_view,
		                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        },
		};

		device.updateDescriptorSets(
		        vk::WriteDescriptorSet{
		                .dstSet = ds[view],
		                .dstBinding = 0,
		                .descriptorCount = image_info.size(),
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = image_info.data(),
		        },
		        {});

		command_buffer.beginRenderPass(
		        vk::RenderPassBeginInfo{
		                .renderPass = *renderpass,
		                .framebuffer = *framebuffers[(motion_index * depth_images + depth_index) * view_count + view],
		                .renderArea = {
		                        .offset = {0, 0},
		                        .extent = extent,
		                },
		        },
		        vk::SubpassContents::eInline);
		command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
		command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout, 0, ds[view], {});
		command_buffer.draw(3, 1, 0, 0);
		command_buffer.endRenderPass();
	}
}

void stream_frame_synthesis::release()
{
	if (not acquired)
		return;
	motion_swapchain.release();
	depth_swapchain.release();
	acquired = false;
}

XrFrameSynthesisInfoEXT * stream_frame_synthesis::info(uint32_t view, const frame & f, bool flip_y)
{
	const auto & h = f.header;

	// The delta pose is in the application's layer space, the stream layer is
	// submitted in the space of the view poses
	const rigid_transform layer_space = h.layer_space_pose;
	const rigid_transform delta = layer_space * rigid_transform(h.app_space_delta_pose) * layer_space.inverse();

	const float y_sign = flip_y ? -1 : 1;
	const XrRect2Di rect{
	        .offset = {0, 0},
	        .extent = {int32_t(extent.width), int32_t(extent.height)},
	};

	infos[view] = XrFrameSynthesisInfoEXT{
	        .type = XR_TYPE_FRAME_SYNTHESIS_INFO_EXT,
	        .next = nullptr,
	        // The frame interval is driven by the stream, not the application
	        .layerFlags = h.layer_flags & ~XR_FRAME_SYNTHESIS_INFO_REQUEST_RELAXED_FRAME_INTERVAL_BIT_EXT,
	        .motionVectorSubImage = {
	                .swapchain = motion_swapchain,
	                .imageRect = rect,
	                .imageArrayIndex = view,
	        },
	        .motionVectorScale = {
	                h.motion_vector_scale.x,
	                y_sign * h.motion_vector_scale.y,
	                h.motion_vector_scale.z,
	                0,
	        },
	        .motionVectorOffset = {
	                h.motion_vector_offset.x,
	                y_sign * h.motion_vector_offset.y,
	                h.motion_vector_offset.z,
	                0,
	        },
	        .appSpaceDeltaPose = delta,
	        .depthSubImage = {
	                .swapchain = depth_swapchain,
	                .imageRect = rect,
	                .imageArrayIndex = view,
	        },
	        .minDepth = h.min_depth,
	        .maxDepth = h.max_depth,
	        .nearZ = h.near_z,
	        .farZ = h.far_z,
	};
	return &infos[view];
}
