/*
 * WiVRn VR streaming
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

// OpenXR tests for the extensions the CTS does not cover, run against a server with the
// virtual headset (test/virtual_headset). The tests check that what the headset sends
// reaches the application (tracking, face, body, hands, refresh rates) and that what the
// application sends reaches the headset (haptics), so they need to know the headset's
// features: pass the same feature options as to wivrn-virtual-headset.
//
// The OpenXR loader is opened at run time (--loader), XR_RUNTIME_JSON selects the runtime.
// Results are written as JUnit XML (--junit), like the CTS, for test/cts/run_cts.py.

#include <vulkan/vulkan.h>

#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_TIMESPEC
// Only used by XR_FB_swapchain_update_state_opengl_es, avoids depending on the EGL headers
typedef unsigned int EGLenum;
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_reflection.h>

#include <openxr/XR_MNDX_system_buttons.h>
#include <openxr/XR_MNDX_xdev_space.h>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <filesystem>
#include <fnmatch.h>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <print>
#include <set>
#include <signal.h>
#include <span>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{
// ---------------------------------------------------------------------------
// Minimal test framework

struct test_failure : std::runtime_error
{
	using std::runtime_error::runtime_error;
};

struct test_skipped : std::runtime_error
{
	using std::runtime_error::runtime_error;
};

struct test_case
{
	std::string name;
	std::vector<std::string> extensions; // all must be advertised, or the test is skipped
	std::function<void()> body;
};

std::vector<test_case> & registry()
{
	static std::vector<test_case> tests;
	return tests;
}

struct registrar
{
	registrar(std::string name, std::vector<std::string> extensions, std::function<void()> body)
	{
		registry().push_back({std::move(name), std::move(extensions), std::move(body)});
	}
};

// Failures of CHECK in the current test: the test continues
std::vector<std::string> current_failures;

#define CONCAT2(a, b) a##b
#define CONCAT(a, b) CONCAT2(a, b)
#define TEST(name, ...)                                                                                \
	static void CONCAT(test_fn_, __LINE__)();                                                      \
	static registrar CONCAT(test_reg_, __LINE__){name, {__VA_ARGS__}, CONCAT(test_fn_, __LINE__)}; \
	static void CONCAT(test_fn_, __LINE__)()

#define REQUIRE(cond, ...)                                                                                                                \
	do                                                                                                                                \
	{                                                                                                                                 \
		if (not(cond))                                                                                                            \
			throw test_failure(std::format("{}:{}: REQUIRE({}) {}", __FILE__, __LINE__, #cond, std::format("" __VA_ARGS__))); \
	} while (0)

#define CHECK(cond, ...)                                                                                                                        \
	do                                                                                                                                      \
	{                                                                                                                                       \
		if (not(cond))                                                                                                                  \
			current_failures.push_back(std::format("{}:{}: CHECK({}) {}", __FILE__, __LINE__, #cond, std::format("" __VA_ARGS__))); \
	} while (0)

#define SKIP(...) throw test_skipped(std::format(__VA_ARGS__))

std::string to_string(XrResult r)
{
#define RESULT_CASE(name, value) \
	case name:               \
		return #name;
	switch (r)
	{
		XR_LIST_ENUM_XrResult(RESULT_CASE);
		default:
			return std::format("XrResult({})", int(r));
	}
#undef RESULT_CASE
}

#define XR_REQUIRE(call)                                                                                                    \
	do                                                                                                                  \
	{                                                                                                                   \
		XrResult r_ = (call);                                                                                       \
		if (XR_FAILED(r_))                                                                                          \
			throw test_failure(std::format("{}:{}: {} returned {}", __FILE__, __LINE__, #call, to_string(r_))); \
	} while (0)

#define VK_REQUIRE(call)                                                                                                       \
	do                                                                                                                     \
	{                                                                                                                      \
		VkResult r_ = (call);                                                                                          \
		if (r_ != VK_SUCCESS)                                                                                          \
			throw test_failure(std::format("{}:{}: {} returned VkResult {}", __FILE__, __LINE__, #call, int(r_))); \
	} while (0)

// ---------------------------------------------------------------------------
// Expected headset features, same options as wivrn-virtual-headset

struct headset_features
{
	std::string controllers = "touch";
	bool hand_tracking = true;
	bool eye_gaze = true;
	bool palm_pose = true;
	bool user_presence = true;
	std::string face = "fb2";
	std::string body = "none";
	uint32_t generic_trackers = 0;
	std::vector<float> refresh_rates{90};
	std::string stats_file; // the virtual headset statistics, to check what it received
};

headset_features features;
std::string loader_path = "libopenxr_loader.so.1";

// ---------------------------------------------------------------------------
// OpenXR loader

struct loader
{
	void * lib = nullptr;
	PFN_xrGetInstanceProcAddr get_proc = nullptr;
	PFN_xrEnumerateInstanceExtensionProperties enumerate_extensions = nullptr;
	PFN_xrCreateInstance create_instance = nullptr;
	std::set<std::string> extensions;

	loader()
	{
		lib = dlopen(loader_path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (not lib)
			throw std::runtime_error(std::format("cannot open the OpenXR loader {}: {}", loader_path, dlerror()));
		get_proc = (PFN_xrGetInstanceProcAddr)dlsym(lib, "xrGetInstanceProcAddr");
		if (not get_proc)
			throw std::runtime_error("xrGetInstanceProcAddr not found in the loader");
		get_proc(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *)&enumerate_extensions);
		get_proc(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&create_instance);

		uint32_t count = 0;
		enumerate_extensions(nullptr, 0, &count, nullptr);
		std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
		enumerate_extensions(nullptr, count, &count, props.data());
		for (const auto & p: props)
			extensions.insert(p.extensionName);
	}
};

loader & xr_loader()
{
	static loader l;
	return l;
}

class instance
{
public:
	XrInstance handle = XR_NULL_HANDLE;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	std::set<std::string> enabled;

	instance(std::vector<std::string> extensions)
	{
		for (auto ext: {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME})
			extensions.push_back(ext);
		std::vector<const char *> names;
		for (const auto & ext: extensions)
		{
			if (not xr_loader().extensions.contains(ext))
				SKIP("{} is not advertised", ext);
			if (enabled.insert(ext).second)
				names.push_back(ext.c_str());
		}
		XrInstanceCreateInfo info{
		        .type = XR_TYPE_INSTANCE_CREATE_INFO,
		        .applicationInfo = {
		                .applicationVersion = 1,
		                .engineVersion = 1,
		                .apiVersion = XR_API_VERSION_1_0,
		        },
		        .enabledExtensionCount = uint32_t(names.size()),
		        .enabledExtensionNames = names.data(),
		};
		strcpy(info.applicationInfo.applicationName, "wivrn-xr-tests");
		strcpy(info.applicationInfo.engineName, "wivrn-xr-tests");
		XR_REQUIRE(xr_loader().create_instance(&info, &handle));

		XrSystemGetInfo system_info{.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
		XR_REQUIRE(fn<PFN_xrGetSystem>("xrGetSystem")(handle, &system_info, &system));
	}

	instance(const instance &) = delete;

	~instance()
	{
		if (handle)
			fn<PFN_xrDestroyInstance>("xrDestroyInstance")(handle);
	}

	template <typename F>
	F fn(const char * name) const
	{
		F f = nullptr;
		XrResult r = xr_loader().get_proc(handle, name, (PFN_xrVoidFunction *)&f);
		if (XR_FAILED(r) or not f)
			throw test_failure(std::format("xrGetInstanceProcAddr({}) returned {}", name, to_string(r)));
		return f;
	}

	XrPath path(const std::string & str) const
	{
		XrPath p;
		XR_REQUIRE(fn<PFN_xrStringToPath>("xrStringToPath")(handle, str.c_str(), &p));
		return p;
	}

	// Fill an extension struct of XrSystemProperties
	template <typename T>
	T system_properties(XrStructureType type) const
	{
		T ext{};
		ext.type = type;
		XrSystemProperties props{.type = XR_TYPE_SYSTEM_PROPERTIES, .next = &ext};
		XR_REQUIRE(fn<PFN_xrGetSystemProperties>("xrGetSystemProperties")(handle, system, &props));
		return ext;
	}

	XrTime now() const
	{
		timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		XrTime t;
		XR_REQUIRE(fn<PFN_xrConvertTimespecTimeToTimeKHR>("xrConvertTimespecTimeToTimeKHR")(handle, &ts, &t));
		return t;
	}
};

// ---------------------------------------------------------------------------
// A running Vulkan session that submits frames with a cleared projection layer

class session
{
public:
	instance & inst;
	XrSession handle = XR_NULL_HANDLE;
	XrSessionState state = XR_SESSION_STATE_UNKNOWN;
	bool begun = false; // between xrBeginSession and xrEndSession
	XrSpace local = XR_NULL_HANDLE;
	XrSpace view = XR_NULL_HANDLE;
	XrSwapchain swapchain = XR_NULL_HANDLE;
	XrExtent2Di extent{};
	int64_t format = 0;
	XrFrameState frame_state{XR_TYPE_FRAME_STATE};
	std::vector<XrView> views;
	std::vector<XrEventDataBuffer> events; // every event received, for the tests
	uint64_t frames = 0;

	VkInstance vk_instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	uint32_t queue_family = 0;
	VkQueue queue = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	std::vector<VkImage> images;

	PFN_xrPollEvent xrPollEvent;
	PFN_xrWaitFrame xrWaitFrame;
	PFN_xrBeginFrame xrBeginFrame;
	PFN_xrEndFrame xrEndFrame;
	PFN_xrLocateViews xrLocateViews;
	PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage;
	PFN_xrWaitSwapchainImage xrWaitSwapchainImage;
	PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage;

	session(instance & inst, const void * create_next = nullptr) :
	        inst(inst)
	{
		xrPollEvent = inst.fn<PFN_xrPollEvent>("xrPollEvent");
		xrWaitFrame = inst.fn<PFN_xrWaitFrame>("xrWaitFrame");
		xrBeginFrame = inst.fn<PFN_xrBeginFrame>("xrBeginFrame");
		xrEndFrame = inst.fn<PFN_xrEndFrame>("xrEndFrame");
		xrLocateViews = inst.fn<PFN_xrLocateViews>("xrLocateViews");
		xrAcquireSwapchainImage = inst.fn<PFN_xrAcquireSwapchainImage>("xrAcquireSwapchainImage");
		xrWaitSwapchainImage = inst.fn<PFN_xrWaitSwapchainImage>("xrWaitSwapchainImage");
		xrReleaseSwapchainImage = inst.fn<PFN_xrReleaseSwapchainImage>("xrReleaseSwapchainImage");

		create_vulkan();

		XrGraphicsBindingVulkan2KHR binding{
		        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
		        .next = create_next,
		        .instance = vk_instance,
		        .physicalDevice = physical_device,
		        .device = device,
		        .queueFamilyIndex = queue_family,
		        .queueIndex = 0,
		};
		XrSessionCreateInfo info{.type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = inst.system};
		XR_REQUIRE(inst.fn<PFN_xrCreateSession>("xrCreateSession")(inst.handle, &info, &handle));

		local = create_reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
		view = create_reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);

		create_swapchain();
	}

	session(const session &) = delete;

	~session()
	{
		if (handle)
		{
			if (begun)
			{
				inst.fn<PFN_xrRequestExitSession>("xrRequestExitSession")(handle);
				// Wait for STOPPING, then end the session
				auto end = std::chrono::steady_clock::now() + 2s;
				while (state != XR_SESSION_STATE_STOPPING and std::chrono::steady_clock::now() < end)
				{
					try
					{
						frame();
					}
					catch (...)
					{
						break;
					}
				}
				if (state == XR_SESSION_STATE_STOPPING)
					inst.fn<PFN_xrEndSession>("xrEndSession")(handle);
			}
			if (swapchain)
				inst.fn<PFN_xrDestroySwapchain>("xrDestroySwapchain")(swapchain);
			inst.fn<PFN_xrDestroySession>("xrDestroySession")(handle);
		}
		if (device)
		{
			vkDeviceWaitIdle(device);
			vkDestroyFence(device, fence, nullptr);
			vkDestroyCommandPool(device, pool, nullptr);
			vkDestroyDevice(device, nullptr);
		}
		if (vk_instance)
			vkDestroyInstance(vk_instance, nullptr);
	}

	XrSpace create_reference_space(XrReferenceSpaceType type)
	{
		XrReferenceSpaceCreateInfo info{
		        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
		        .referenceSpaceType = type,
		        .poseInReferenceSpace = {.orientation = {0, 0, 0, 1}},
		};
		XrSpace space;
		XR_REQUIRE(inst.fn<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace")(handle, &info, &space));
		return space;
	}

	XrSpaceLocation locate(XrSpace space, XrSpace base, XrTime time)
	{
		XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
		XR_REQUIRE(inst.fn<PFN_xrLocateSpace>("xrLocateSpace")(space, base, time, &loc));
		return loc;
	}

	void create_vulkan()
	{
		XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
		XR_REQUIRE(inst.fn<PFN_xrGetVulkanGraphicsRequirements2KHR>("xrGetVulkanGraphicsRequirements2KHR")(inst.handle, inst.system, &req));

		VkApplicationInfo app{
		        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		        .pApplicationName = "wivrn-xr-tests",
		        .apiVersion = VK_API_VERSION_1_1,
		};
		VkInstanceCreateInfo vk_info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
		XrVulkanInstanceCreateInfoKHR xr_vk_info{
		        .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
		        .systemId = inst.system,
		        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr,
		        .vulkanCreateInfo = &vk_info,
		};
		VkResult vk_result;
		XR_REQUIRE(inst.fn<PFN_xrCreateVulkanInstanceKHR>("xrCreateVulkanInstanceKHR")(inst.handle, &xr_vk_info, &vk_instance, &vk_result));
		VK_REQUIRE(vk_result);

		XrVulkanGraphicsDeviceGetInfoKHR dev_info{
		        .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
		        .systemId = inst.system,
		        .vulkanInstance = vk_instance,
		};
		XR_REQUIRE(inst.fn<PFN_xrGetVulkanGraphicsDevice2KHR>("xrGetVulkanGraphicsDevice2KHR")(inst.handle, &dev_info, &physical_device));

		uint32_t count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, families.data());
		queue_family = count;
		for (uint32_t i = 0; i < count; ++i)
			if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			{
				queue_family = i;
				break;
			}
		REQUIRE(queue_family < count, "no graphics queue");

		float priority = 1;
		VkDeviceQueueCreateInfo queue_info{
		        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		        .queueFamilyIndex = queue_family,
		        .queueCount = 1,
		        .pQueuePriorities = &priority,
		};
		VkDeviceCreateInfo device_info{
		        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		        .queueCreateInfoCount = 1,
		        .pQueueCreateInfos = &queue_info,
		};
		XrVulkanDeviceCreateInfoKHR xr_device_info{
		        .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR,
		        .systemId = inst.system,
		        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr,
		        .vulkanPhysicalDevice = physical_device,
		        .vulkanCreateInfo = &device_info,
		};
		XR_REQUIRE(inst.fn<PFN_xrCreateVulkanDeviceKHR>("xrCreateVulkanDeviceKHR")(inst.handle, &xr_device_info, &device, &vk_result));
		VK_REQUIRE(vk_result);
		vkGetDeviceQueue(device, queue_family, 0, &queue);

		VkCommandPoolCreateInfo pool_info{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		        .queueFamilyIndex = queue_family,
		};
		VK_REQUIRE(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
		VkCommandBufferAllocateInfo alloc{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		        .commandPool = pool,
		        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		        .commandBufferCount = 1,
		};
		VK_REQUIRE(vkAllocateCommandBuffers(device, &alloc, &cmd));
		VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		VK_REQUIRE(vkCreateFence(device, &fence_info, nullptr, &fence));
	}

	std::vector<int64_t> swapchain_formats()
	{
		auto enumerate = inst.fn<PFN_xrEnumerateSwapchainFormats>("xrEnumerateSwapchainFormats");
		uint32_t count = 0;
		XR_REQUIRE(enumerate(handle, 0, &count, nullptr));
		std::vector<int64_t> formats(count);
		XR_REQUIRE(enumerate(handle, count, &count, formats.data()));
		return formats;
	}

	// Create a swapchain, return its result instead of failing
	XrResult try_create_swapchain(XrSwapchainUsageFlags usage, const void * next, int64_t fmt, XrSwapchain & out)
	{
		XrSwapchainCreateInfo info{
		        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
		        .next = next,
		        .usageFlags = usage,
		        .format = fmt,
		        .sampleCount = 1,
		        .width = uint32_t(extent.width),
		        .height = uint32_t(extent.height),
		        .faceCount = 1,
		        .arraySize = 2,
		        .mipCount = 1,
		};
		return inst.fn<PFN_xrCreateSwapchain>("xrCreateSwapchain")(handle, &info, &out);
	}

	void create_swapchain()
	{
		auto enumerate_views = inst.fn<PFN_xrEnumerateViewConfigurationViews>("xrEnumerateViewConfigurationViews");
		uint32_t count = 0;
		XR_REQUIRE(enumerate_views(inst.handle, inst.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr));
		std::vector<XrViewConfigurationView> config_views(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
		XR_REQUIRE(enumerate_views(inst.handle, inst.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, config_views.data()));
		REQUIRE(count == 2, "{} views", count);
		// Small images: the content is not checked, keep lavapipe fast
		extent = {int32_t(std::min<uint32_t>(256, config_views[0].recommendedImageRectWidth)),
		          int32_t(std::min<uint32_t>(256, config_views[0].recommendedImageRectHeight))};
		views.assign(count, {XR_TYPE_VIEW});

		auto formats = swapchain_formats();
		REQUIRE(not formats.empty());
		format = formats[0];
		for (auto f: formats)
			if (f == VK_FORMAT_R8G8B8A8_SRGB or f == VK_FORMAT_B8G8R8A8_SRGB)
			{
				format = f;
				break;
			}

		XR_REQUIRE(try_create_swapchain(XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT, nullptr, format, swapchain));

		auto enumerate_images = inst.fn<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages");
		XR_REQUIRE(enumerate_images(swapchain, 0, &count, nullptr));
		std::vector<XrSwapchainImageVulkan2KHR> xr_images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
		XR_REQUIRE(enumerate_images(swapchain, count, &count, (XrSwapchainImageBaseHeader *)xr_images.data()));
		for (auto & i: xr_images)
			images.push_back(i.image);
	}

	void clear_image(VkImage image)
	{
		VK_REQUIRE(vkResetCommandBuffer(cmd, 0));
		VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
		VK_REQUIRE(vkBeginCommandBuffer(cmd, &begin));
		VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
		VkImageMemoryBarrier barrier{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = 0,
		        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image = image,
		        .subresourceRange = range,
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		float shade = (frames % 60) / 60.f;
		VkClearColorValue color{.float32 = {shade, 0.2f, 1 - shade, 1}};
		vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		VK_REQUIRE(vkEndCommandBuffer(cmd));
		VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
		VK_REQUIRE(vkQueueSubmit(queue, 1, &submit, fence));
		VK_REQUIRE(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
		VK_REQUIRE(vkResetFences(device, 1, &fence));
	}

	bool running() const
	{
		return begun and state != XR_SESSION_STATE_STOPPING;
	}

	void poll_events()
	{
		while (true)
		{
			XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
			XrResult r = xrPollEvent(inst.handle, &event);
			XR_REQUIRE(r);
			if (r == XR_EVENT_UNAVAILABLE)
				return;
			events.push_back(event);
			if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
			{
				auto & e = (XrEventDataSessionStateChanged &)event;
				if (e.session != handle)
					continue;
				state = e.state;
				if (state == XR_SESSION_STATE_READY)
				{
					XrSessionBeginInfo begin{
					        .type = XR_TYPE_SESSION_BEGIN_INFO,
					        .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
					};
					XR_REQUIRE(inst.fn<PFN_xrBeginSession>("xrBeginSession")(handle, &begin));
					begun = true;
				}
			}
		}
	}

	// One frame: projection layer, optionally with an extension struct and extra layers
	void frame(const void * projection_next = nullptr, std::vector<const XrCompositionLayerBaseHeader *> extra_layers = {})
	{
		poll_events();
		if (not running())
		{
			std::this_thread::sleep_for(10ms);
			return;
		}

		XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
		frame_state = {XR_TYPE_FRAME_STATE};
		XR_REQUIRE(xrWaitFrame(handle, &wait, &frame_state));
		XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
		XR_REQUIRE(xrBeginFrame(handle, &begin));

		XrViewLocateInfo locate{
		        .type = XR_TYPE_VIEW_LOCATE_INFO,
		        .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
		        .displayTime = frame_state.predictedDisplayTime,
		        .space = local,
		};
		XrViewState view_state{XR_TYPE_VIEW_STATE};
		uint32_t count = views.size();
		XR_REQUIRE(xrLocateViews(handle, &locate, &view_state, count, &count, views.data()));

		std::vector<const XrCompositionLayerBaseHeader *> layers;
		std::array<XrCompositionLayerProjectionView, 2> projection_views{};
		XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		bool views_valid = (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) and
		                   (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
		if (frame_state.shouldRender and views_valid)
		{
			uint32_t index;
			XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			XR_REQUIRE(xrAcquireSwapchainImage(swapchain, &acquire, &index));
			XrSwapchainImageWaitInfo wait_image{.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};
			XR_REQUIRE(xrWaitSwapchainImage(swapchain, &wait_image));
			clear_image(images[index]);
			XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			XR_REQUIRE(xrReleaseSwapchainImage(swapchain, &release));

			for (uint32_t i = 0; i < 2; ++i)
				projection_views[i] = {
				        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
				        .pose = views[i].pose,
				        .fov = views[i].fov,
				        .subImage = {.swapchain = swapchain, .imageRect = {{0, 0}, extent}, .imageArrayIndex = i},
				};
			projection.next = projection_next;
			projection.space = local;
			projection.viewCount = 2;
			projection.views = projection_views.data();
			layers.push_back((const XrCompositionLayerBaseHeader *)&projection);
			for (auto l: extra_layers)
				layers.push_back(l);
		}

		XrFrameEndInfo end{
		        .type = XR_TYPE_FRAME_END_INFO,
		        .displayTime = frame_state.predictedDisplayTime,
		        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
		        .layerCount = uint32_t(layers.size()),
		        .layers = layers.data(),
		};
		XR_REQUIRE(xrEndFrame(handle, &end));
		++frames;
	}

	// Submit frames until the predicate is true
	bool run_until(std::function<bool()> predicate, std::chrono::milliseconds timeout = 10s)
	{
		auto end = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < end)
		{
			frame();
			if (predicate())
				return true;
		}
		return false;
	}

	void wait_focused()
	{
		REQUIRE(run_until([&] { return state == XR_SESSION_STATE_FOCUSED; }), "session state {}", int(state));
		// Let tracking requests reach the headset and its answers come back
		run_until([] { return false; }, 300ms);
	}
};

// ---------------------------------------------------------------------------
// Helpers

nlohmann::json headset_stats()
{
	if (features.stats_file.empty())
		return {};
	try
	{
		return nlohmann::json::parse(std::ifstream(features.stats_file));
	}
	catch (...)
	{
		return {};
	}
}

bool near(float a, float b, float tolerance)
{
	return std::abs(a - b) <= tolerance;
}

std::string str(const XrVector3f & v)
{
	return std::format("({:.4f}, {:.4f}, {:.4f})", v.x, v.y, v.z);
}

bool is_valid(XrSpaceLocationFlags flags)
{
	return (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) and (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
}

struct action_set
{
	instance & inst;
	XrActionSet handle = XR_NULL_HANDLE;

	action_set(instance & inst, const char * name, uint32_t priority = 0) :
	        inst(inst)
	{
		XrActionSetCreateInfo info{.type = XR_TYPE_ACTION_SET_CREATE_INFO, .priority = priority};
		strcpy(info.actionSetName, name);
		strcpy(info.localizedActionSetName, name);
		XR_REQUIRE(inst.fn<PFN_xrCreateActionSet>("xrCreateActionSet")(inst.handle, &info, &handle));
	}

	XrAction action(const char * name, XrActionType type, std::vector<XrPath> subaction_paths = {})
	{
		XrActionCreateInfo info{
		        .type = XR_TYPE_ACTION_CREATE_INFO,
		        .actionType = type,
		        .countSubactionPaths = uint32_t(subaction_paths.size()),
		        .subactionPaths = subaction_paths.data(),
		};
		strcpy(info.actionName, name);
		strcpy(info.localizedActionName, name);
		XrAction a;
		XR_REQUIRE(inst.fn<PFN_xrCreateAction>("xrCreateAction")(handle, &info, &a));
		return a;
	}
};

XrResult suggest(instance & inst, const std::string & profile, std::vector<std::pair<XrAction, std::string>> bindings)
{
	std::vector<XrActionSuggestedBinding> b;
	for (auto & [action, path]: bindings)
		b.push_back({action, inst.path(path)});
	XrInteractionProfileSuggestedBinding info{
	        .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
	        .interactionProfile = inst.path(profile),
	        .countSuggestedBindings = uint32_t(b.size()),
	        .suggestedBindings = b.data(),
	};
	return inst.fn<PFN_xrSuggestInteractionProfileBindings>("xrSuggestInteractionProfileBindings")(inst.handle, &info);
}

void attach(session & s, std::vector<XrActionSet> sets)
{
	XrSessionActionSetsAttachInfo info{
	        .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
	        .countActionSets = uint32_t(sets.size()),
	        .actionSets = sets.data(),
	};
	XR_REQUIRE(s.inst.fn<PFN_xrAttachSessionActionSets>("xrAttachSessionActionSets")(s.handle, &info));
}

XrResult sync(session & s, std::vector<XrActionSet> sets, const void * next = nullptr)
{
	std::vector<XrActiveActionSet> active;
	for (auto set: sets)
		active.push_back({set, XR_NULL_PATH});
	XrActionsSyncInfo info{
	        .type = XR_TYPE_ACTIONS_SYNC_INFO,
	        .next = next,
	        .countActiveActionSets = uint32_t(active.size()),
	        .activeActionSets = active.data(),
	};
	return s.inst.fn<PFN_xrSyncActions>("xrSyncActions")(s.handle, &info);
}

XrSpace action_space(session & s, XrAction action, XrPath subaction = XR_NULL_PATH)
{
	XrActionSpaceCreateInfo info{
	        .type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
	        .action = action,
	        .subactionPath = subaction,
	        .poseInActionSpace = {.orientation = {0, 0, 0, 1}},
	};
	XrSpace space;
	XR_REQUIRE(s.inst.fn<PFN_xrCreateActionSpace>("xrCreateActionSpace")(s.handle, &info, &space));
	return space;
}

// Position of the left controller sent by the virtual headset, relative to its head
constexpr XrVector3f left_controller_offset{-0.2, -0.3, -0.35};

// ---------------------------------------------------------------------------
// Tests

TEST("WiVRn.view_poses")
{
	instance inst({});
	session s(inst);
	s.wait_focused();
	s.frame();

	for (auto & v: s.views)
	{
		CHECK(near(v.fov.angleLeft, -0.785398, 0.001) and near(v.fov.angleRight, 0.785398, 0.001), "fov {} {}", v.fov.angleLeft, v.fov.angleRight);
		CHECK(near(v.fov.angleUp, 0.785398, 0.001) and near(v.fov.angleDown, -0.785398, 0.001), "fov {} {}", v.fov.angleUp, v.fov.angleDown);
		auto & q = v.pose.orientation;
		CHECK(near(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w, 1, 0.001), "orientation not normalized");
	}
	auto & a = s.views[0].pose.position;
	auto & b = s.views[1].pose.position;
	float ipd = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
	CHECK(near(ipd, 0.064, 0.001), "IPD {}", ipd);

	auto head = s.locate(s.view, s.local, s.frame_state.predictedDisplayTime);
	CHECK(is_valid(head.locationFlags), "head location flags {:x}", head.locationFlags);
}

TEST("WiVRn.video_stream")
{
	// The frames submitted by the application must reach the headset
	auto before = headset_stats();
	if (before.is_null())
		SKIP("no headset statistics (--headset-stats)");
	instance inst({});
	session s(inst);
	s.wait_focused();
	uint64_t frames_before = before.value("video_frames", 0);
	bool received = s.run_until([&] { return headset_stats().value("video_frames", 0) >= frames_before + 30; }, 10s);
	CHECK(received, "the headset received {} frames", headset_stats().value("video_frames", 0) - frames_before);
}

TEST("WiVRn.XR_FB_display_refresh_rate", XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)
{
	instance inst({XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME});
	session s(inst);
	s.wait_focused();

	auto enumerate = inst.fn<PFN_xrEnumerateDisplayRefreshRatesFB>("xrEnumerateDisplayRefreshRatesFB");
	auto get = inst.fn<PFN_xrGetDisplayRefreshRateFB>("xrGetDisplayRefreshRateFB");
	auto request = inst.fn<PFN_xrRequestDisplayRefreshRateFB>("xrRequestDisplayRefreshRateFB");

	uint32_t count = 0;
	XR_REQUIRE(enumerate(s.handle, 0, &count, nullptr));
	std::vector<float> rates(count);
	XR_REQUIRE(enumerate(s.handle, count, &count, rates.data()));
	std::set<float> got(rates.begin(), rates.end());
	std::set<float> expected(features.refresh_rates.begin(), features.refresh_rates.end());
	REQUIRE(got == expected, "refresh rates {} expected {}", rates, features.refresh_rates);

	float current;
	XR_REQUIRE(get(s.handle, &current));
	CHECK(current == features.refresh_rates.front(), "current rate {}", current);

	CHECK(request(s.handle, 1.f) == XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB);

	if (features.refresh_rates.size() < 2)
		return;

	float target = features.refresh_rates[1];
	XR_REQUIRE(request(s.handle, target));
	bool changed = s.run_until([&] {
		return std::ranges::any_of(s.events, [&](const XrEventDataBuffer & e) {
			return e.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB and
			       ((const XrEventDataDisplayRefreshRateChangedFB &)e).toDisplayRefreshRate == target;
		});
	});
	REQUIRE(changed, "no XrEventDataDisplayRefreshRateChangedFB to {}", target);
	XR_REQUIRE(get(s.handle, &current));
	CHECK(current == target, "current rate {} after requesting {}", current, target);

	// Back to the default
	XR_REQUIRE(request(s.handle, 0));
	s.run_until([&] { float r; return get(s.handle, &r) == XR_SUCCESS and r == features.refresh_rates.front(); }, 5s);
}

TEST("WiVRn.XR_FB_haptic_pcm", XR_FB_HAPTIC_PCM_EXTENSION_NAME)
{
	if (features.controllers != "touch")
		SKIP("needs the touch controller profile");
	instance inst({XR_FB_HAPTIC_PCM_EXTENSION_NAME});
	action_set set(inst, "haptics");
	auto right = inst.path("/user/hand/right");
	auto haptic = set.action("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, {right});
	XR_REQUIRE(suggest(inst, "/interaction_profiles/oculus/touch_controller", {{haptic, "/user/hand/right/output/haptic"}}));
	session s(inst);
	attach(s, {set.handle});
	s.wait_focused();
	XR_REQUIRE(sync(s, {set.handle}));

	XrHapticActionInfo info{.type = XR_TYPE_HAPTIC_ACTION_INFO, .action = haptic, .subactionPath = right};
	XrDevicePcmSampleRateGetInfoFB rate{XR_TYPE_DEVICE_PCM_SAMPLE_RATE_GET_INFO_FB};
	XR_REQUIRE(inst.fn<PFN_xrGetDeviceSampleRateFB>("xrGetDeviceSampleRateFB")(s.handle, &info, &rate));
	CHECK(rate.sampleRate > 0, "sample rate {}", rate.sampleRate);

	uint64_t before = headset_stats().value("haptics_received", 0);
	std::vector<float> samples(200);
	for (size_t i = 0; i < samples.size(); ++i)
		samples[i] = std::sin(i * 0.3f);
	uint32_t consumed = 0;
	XrHapticPcmVibrationFB pcm{
	        .type = XR_TYPE_HAPTIC_PCM_VIBRATION_FB,
	        .bufferSize = uint32_t(samples.size()),
	        .buffer = samples.data(),
	        .sampleRate = 1000,
	        .append = XR_FALSE,
	        .samplesConsumed = &consumed,
	};
	XR_REQUIRE(inst.fn<PFN_xrApplyHapticFeedback>("xrApplyHapticFeedback")(s.handle, &info, (const XrHapticBaseHeader *)&pcm));
	CHECK(consumed > 0 and consumed <= samples.size(), "{} samples consumed", consumed);

	if (not headset_stats().is_null())
	{
		bool received = s.run_until([&] { return headset_stats().value("haptics_received", 0) > before; }, 3s);
		CHECK(received, "the headset received no haptics packet");
	}
}

TEST("WiVRn.XR_EXT_hand_tracking", XR_EXT_HAND_TRACKING_EXTENSION_NAME)
{
	instance inst({XR_EXT_HAND_TRACKING_EXTENSION_NAME});
	auto props = inst.system_properties<XrSystemHandTrackingPropertiesEXT>(XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT);
	session s(inst);
	auto create = inst.fn<PFN_xrCreateHandTrackerEXT>("xrCreateHandTrackerEXT");
	XrHandTrackerCreateInfoEXT info{
	        .type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT,
	        .hand = XR_HAND_LEFT_EXT,
	        .handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT,
	};
	XrHandTrackerEXT tracker;
	XrResult r = create(s.handle, &info, &tracker);
	if (not props.supportsHandTracking)
	{
		CHECK(r == XR_ERROR_FEATURE_UNSUPPORTED, "xrCreateHandTrackerEXT returned {} without hand tracking", to_string(r));
		return;
	}
	XR_REQUIRE(r);
	s.wait_focused();

	std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> joints;
	XrHandJointLocationsEXT locations{
	        .type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT,
	        .jointCount = XR_HAND_JOINT_COUNT_EXT,
	        .jointLocations = joints.data(),
	};
	auto locate = inst.fn<PFN_xrLocateHandJointsEXT>("xrLocateHandJointsEXT");
	bool active = s.run_until([&] {
		XrHandJointsLocateInfoEXT locate_info{
		        .type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT,
		        .baseSpace = s.view,
		        .time = s.frame_state.predictedDisplayTime,
		};
		return locate(tracker, &locate_info, &locations) == XR_SUCCESS and locations.isActive;
	});
	REQUIRE(active, "the hand tracker never became active");

	// The virtual headset puts joint i of the left hand at (-0.2 - 0.01 * (i % 5), -0.3, -0.35 - 0.01 * (i / 5)) from the head
	for (int i: {0, 5, 25})
	{
		XrVector3f expected{-0.2f - 0.01f * (i % 5), -0.3f, -0.35f - 0.01f * (i / 5)};
		const auto & p = joints[i].pose.position;
		CHECK(is_valid(joints[i].locationFlags), "joint {} flags {:x}", i, joints[i].locationFlags);
		CHECK(near(p.x, expected.x, 0.005) and near(p.y, expected.y, 0.005) and near(p.z, expected.z, 0.005),
		      "joint {} at {} expected {}",
		      i,
		      str(p),
		      str(expected));
		CHECK(near(joints[i].radius, 0.01, 0.0005), "joint {} radius {}", i, joints[i].radius);
	}
	XR_REQUIRE(inst.fn<PFN_xrDestroyHandTrackerEXT>("xrDestroyHandTrackerEXT")(tracker));
}

TEST("WiVRn.XR_EXT_hand_tracking_data_source", XR_EXT_HAND_TRACKING_EXTENSION_NAME, XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME)
{
	if (not features.hand_tracking)
		SKIP("no hand tracking");
	instance inst({XR_EXT_HAND_TRACKING_EXTENSION_NAME, XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME});
	session s(inst);
	XrHandTrackingDataSourceEXT sources[] = {XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT};
	XrHandTrackingDataSourceInfoEXT source_info{
	        .type = XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT,
	        .requestedDataSourceCount = 1,
	        .requestedDataSources = sources,
	};
	XrHandTrackerCreateInfoEXT info{
	        .type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT,
	        .next = &source_info,
	        .hand = XR_HAND_RIGHT_EXT,
	        .handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT,
	};
	XrHandTrackerEXT tracker;
	XR_REQUIRE(inst.fn<PFN_xrCreateHandTrackerEXT>("xrCreateHandTrackerEXT")(s.handle, &info, &tracker));
	s.wait_focused();

	std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> joints;
	XrHandTrackingDataSourceStateEXT state{XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT};
	XrHandJointLocationsEXT locations{
	        .type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT,
	        .next = &state,
	        .jointCount = XR_HAND_JOINT_COUNT_EXT,
	        .jointLocations = joints.data(),
	};
	auto locate = inst.fn<PFN_xrLocateHandJointsEXT>("xrLocateHandJointsEXT");
	bool active = s.run_until([&] {
		XrHandJointsLocateInfoEXT locate_info{
		        .type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT,
		        .baseSpace = s.local,
		        .time = s.frame_state.predictedDisplayTime,
		};
		return locate(tracker, &locate_info, &locations) == XR_SUCCESS and locations.isActive;
	});
	REQUIRE(active, "the hand tracker never became active");
	CHECK(state.isActive, "data source state not active");
	CHECK(state.dataSource == XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT, "data source {}", int(state.dataSource));
	XR_REQUIRE(inst.fn<PFN_xrDestroyHandTrackerEXT>("xrDestroyHandTrackerEXT")(tracker));
}

TEST("WiVRn.XR_EXT_eye_gaze_interaction", XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)
{
	instance inst({XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME});
	auto props = inst.system_properties<XrSystemEyeGazeInteractionPropertiesEXT>(XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT);
	CHECK(bool(props.supportsEyeGazeInteraction) == features.eye_gaze, "supportsEyeGazeInteraction {}", props.supportsEyeGazeInteraction);
	if (not props.supportsEyeGazeInteraction)
		return;

	action_set set(inst, "gaze");
	auto gaze = set.action("gaze", XR_ACTION_TYPE_POSE_INPUT);
	XR_REQUIRE(suggest(inst, "/interaction_profiles/ext/eye_gaze_interaction", {{gaze, "/user/eyes_ext/input/gaze_ext/pose"}}));
	session s(inst);
	attach(s, {set.handle});
	auto space = action_space(s, gaze);
	s.wait_focused();

	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
	bool valid = s.run_until([&] {
		if (XR_FAILED(sync(s, {set.handle})))
			return false;
		loc = s.locate(space, s.view, s.frame_state.predictedDisplayTime);
		return is_valid(loc.locationFlags);
	});
	REQUIRE(valid, "gaze pose never valid, flags {:x}", loc.locationFlags);
	// The headset sends the head pose as gaze
	CHECK(near(loc.pose.position.x, 0, 0.01) and near(loc.pose.position.y, 0, 0.01) and near(loc.pose.position.z, 0, 0.01),
	      "gaze at {} relative to the head",
	      str(loc.pose.position));
}

TEST("WiVRn.XR_EXT_palm_pose", XR_EXT_PALM_POSE_EXTENSION_NAME)
{
	if (features.controllers != "touch")
		SKIP("needs the touch controller profile");
	instance inst({XR_EXT_PALM_POSE_EXTENSION_NAME});
	action_set set(inst, "palm");
	auto left = inst.path("/user/hand/left");
	auto palm = set.action("palm", XR_ACTION_TYPE_POSE_INPUT, {left});
	XR_REQUIRE(suggest(inst, "/interaction_profiles/oculus/touch_controller", {{palm, "/user/hand/left/input/palm_ext/pose"}}));
	session s(inst);
	attach(s, {set.handle});
	auto space = action_space(s, palm, left);
	s.wait_focused();

	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
	bool valid = s.run_until([&] {
		if (XR_FAILED(sync(s, {set.handle})))
			return false;
		loc = s.locate(space, s.view, s.frame_state.predictedDisplayTime);
		return is_valid(loc.locationFlags);
	});
	REQUIRE(valid, "palm pose never valid");
	CHECK(near(loc.pose.position.x, left_controller_offset.x, 0.01) and near(loc.pose.position.y, left_controller_offset.y, 0.01) and
	              near(loc.pose.position.z, left_controller_offset.z, 0.01),
	      "palm at {} expected {}",
	      str(loc.pose.position),
	      str(left_controller_offset));
}

TEST("WiVRn.XR_FB_face_tracking2", XR_FB_FACE_TRACKING2_EXTENSION_NAME)
{
	instance inst({XR_FB_FACE_TRACKING2_EXTENSION_NAME});
	auto props = inst.system_properties<XrSystemFaceTrackingProperties2FB>(XR_TYPE_SYSTEM_FACE_TRACKING_PROPERTIES2_FB);
	CHECK(bool(props.supportsVisualFaceTracking) == (features.face == "fb2"), "supportsVisualFaceTracking {}", props.supportsVisualFaceTracking);
	session s(inst);
	XrFaceTrackingDataSource2FB sources[] = {XR_FACE_TRACKING_DATA_SOURCE2_VISUAL_FB};
	XrFaceTrackerCreateInfo2FB info{
	        .type = XR_TYPE_FACE_TRACKER_CREATE_INFO2_FB,
	        .faceExpressionSet = XR_FACE_EXPRESSION_SET2_DEFAULT_FB,
	        .requestedDataSourceCount = 1,
	        .requestedDataSources = sources,
	};
	XrFaceTracker2FB tracker;
	XrResult r = inst.fn<PFN_xrCreateFaceTracker2FB>("xrCreateFaceTracker2FB")(s.handle, &info, &tracker);
	if (not props.supportsVisualFaceTracking)
	{
		CHECK(XR_FAILED(r), "xrCreateFaceTracker2FB returned {} without face tracking", to_string(r));
		return;
	}
	XR_REQUIRE(r);
	s.wait_focused();

	std::array<float, XR_FACE_EXPRESSION2_COUNT_FB> weights;
	std::array<float, XR_FACE_CONFIDENCE2_COUNT_FB> confidences;
	XrFaceExpressionWeights2FB result{
	        .type = XR_TYPE_FACE_EXPRESSION_WEIGHTS2_FB,
	        .weightCount = weights.size(),
	        .weights = weights.data(),
	        .confidenceCount = confidences.size(),
	        .confidences = confidences.data(),
	};
	auto get = inst.fn<PFN_xrGetFaceExpressionWeights2FB>("xrGetFaceExpressionWeights2FB");
	bool valid = s.run_until([&] {
		XrFaceExpressionInfo2FB get_info{.type = XR_TYPE_FACE_EXPRESSION_INFO2_FB, .time = s.frame_state.predictedDisplayTime};
		return get(tracker, &get_info, &result) == XR_SUCCESS and result.isValid;
	});
	REQUIRE(valid, "face expression weights never valid");
	CHECK(near(weights[0], 0.1, 0.001) and near(weights.back(), 0.1, 0.001), "weights {} {}", weights[0], weights.back());
	CHECK(near(confidences[0], 1, 0.001), "confidence {}", confidences[0]);
	XR_REQUIRE(inst.fn<PFN_xrDestroyFaceTracker2FB>("xrDestroyFaceTracker2FB")(tracker));
}

TEST("WiVRn.XR_ANDROID_face_tracking", XR_ANDROID_FACE_TRACKING_EXTENSION_NAME)
{
	instance inst({XR_ANDROID_FACE_TRACKING_EXTENSION_NAME});
	auto props = inst.system_properties<XrSystemFaceTrackingPropertiesANDROID>(XR_TYPE_SYSTEM_FACE_TRACKING_PROPERTIES_ANDROID);
	CHECK(bool(props.supportsFaceTracking) == (features.face == "android"), "supportsFaceTracking {}", props.supportsFaceTracking);
	session s(inst);
	XrFaceTrackerCreateInfoANDROID info{XR_TYPE_FACE_TRACKER_CREATE_INFO_ANDROID};
	XrFaceTrackerANDROID tracker;
	XrResult r = inst.fn<PFN_xrCreateFaceTrackerANDROID>("xrCreateFaceTrackerANDROID")(s.handle, &info, &tracker);
	if (not props.supportsFaceTracking)
	{
		CHECK(XR_FAILED(r), "xrCreateFaceTrackerANDROID returned {} without face tracking", to_string(r));
		return;
	}
	XR_REQUIRE(r);
	s.wait_focused();

	std::array<float, XR_FACE_PARAMETER_COUNT_ANDROID> parameters;
	std::array<float, XR_FACE_REGION_CONFIDENCE_COUNT_ANDROID> confidences;
	XrFaceStateANDROID state{
	        .type = XR_TYPE_FACE_STATE_ANDROID,
	        .parametersCapacityInput = parameters.size(),
	        .parameters = parameters.data(),
	        .regionConfidencesCapacityInput = confidences.size(),
	        .regionConfidences = confidences.data(),
	};
	auto get = inst.fn<PFN_xrGetFaceStateANDROID>("xrGetFaceStateANDROID");
	bool valid = s.run_until([&] {
		XrFaceStateGetInfoANDROID get_info{.type = XR_TYPE_FACE_STATE_GET_INFO_ANDROID, .time = s.frame_state.predictedDisplayTime};
		return get(tracker, &get_info, &state) == XR_SUCCESS and state.isValid;
	});
	REQUIRE(valid, "face state never valid");
	CHECK(state.faceTrackingState == XR_FACE_TRACKING_STATE_TRACKING_ANDROID, "state {}", int(state.faceTrackingState));
	CHECK(near(parameters[0], 0.1, 0.001), "parameter {}", parameters[0]);
	CHECK(near(confidences[0], 1, 0.001), "confidence {}", confidences[0]);
	XR_REQUIRE(inst.fn<PFN_xrDestroyFaceTrackerANDROID>("xrDestroyFaceTrackerANDROID")(tracker));
}

TEST("WiVRn.XR_HTC_facial_tracking", XR_HTC_FACIAL_TRACKING_EXTENSION_NAME)
{
	instance inst({XR_HTC_FACIAL_TRACKING_EXTENSION_NAME});
	auto props = inst.system_properties<XrSystemFacialTrackingPropertiesHTC>(XR_TYPE_SYSTEM_FACIAL_TRACKING_PROPERTIES_HTC);
	bool expected = features.face == "htc";
	CHECK(bool(props.supportEyeFacialTracking) == expected and bool(props.supportLipFacialTracking) == expected,
	      "eye {} lip {}",
	      props.supportEyeFacialTracking,
	      props.supportLipFacialTracking);
	session s(inst);
	XrFacialTrackerCreateInfoHTC info{.type = XR_TYPE_FACIAL_TRACKER_CREATE_INFO_HTC, .facialTrackingType = XR_FACIAL_TRACKING_TYPE_LIP_DEFAULT_HTC};
	XrFacialTrackerHTC tracker;
	XrResult r = inst.fn<PFN_xrCreateFacialTrackerHTC>("xrCreateFacialTrackerHTC")(s.handle, &info, &tracker);
	if (not props.supportLipFacialTracking)
	{
		CHECK(XR_FAILED(r), "xrCreateFacialTrackerHTC returned {} without facial tracking", to_string(r));
		return;
	}
	XR_REQUIRE(r);
	s.wait_focused();

	std::array<float, XR_FACIAL_EXPRESSION_LIP_COUNT_HTC> weights;
	XrFacialExpressionsHTC expressions{
	        .type = XR_TYPE_FACIAL_EXPRESSIONS_HTC,
	        .expressionCount = weights.size(),
	        .expressionWeightings = weights.data(),
	};
	auto get = inst.fn<PFN_xrGetFacialExpressionsHTC>("xrGetFacialExpressionsHTC");
	bool active = s.run_until([&] {
		expressions.sampleTime = s.frame_state.predictedDisplayTime;
		return get(tracker, &expressions) == XR_SUCCESS and expressions.isActive;
	});
	REQUIRE(active, "facial expressions never active");
	CHECK(near(weights[0], 0.1, 0.001), "weight {}", weights[0]);
	XR_REQUIRE(inst.fn<PFN_xrDestroyFacialTrackerHTC>("xrDestroyFacialTrackerHTC")(tracker));
}

// Body tracking: the virtual headset puts the root 0.6 m under the head and joint i i cm above the root
void check_body(instance & inst, session & s, XrBodyJointSetFB joint_set, uint32_t joint_count, std::function<void(XrBodyTrackerFB)> extra = {})
{
	XrBodyTrackerCreateInfoFB info{.type = XR_TYPE_BODY_TRACKER_CREATE_INFO_FB, .bodyJointSet = joint_set};
	XrBodyTrackerFB tracker;
	XR_REQUIRE(inst.fn<PFN_xrCreateBodyTrackerFB>("xrCreateBodyTrackerFB")(s.handle, &info, &tracker));
	s.wait_focused();

	std::vector<XrBodyJointLocationFB> joints(joint_count);
	XrBodyJointLocationsFB locations{
	        .type = XR_TYPE_BODY_JOINT_LOCATIONS_FB,
	        .jointCount = joint_count,
	        .jointLocations = joints.data(),
	};
	auto locate = inst.fn<PFN_xrLocateBodyJointsFB>("xrLocateBodyJointsFB");
	bool active = s.run_until([&] {
		XrBodyJointsLocateInfoFB locate_info{
		        .type = XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB,
		        .baseSpace = s.view,
		        .time = s.frame_state.predictedDisplayTime,
		};
		return locate(tracker, &locate_info, &locations) == XR_SUCCESS and locations.isActive;
	});
	REQUIRE(active, "the body tracker never became active");
	CHECK(near(locations.confidence, 1, 0.001), "confidence {}", locations.confidence);
	for (uint32_t i: {0u, 1u, joint_count - 1})
	{
		const auto & p = joints[i].pose.position;
		float y = -0.6f + (i == 0 ? 0 : 0.01f * i);
		CHECK(is_valid(joints[i].locationFlags), "joint {} flags {:x}", i, joints[i].locationFlags);
		CHECK(near(p.x, 0, 0.005) and near(p.y, y, 0.005) and near(p.z, 0, 0.005), "joint {} at {} expected (0, {}, 0)", i, str(p), y);
	}

	std::vector<XrBodySkeletonJointFB> skeleton_joints(joint_count);
	XrBodySkeletonFB skeleton{.type = XR_TYPE_BODY_SKELETON_FB, .jointCount = joint_count, .joints = skeleton_joints.data()};
	XR_REQUIRE(inst.fn<PFN_xrGetBodySkeletonFB>("xrGetBodySkeletonFB")(tracker, &skeleton));
	CHECK(skeleton_joints[1].parentJoint == 0 and skeleton_joints[1].joint == 1, "skeleton joint 1: {} parent {}", skeleton_joints[1].joint, skeleton_joints[1].parentJoint);

	if (extra)
		extra(tracker);
	XR_REQUIRE(inst.fn<PFN_xrDestroyBodyTrackerFB>("xrDestroyBodyTrackerFB")(tracker));
}

TEST("WiVRn.XR_FB_body_tracking", XR_FB_BODY_TRACKING_EXTENSION_NAME)
{
	instance inst({XR_FB_BODY_TRACKING_EXTENSION_NAME});
	auto props = inst.system_properties<XrSystemBodyTrackingPropertiesFB>(XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB);
	bool expected = features.body == "fb" or features.body == "meta";
	CHECK(bool(props.supportsBodyTracking) == expected, "supportsBodyTracking {}", props.supportsBodyTracking);
	if (features.body != "fb")
	{
		session s(inst);
		XrBodyTrackerCreateInfoFB info{.type = XR_TYPE_BODY_TRACKER_CREATE_INFO_FB, .bodyJointSet = XR_BODY_JOINT_SET_DEFAULT_FB};
		XrBodyTrackerFB tracker;
		XrResult r = inst.fn<PFN_xrCreateBodyTrackerFB>("xrCreateBodyTrackerFB")(s.handle, &info, &tracker);
		if (not expected)
			CHECK(XR_FAILED(r), "xrCreateBodyTrackerFB returned {} without body tracking", to_string(r));
		else if (XR_SUCCEEDED(r))
			inst.fn<PFN_xrDestroyBodyTrackerFB>("xrDestroyBodyTrackerFB")(tracker);
		return;
	}
	session s(inst);
	check_body(inst, s, XR_BODY_JOINT_SET_DEFAULT_FB, XR_BODY_JOINT_COUNT_FB);
}

TEST("WiVRn.XR_META_body_tracking_full_body", XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME)
{
	instance inst({XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME});
	auto props = inst.system_properties<XrSystemPropertiesBodyTrackingFullBodyMETA>(XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META);
	CHECK(bool(props.supportsFullBodyTracking) == (features.body == "meta"), "supportsFullBodyTracking {}", props.supportsFullBodyTracking);
	if (features.body != "meta")
		return;
	session s(inst);
	check_body(inst, s, XR_BODY_JOINT_SET_FULL_BODY_META, XR_FULL_BODY_JOINT_COUNT_META);
}

TEST("WiVRn.XR_META_body_tracking_fidelity", XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME)
{
	instance inst({XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME});
	auto props = inst.system_properties<XrSystemPropertiesBodyTrackingFidelityMETA>(XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FIDELITY_META);
	if (features.body != "fb" and features.body != "meta")
	{
		CHECK(not props.supportsBodyTrackingFidelity, "supportsBodyTrackingFidelity without body tracking");
		return;
	}
	if (not props.supportsBodyTrackingFidelity)
		SKIP("body tracking fidelity not supported by the runtime");
	session s(inst);
	check_body(inst, s, XR_BODY_JOINT_SET_DEFAULT_FB, XR_BODY_JOINT_COUNT_FB, [&](XrBodyTrackerFB tracker) {
		XR_REQUIRE(inst.fn<PFN_xrRequestBodyTrackingFidelityMETA>("xrRequestBodyTrackingFidelityMETA")(tracker, XR_BODY_TRACKING_FIDELITY_HIGH_META));
	});
}

TEST("WiVRn.XR_META_body_tracking_calibration", XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME)
{
	instance inst({XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME});
	auto props = inst.system_properties<XrSystemPropertiesBodyTrackingCalibrationMETA>(XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_CALIBRATION_META);
	if (features.body != "fb" and features.body != "meta")
	{
		CHECK(not props.supportsHeightOverride, "supportsHeightOverride without body tracking");
		return;
	}
	if (not props.supportsHeightOverride)
		SKIP("height override not supported by the runtime");
	session s(inst);
	check_body(inst, s, XR_BODY_JOINT_SET_DEFAULT_FB, XR_BODY_JOINT_COUNT_FB, [&](XrBodyTrackerFB tracker) {
		XrBodyTrackingCalibrationInfoMETA calibration{.type = XR_TYPE_BODY_TRACKING_CALIBRATION_INFO_META, .bodyHeight = 1.8};
		XR_REQUIRE(inst.fn<PFN_xrSuggestBodyTrackingCalibrationOverrideMETA>("xrSuggestBodyTrackingCalibrationOverrideMETA")(tracker, &calibration));
		XR_REQUIRE(inst.fn<PFN_xrResetBodyTrackingCalibrationMETA>("xrResetBodyTrackingCalibrationMETA")(tracker));
	});
}

TEST("WiVRn.XR_MSFT_unbounded_reference_space", XR_MSFT_UNBOUNDED_REFERENCE_SPACE_EXTENSION_NAME)
{
	instance inst({XR_MSFT_UNBOUNDED_REFERENCE_SPACE_EXTENSION_NAME});
	session s(inst);
	auto enumerate = inst.fn<PFN_xrEnumerateReferenceSpaces>("xrEnumerateReferenceSpaces");
	uint32_t count = 0;
	XR_REQUIRE(enumerate(s.handle, 0, &count, nullptr));
	std::vector<XrReferenceSpaceType> types(count);
	XR_REQUIRE(enumerate(s.handle, count, &count, types.data()));
	REQUIRE(std::ranges::contains(types, XR_REFERENCE_SPACE_TYPE_UNBOUNDED_MSFT), "the extension is advertised but the unbounded reference space is not available");
	auto space = s.create_reference_space(XR_REFERENCE_SPACE_TYPE_UNBOUNDED_MSFT);
	s.wait_focused();
	auto loc = s.locate(s.view, space, s.frame_state.predictedDisplayTime);
	CHECK(is_valid(loc.locationFlags), "view in unbounded space flags {:x}", loc.locationFlags);
}

TEST("WiVRn.XR_EXT_active_action_set_priority", XR_EXT_ACTIVE_ACTION_SET_PRIORITY_EXTENSION_NAME)
{
	instance inst({XR_EXT_ACTIVE_ACTION_SET_PRIORITY_EXTENSION_NAME});
	action_set low(inst, "low", 1);
	action_set high(inst, "high", 2);
	auto a = low.action("a_low", XR_ACTION_TYPE_BOOLEAN_INPUT);
	auto b = high.action("a_high", XR_ACTION_TYPE_BOOLEAN_INPUT);
	XR_REQUIRE(suggest(inst, "/interaction_profiles/khr/simple_controller", {{a, "/user/hand/left/input/select/click"}, {b, "/user/hand/left/input/select/click"}}));
	session s(inst);
	attach(s, {low.handle, high.handle});
	s.wait_focused();
	XrActiveActionSetPriorityEXT priorities[] = {{low.handle, 10}, {high.handle, 1}};
	XrActiveActionSetPrioritiesEXT info{
	        .type = XR_TYPE_ACTIVE_ACTION_SET_PRIORITIES_EXT,
	        .actionSetPriorityCount = 2,
	        .actionSetPriorities = priorities,
	};
	XR_REQUIRE(sync(s, {low.handle, high.handle}, &info));
}

TEST("WiVRn.XR_EXT_future", XR_EXT_FUTURE_EXTENSION_NAME)
{
	instance inst({XR_EXT_FUTURE_EXTENSION_NAME});
	XrFuturePollInfoEXT poll{.type = XR_TYPE_FUTURE_POLL_INFO_EXT, .future = XR_NULL_FUTURE_EXT};
	XrFuturePollResultEXT result{XR_TYPE_FUTURE_POLL_RESULT_EXT};
	XrResult r = inst.fn<PFN_xrPollFutureEXT>("xrPollFutureEXT")(inst.handle, &poll, &result);
	// XR_ERROR_FUTURE_INVALID_EXT would be more precise, the registry also allows these
	auto is_error = [](XrResult r) { return r == XR_ERROR_FUTURE_INVALID_EXT or r == XR_ERROR_HANDLE_INVALID or r == XR_ERROR_VALIDATION_FAILURE; };
	CHECK(is_error(r), "xrPollFutureEXT(XR_NULL_FUTURE_EXT) returned {}", to_string(r));
	XrFutureCancelInfoEXT cancel{.type = XR_TYPE_FUTURE_CANCEL_INFO_EXT, .future = XR_NULL_FUTURE_EXT};
	r = inst.fn<PFN_xrCancelFutureEXT>("xrCancelFutureEXT")(inst.handle, &cancel);
	CHECK(is_error(r), "xrCancelFutureEXT(XR_NULL_FUTURE_EXT) returned {}", to_string(r));
}

std::string self_exe;

// Body of the overlay process, see WiVRn.XR_EXTX_overlay
int overlay_child()
{
	try
	{
		instance inst({XR_EXTX_OVERLAY_EXTENSION_NAME});
		XrSessionCreateInfoOverlayEXTX overlay_info{.type = XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX, .sessionLayersPlacement = 1};
		session overlay(inst, &overlay_info);
		if (not overlay.run_until([&] { return overlay.state == XR_SESSION_STATE_VISIBLE or overlay.state == XR_SESSION_STATE_FOCUSED; }))
		{
			std::println(stderr, "overlay session state {}", int(overlay.state));
			return 1;
		}
		for (int i = 0; i < 30; ++i)
			overlay.frame();
		return 0;
	}
	catch (std::exception & e)
	{
		std::println(stderr, "overlay: {}", e.what());
		return 1;
	}
}

TEST("WiVRn.XR_EXTX_overlay", XR_EXTX_OVERLAY_EXTENSION_NAME)
{
	// A main application and an overlay on top of it, in another process: the runtime
	// client library allows one instance per process
	instance main_inst({});
	session main_session(main_inst);
	main_session.wait_focused();

	pid_t child = fork();
	REQUIRE(child >= 0);
	if (child == 0)
	{
		execl(self_exe.c_str(), self_exe.c_str(), "--overlay-child", "--loader", loader_path.c_str(), nullptr);
		_exit(127);
	}
	int status = 0;
	auto end = std::chrono::steady_clock::now() + 20s;
	while (waitpid(child, &status, WNOHANG) == 0)
	{
		if (std::chrono::steady_clock::now() > end)
		{
			kill(child, SIGKILL);
			waitpid(child, &status, 0);
			break;
		}
		main_session.frame();
	}
	CHECK(WIFEXITED(status) and WEXITSTATUS(status) == 0, "overlay process status {:x}", status);
	main_session.frame();
	CHECK(main_session.running(), "main session state {}", int(main_session.state));
}

TEST("WiVRn.XR_KHR_composition_layer_color_scale_bias", XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME)
{
	instance inst({XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME});
	session s(inst);
	s.wait_focused();
	XrCompositionLayerColorScaleBiasKHR scale_bias{
	        .type = XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR,
	        .colorScale = {0.5, 0.5, 0.5, 1},
	        .colorBias = {0.1, 0, 0, 0},
	};
	for (int i = 0; i < 30; ++i)
		s.frame(&scale_bias);
}

TEST("WiVRn.XR_KHR_composition_layer_equirect2", XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME)
{
	instance inst({XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME});
	session s(inst);
	s.wait_focused();
	XrCompositionLayerEquirect2KHR equirect{
	        .type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR,
	        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
	        .space = s.local,
	        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
	        .subImage = {.swapchain = s.swapchain, .imageRect = {{0, 0}, s.extent}, .imageArrayIndex = 0},
	        .pose = {.orientation = {0, 0, 0, 1}, .position = {0, 0, -1}},
	        .radius = 2,
	        .centralHorizontalAngle = 1,
	        .upperVerticalAngle = 0.5,
	        .lowerVerticalAngle = -0.5,
	};
	for (int i = 0; i < 30; ++i)
		s.frame(nullptr, {(const XrCompositionLayerBaseHeader *)&equirect});
}

void check_input_attachment_swapchain(const char * extension)
{
	instance inst({extension});
	session s(inst);
	XrSwapchain swapchain;
	XR_REQUIRE(s.try_create_swapchain(XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR, nullptr, s.format, swapchain));
	XR_REQUIRE(inst.fn<PFN_xrDestroySwapchain>("xrDestroySwapchain")(swapchain));
}

TEST("WiVRn.swapchain_usage_input_attachment", XR_KHR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_EXTENSION_NAME, XR_MND_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_EXTENSION_NAME)
{
	check_input_attachment_swapchain(XR_KHR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_EXTENSION_NAME);
	check_input_attachment_swapchain(XR_MND_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_EXTENSION_NAME);
}

TEST("WiVRn.XR_KHR_vulkan_swapchain_format_list", XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME)
{
	instance inst({XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME});
	session s(inst);
	auto formats = s.swapchain_formats();
	VkFormat srgb = VK_FORMAT_R8G8B8A8_SRGB, unorm = VK_FORMAT_R8G8B8A8_UNORM;
	if (not std::ranges::contains(formats, srgb) or not std::ranges::contains(formats, unorm))
		SKIP("R8G8B8A8 sRGB and UNORM swapchain formats are not both available");
	VkFormat view_formats[] = {srgb, unorm};
	XrVulkanSwapchainFormatListCreateInfoKHR list{
	        .type = XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR,
	        .viewFormatCount = 2,
	        .viewFormats = view_formats,
	};
	XrSwapchain swapchain;
	XR_REQUIRE(s.try_create_swapchain(XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT, &list, srgb, swapchain));
	XR_REQUIRE(inst.fn<PFN_xrDestroySwapchain>("xrDestroySwapchain")(swapchain));
}

TEST("WiVRn.XR_KHR_opengl_es_enable", XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)
{
	instance inst({XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME});
	XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
	XR_REQUIRE(inst.fn<PFN_xrGetOpenGLESGraphicsRequirementsKHR>("xrGetOpenGLESGraphicsRequirementsKHR")(inst.handle, inst.system, &req));
	CHECK(req.minApiVersionSupported <= req.maxApiVersionSupported, "min {:x} max {:x}", req.minApiVersionSupported, req.maxApiVersionSupported);
}

TEST("WiVRn.XR_MNDX_egl_enable", "XR_MNDX_egl_enable")
{
	// The extension only adds a graphics binding: check it can be enabled with OpenGL ES
	instance inst({"XR_MNDX_egl_enable", XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME});
	XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
	XR_REQUIRE(inst.fn<PFN_xrGetOpenGLESGraphicsRequirementsKHR>("xrGetOpenGLESGraphicsRequirementsKHR")(inst.handle, inst.system, &req));
}

TEST("WiVRn.XR_MND_query_egl_device", "XR_MND_query_egl_device")
{
	void * egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
	if (not egl)
		SKIP("libEGL.so.1 not available");
	using egl_get_proc_address_t = void (*(*)(const char *))();
	auto get_proc = (egl_get_proc_address_t)dlsym(egl, "eglGetProcAddress");
	REQUIRE(get_proc);

	instance inst({"XR_MND_query_egl_device"});
	struct
	{
		XrStructureType type;
		const void * next;
		XrSystemId systemId;
		egl_get_proc_address_t getProcAddress;
	} info{XrStructureType(1000445001), nullptr, inst.system, get_proc};
	struct
	{
		XrStructureType type;
		void * next;
		void * eglDevice;
	} device{XrStructureType(1000445002), nullptr, nullptr};
	using get_device_t = XrResult(XRAPI_PTR *)(XrInstance, const void *, void *);
	XrResult r = inst.fn<get_device_t>("xrGetSystemEGLDeviceMND")(inst.handle, &info, &device);
	// Whether a matching EGL device exists depends on the GPU and drivers
	CHECK(r == XR_SUCCESS or r == XrResult(-1000445001) or r == XrResult(-1000445002), "xrGetSystemEGLDeviceMND returned {}", to_string(r));
	if (r == XR_SUCCESS)
		CHECK(device.eglDevice != nullptr);
}

TEST("WiVRn.XR_MNDX_force_feedback_curl", XR_EXT_HAND_TRACKING_EXTENSION_NAME, XR_MNDX_FORCE_FEEDBACK_CURL_EXTENSION_NAME)
{
	if (not features.hand_tracking)
		SKIP("no hand tracking");
	instance inst({XR_EXT_HAND_TRACKING_EXTENSION_NAME, XR_MNDX_FORCE_FEEDBACK_CURL_EXTENSION_NAME});
	auto props = inst.system_properties<XrSystemForceFeedbackCurlPropertiesMNDX>(XR_TYPE_SYSTEM_FORCE_FEEDBACK_CURL_PROPERTIES_MNDX);
	CHECK(not props.supportsForceFeedbackCurl, "WiVRn headsets have no force feedback");
	session s(inst);
	XrHandTrackerCreateInfoEXT info{
	        .type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT,
	        .hand = XR_HAND_LEFT_EXT,
	        .handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT,
	};
	XrHandTrackerEXT tracker;
	XR_REQUIRE(inst.fn<PFN_xrCreateHandTrackerEXT>("xrCreateHandTrackerEXT")(s.handle, &info, &tracker));
	s.wait_focused();
	XrForceFeedbackCurlApplyLocationMNDX location{XR_FORCE_FEEDBACK_CURL_LOCATION_INDEX_CURL_MNDX, 0.5};
	XrForceFeedbackCurlApplyLocationsMNDX locations{
	        .type = XR_TYPE_FORCE_FEEDBACK_CURL_APPLY_LOCATIONS_MNDX,
	        .locationCount = 1,
	        .locations = &location,
	};
	XrResult r = inst.fn<PFN_xrApplyForceFeedbackCurlMNDX>("xrApplyForceFeedbackCurlMNDX")(tracker, &locations);
	CHECK(r == XR_SUCCESS or r == XR_ERROR_FEATURE_UNSUPPORTED, "xrApplyForceFeedbackCurlMNDX returned {}", to_string(r));
	XR_REQUIRE(inst.fn<PFN_xrDestroyHandTrackerEXT>("xrDestroyHandTrackerEXT")(tracker));
}

TEST("WiVRn.XR_MNDX_system_buttons", XR_MNDX_SYSTEM_BUTTONS_EXTENSION_NAME)
{
	// The extension adds /input/system/click to controllers that do not have it in the specification
	{
		instance inst({});
		action_set set(inst, "system");
		auto action = set.action("system", XR_ACTION_TYPE_BOOLEAN_INPUT);
		CHECK(suggest(inst, "/interaction_profiles/microsoft/motion_controller", {{action, "/user/hand/left/input/system/click"}}) == XR_ERROR_PATH_UNSUPPORTED,
		      "system button accepted without the extension");
	}
	instance inst({XR_MNDX_SYSTEM_BUTTONS_EXTENSION_NAME});
	action_set set(inst, "system");
	auto action = set.action("system", XR_ACTION_TYPE_BOOLEAN_INPUT);
	XR_REQUIRE(suggest(inst, "/interaction_profiles/microsoft/motion_controller", {{action, "/user/hand/left/input/system/click"}}));
}

TEST("WiVRn.XR_MNDX_xdev_space", XR_MNDX_XDEV_SPACE_EXTENSION_NAME)
{
	instance inst({XR_MNDX_XDEV_SPACE_EXTENSION_NAME});
	auto props = inst.system_properties<XrSystemXDevSpacePropertiesMNDX>(XR_TYPE_SYSTEM_XDEV_SPACE_PROPERTIES_MNDX);
	REQUIRE(props.supportsXDevSpace);
	session s(inst);
	s.wait_focused();

	XrCreateXDevListInfoMNDX list_info{XR_TYPE_CREATE_XDEV_LIST_INFO_MNDX};
	XrXDevListMNDX list;
	XR_REQUIRE(inst.fn<PFN_xrCreateXDevListMNDX>("xrCreateXDevListMNDX")(s.handle, &list_info, &list));
	auto enumerate = inst.fn<PFN_xrEnumerateXDevsMNDX>("xrEnumerateXDevsMNDX");
	uint32_t count = 0;
	XR_REQUIRE(enumerate(list, 0, &count, nullptr));
	std::vector<XrXDevIdMNDX> ids(count);
	XR_REQUIRE(enumerate(list, count, &count, ids.data()));
	REQUIRE(count > 0);

	bool located = false;
	std::vector<std::string> names;
	for (auto id: ids)
	{
		XrGetXDevInfoMNDX get{.type = XR_TYPE_GET_XDEV_INFO_MNDX, .id = id};
		XrXDevPropertiesMNDX xdev{XR_TYPE_XDEV_PROPERTIES_MNDX};
		XR_REQUIRE(inst.fn<PFN_xrGetXDevPropertiesMNDX>("xrGetXDevPropertiesMNDX")(list, &get, &xdev));
		names.push_back(xdev.name);
		if (not xdev.canCreateSpace)
			continue;
		XrCreateXDevSpaceInfoMNDX space_info{
		        .type = XR_TYPE_CREATE_XDEV_SPACE_INFO_MNDX,
		        .xdevList = list,
		        .id = id,
		        .offset = {.orientation = {0, 0, 0, 1}},
		};
		XrSpace space;
		XR_REQUIRE(inst.fn<PFN_xrCreateXDevSpaceMNDX>("xrCreateXDevSpaceMNDX")(s.handle, &space_info, &space));
		s.frame();
		if (is_valid(s.locate(space, s.local, s.frame_state.predictedDisplayTime).locationFlags))
			located = true;
	}
	CHECK(located, "no xdev space could be located, devices: {}", names);
	XR_REQUIRE(inst.fn<PFN_xrDestroyXDevListMNDX>("xrDestroyXDevListMNDX")(list));
}

TEST("WiVRn.monado_interaction_profiles")
{
	// Interaction profiles only known to Monado (the CTS checks the registered ones)
	struct profile
	{
		const char * extension;
		const char * path;
		const char * binding;
		XrActionType type;
	};
	const profile profiles[] = {
	        {"XR_MNDX_ball_on_a_stick_controller", "/interaction_profiles/mndx/ball_on_a_stick_controller", "/user/hand/left/input/system/click", XR_ACTION_TYPE_BOOLEAN_INPUT},
	        {"XR_MNDX_hydra", "/interaction_profiles/mndx/hydra", "/user/hand/left/input/1/click", XR_ACTION_TYPE_BOOLEAN_INPUT},
	        {"XR_MNDX_flipvr", "/interaction_profiles/mndx/flipvr", "/user/hand/left/input/joystick/click", XR_ACTION_TYPE_BOOLEAN_INPUT},
	        {"XR_MNDX_oculus_remote", "/interaction_profiles/mndx/oculus_remote", "/user/gamepad/input/select/click", XR_ACTION_TYPE_BOOLEAN_INPUT},
	        {"XR_MNDX_blubur_s1", "/interaction_profiles/mndx/blubur_s1_mndx", "/user/head/input/menu/click", XR_ACTION_TYPE_BOOLEAN_INPUT},
	        {"XR_MNDX_psvr2_interaction", "/interaction_profiles/mndx/psvr2_mndx", "/user/head/input/system/click", XR_ACTION_TYPE_BOOLEAN_INPUT},
	        {"XR_MND_contactglove2_interaction", "/interaction_profiles/mnd/contactglove2_mnd", "/user/hand/left/input/squeeze/value", XR_ACTION_TYPE_FLOAT_INPUT},
	        {"XR_VALVE_frame_controller_interaction", "/interaction_profiles/valve/frame_controller_valve", "/user/hand/right/input/a/click", XR_ACTION_TYPE_BOOLEAN_INPUT},
	};
	int tested = 0;
	for (const auto & p: profiles)
	{
		if (not xr_loader().extensions.contains(p.extension))
			continue;
		++tested;
		instance inst({p.extension});
		action_set set(inst, "profile");
		auto action = set.action("input", p.type);
		XrResult r = suggest(inst, p.path, {{action, p.binding}});
		CHECK(r == XR_SUCCESS, "{} {}: {}", p.path, p.binding, to_string(r));
	}
	if (tested == 0)
		SKIP("none of these extensions is advertised");
}

// ---------------------------------------------------------------------------

std::string xml_escape(const std::string & s)
{
	std::string out;
	for (char c: s)
	{
		switch (c)
		{
			case '<':
				out += "&lt;";
				break;
			case '>':
				out += "&gt;";
				break;
			case '&':
				out += "&amp;";
				break;
			case '"':
				out += "&quot;";
				break;
			default:
				out += c;
		}
	}
	return out;
}

struct result
{
	std::string name;
	std::string status; // pass, fail, skip
	std::string message;
	double seconds;
};

} // namespace

int main(int argc, char * argv[])
{
	CLI::App app{"OpenXR tests for WiVRn with the virtual headset"};
	app.allow_extras();
	std::string junit;
	bool list = false;
	std::vector<std::string> filters;
	app.add_option("--loader", loader_path, "OpenXR loader library")->capture_default_str();
	app.add_option("--junit", junit, "JUnit XML report");
	app.add_flag("--list", list, "list the tests");
	app.add_option("--headset-stats", features.stats_file, "statistics file of the virtual headset");
	app.add_option("--controllers", features.controllers);
	app.add_option("--hand-tracking", features.hand_tracking);
	app.add_option("--eye-gaze", features.eye_gaze);
	app.add_option("--palm-pose", features.palm_pose);
	app.add_option("--user-presence", features.user_presence);
	app.add_option("--face", features.face);
	app.add_option("--body", features.body);
	app.add_option("--generic-trackers", features.generic_trackers);
	app.add_option("--refresh-rate", features.refresh_rates);
	app.add_option("--test", filters, "test name glob (repeatable)");
	bool child = false;
	app.add_flag("--overlay-child", child)->group("");
	CLI11_PARSE(app, argc, argv);

	self_exe = std::filesystem::read_symlink("/proc/self/exe");
	if (child)
		return overlay_child();

	if (list)
	{
		for (const auto & t: registry())
			std::println("{}", t.name);
		return 0;
	}

	try
	{
		xr_loader();
	}
	catch (std::exception & e)
	{
		std::println(stderr, "{}", e.what());
		return 2;
	}

	std::vector<result> results;
	for (const auto & t: registry())
	{
		if (not filters.empty() and std::ranges::none_of(filters, [&](const auto & f) { return fnmatch(f.c_str(), t.name.c_str(), 0) == 0; }))
			continue;

		std::println(stderr, "[ RUN  ] {}", t.name);
		auto start = std::chrono::steady_clock::now();
		result r{t.name, "pass", "", 0};
		current_failures.clear();
		try
		{
			for (const auto & ext: t.extensions)
				if (not xr_loader().extensions.contains(ext))
					SKIP("{} is not advertised", ext);
			t.body();
		}
		catch (test_skipped & e)
		{
			r.status = "skip";
			r.message = e.what();
		}
		catch (std::exception & e)
		{
			current_failures.push_back(e.what());
		}
		if (not current_failures.empty())
		{
			r.status = "fail";
			for (const auto & f: current_failures)
				r.message += f + "\n";
		}
		r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		std::println(stderr, "[ {} ] {} ({:.1f} s){}{}", r.status == "pass" ? "PASS" : r.status == "skip" ? "SKIP"
		                                                                                                  : "FAIL",
		             t.name,
		             r.seconds,
		             r.message.empty() ? "" : "\n",
		             r.message);
		results.push_back(r);
	}

	int failures = std::ranges::count(results, std::string("fail"), &result::status);
	int skipped = std::ranges::count(results, std::string("skip"), &result::status);
	if (not junit.empty())
	{
		std::ofstream out(junit);
		out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites>\n";
		out << std::format("  <testsuite name=\"wivrn-xr-tests\" tests=\"{}\" failures=\"{}\" skipped=\"{}\" errors=\"0\">\n", results.size(), failures, skipped);
		for (const auto & r: results)
		{
			out << std::format("    <testcase classname=\"global\" name=\"{}\" time=\"{:.3f}\"", xml_escape(r.name), r.seconds);
			if (r.status == "pass")
				out << "/>\n";
			else if (r.status == "skip")
				out << std::format(">\n      <skipped message=\"{}\"/>\n    </testcase>\n", xml_escape(r.message));
			else
				out << std::format(">\n      <failure message=\"{}\">{}</failure>\n    </testcase>\n",
				                   xml_escape(r.message.substr(0, r.message.find('\n'))),
				                   xml_escape(r.message));
		}
		out << "  </testsuite>\n</testsuites>\n";
	}
	std::println(stderr, "{} tests, {} failed, {} skipped", results.size(), failures, skipped);
	return failures ? 1 : 0;
}
