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

// A headless stand-in for the headset, used to test the server without a device.
//
// It speaks the real protocol (unencrypted, so the server must run with --no-encrypt),
// reports a configurable set of headset features, answers time synchronisation, sends
// synthetic tracking at the display rate, acknowledges video frames with feedback packets
// and discards the video. The server only creates its OpenXR system once a headset is
// connected, so this is what lets OpenXR applications (e.g. the OpenXR CTS) run against
// the server in CI.

#include "crypto.h"
#include "frame_synthesis/annexb_sei.h"
#include "frame_synthesis/aux_frame.h"
#include "protocol_version.h"
#include "utils/overloaded.h"
#include "wivrn_packets.h"
#include "wivrn_sockets.h"

#include <CLI/CLI.hpp>
#include <arpa/inet.h>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstring>
#include <ctime>
#include <cxxabi.h>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <print>
#include <set>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

using namespace wivrn;
using namespace std::chrono_literals;

namespace
{
using control_socket = typed_socket<TCP, to_headset::packets, from_headset::packets>;
using stream_socket = typed_socket<UDP, to_headset::packets, from_headset::packets>;

std::atomic<bool> quit = false;

XrTime now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return XrTime(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

void log(std::string_view msg)
{
	std::println(stderr, "[virtual-headset {:.3f}] {}", now() / 1e9, msg);
}

struct options
{
	std::string host = "127.0.0.1";
	int port = 9757;
	bool tcp_only = false;
	std::string name = "WiVRn virtual headset";
	uint16_t width = 1024;
	uint16_t height = 1024;
	float stream_scale = 0.5;
	std::vector<float> refresh_rates{90};
	std::vector<std::string> codecs{"h264", "h265", "av1", "raw"};
	bool hand_tracking = true;
	bool eye_gaze = true;
	bool palm_pose = true;
	bool user_presence = true;
	bool passthrough = false;
	std::string face = "fb2";
	std::string body = "none";
	uint32_t generic_trackers = 0;
	std::string controllers = "touch";
	bool frame_synthesis = false;
	std::vector<uint16_t> motion_vector_size{0, 0};
	bool motion = false;
	double duration = 0;
	std::string ready_file;
	std::string stats_file;
	double stats_interval = 0;
};

struct statistics
{
	uint64_t video_shards = 0;
	uint64_t video_bytes = 0;
	std::set<uint64_t> frames;
	uint64_t feedback_sent = 0;
	uint64_t tracking_sent = 0;
	uint64_t hand_tracking_sent = 0;
	uint64_t body_tracking_sent = 0;
	uint64_t timesync_answered = 0;
	uint64_t tracking_control_received = 0;
	uint64_t haptics_received = 0;
	uint64_t refresh_rate_changes = 0;
	// XR_EXT_frame_synthesis auxiliary data (with --frame-synthesis)
	uint64_t aux_frames = 0; // parsed successfully
	uint64_t aux_errors = 0; // incomplete or invalid
	uint64_t aux_bytes = 0;
	std::map<std::string, uint64_t> packets;
	std::optional<to_headset::video_stream_description> video;
	std::string exit_reason;

	nlohmann::json to_json() const
	{
		nlohmann::json j{
		        {"video_shards", video_shards},
		        {"video_bytes", video_bytes},
		        {"video_frames", frames.size()},
		        {"feedback_sent", feedback_sent},
		        {"tracking_sent", tracking_sent},
		        {"hand_tracking_sent", hand_tracking_sent},
		        {"body_tracking_sent", body_tracking_sent},
		        {"timesync_answered", timesync_answered},
		        {"tracking_control_received", tracking_control_received},
		        {"haptics_received", haptics_received},
		        {"refresh_rate_changes", refresh_rate_changes},
		        {"frame_synthesis", {{"frames", aux_frames}, {"errors", aux_errors}, {"bytes", aux_bytes}}},
		        {"packets", packets},
		        {"exit_reason", exit_reason},
		};
		if (video)
			j["video"] = {
			        {"width", video->width},
			        {"height", video->height},
			        {"codec", {int(video->codec[0]), int(video->codec[1]), int(video->codec[2])}},
			        {"frame_rate", video->frame_rate},
			        {"refresh_rate", video->refresh_rate},
			};
		return j;
	}
};

sockaddr_in6 resolve(const std::string & host, int port)
{
	sockaddr_in6 sa{};
	sa.sin6_family = AF_INET6;
	sa.sin6_port = htons(port);
	if (inet_pton(AF_INET6, host.c_str(), &sa.sin6_addr) == 1)
		return sa;

	in_addr v4;
	if (inet_pton(AF_INET, host.c_str(), &v4) != 1)
		throw std::runtime_error("invalid address " + host + " (use a numeric IPv4 or IPv6 address)");
	// IPv4-mapped IPv6 address
	sa.sin6_addr.s6_addr[10] = 0xff;
	sa.sin6_addr.s6_addr[11] = 0xff;
	memcpy(&sa.sin6_addr.s6_addr[12], &v4, 4);
	return sa;
}

video_codec parse_codec(const std::string & name)
{
	if (name == "h264")
		return h264;
	if (name == "h265" or name == "hevc")
		return h265;
	if (name == "av1")
		return av1;
	if (name == "raw")
		return raw;
	throw std::runtime_error("unknown codec " + name);
}

from_headset::face_type parse_face(const std::string & name)
{
	if (name == "none")
		return from_headset::face_type::none;
	if (name == "android")
		return from_headset::face_type::android;
	if (name == "fb2")
		return from_headset::face_type::fb2;
	if (name == "htc")
		return from_headset::face_type::htc;
	throw std::runtime_error("unknown face tracker " + name);
}

from_headset::body_type parse_body(const std::string & name)
{
	if (name == "none")
		return from_headset::body_type::none;
	if (name == "fb")
		return from_headset::body_type::fb;
	if (name == "meta")
		return from_headset::body_type::meta;
	if (name == "bd")
		return from_headset::body_type::bd;
	if (name == "htc")
		return from_headset::body_type::htc;
	throw std::runtime_error("unknown body tracker " + name);
}

interaction_profile parse_controllers(const std::string & name)
{
	if (name == "none")
		return interaction_profile::none;
	if (name == "simple")
		return interaction_profile::khr_simple_controller;
	if (name == "touch")
		return interaction_profile::oculus_touch_controller;
	if (name == "touch-plus")
		return interaction_profile::meta_touch_plus_controller;
	if (name == "touch-pro")
		return interaction_profile::meta_touch_pro_controller;
	if (name == "hand")
		return interaction_profile::ext_hand_interaction_ext;
	if (name == "index")
		return interaction_profile::valve_index_controller;
	throw std::runtime_error("unknown controller profile " + name);
}

XrQuaternionf yaw(float angle)
{
	return {0, std::sin(angle / 2), 0, std::cos(angle / 2)};
}

// Visibility masks of a view with a ±45° field of view, in tangent space.
from_headset::visibility_mask_changed visibility_mask(uint8_t view_index)
{
	from_headset::visibility_mask_changed res{.view_index = view_index};

	// XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR: one triangle in each corner
	auto & hidden = res.data[0];
	for (auto [x, y]: {std::pair{-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f}, {-1.f, 1.f}})
	{
		auto base = uint32_t(hidden.vertices.size());
		hidden.vertices.push_back({x, y});
		hidden.vertices.push_back({x * 0.8f, y});
		hidden.vertices.push_back({x, y * 0.8f});
		// counter-clockwise: (b - a) × (c - a) > 0
		if (x * y > 0)
			hidden.indices.insert(hidden.indices.end(), {base, base + 1, base + 2});
		else
			hidden.indices.insert(hidden.indices.end(), {base, base + 2, base + 1});
	}

	// XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR: the whole view
	auto & visible = res.data[1];
	visible.vertices = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
	visible.indices = {0, 1, 2, 0, 2, 3};

	// XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR
	auto & line_loop = res.data[2];
	line_loop.vertices = visible.vertices;
	line_loop.indices = {0, 1, 2, 3};

	return res;
}

class virtual_headset
{
	options opt;
	control_socket control;
	stream_socket stream{-1};
	statistics stats;

	float refresh_rate;
	interaction_profile controllers;
	std::vector<to_headset::tracking_control::sample> pattern;
	std::map<std::pair<uint64_t, uint8_t>, XrTime> display_times;
	XrTime start_time = now();

	template <typename T>
	void send_stream(T && packet)
	{
		if (stream)
			stream.send(std::forward<T>(packet));
		else
			control.send(std::forward<T>(packet));
	}

	to_headset::packets receive_control(std::chrono::seconds timeout)
	{
		auto deadline = std::chrono::steady_clock::now() + timeout;
		while (true)
		{
			if (auto packet = control.receive_pending())
				return std::move(*packet);

			pollfd fds{.fd = control.get_fd(), .events = POLLIN};
			int r = ::poll(&fds, 1, 100);
			if (r < 0 and errno != EINTR)
				throw std::system_error(errno, std::system_category());
			if (fds.revents & (POLLHUP | POLLERR))
				throw std::runtime_error("control socket closed during handshake");
			if (fds.revents & POLLIN)
				if (auto packet = control.receive())
					return std::move(*packet);
			if (std::chrono::steady_clock::now() > deadline)
				throw std::runtime_error("timeout during handshake");
			if (quit)
				throw std::runtime_error("interrupted");
		}
	}

	void handshake()
	{
		auto keypair = crypto::key::generate_x448_keypair();
		control.send(from_headset::crypto_handshake{
		        .protocol_version = protocol_version,
		        .public_key = keypair.public_key(),
		        .name = opt.name,
		});

		auto crypto = std::get<to_headset::crypto_handshake>(receive_control(10s));
		switch (crypto.state)
		{
			case to_headset::crypto_handshake::crypto_state::encryption_disabled:
				break;
			case to_headset::crypto_handshake::crypto_state::incompatible_version:
				throw std::runtime_error("incompatible protocol version");
			default:
				throw std::runtime_error("the server requires encryption, start it with --no-encrypt");
		}

		control.send(from_headset::crypto_handshake{});

		auto h = std::get<to_headset::handshake>(receive_control(10s));
		if (h.stream_port > 0 and not opt.tcp_only)
		{
			stream = stream_socket();
			stream.connect(resolve(opt.host, h.stream_port));
			stream.set_receive_buffer_size(1024 * 1024 * 5);
		}

		// The handshake on the stream socket may be lost, repeat it until the server answers
		auto deadline = std::chrono::steady_clock::now() + 10s;
		while (true)
		{
			send_stream(from_headset::handshake{});
			pollfd fds{.fd = control.get_fd(), .events = POLLIN};
			if (::poll(&fds, 1, 200) > 0)
			{
				auto packet = receive_control(10s);
				if (std::holds_alternative<to_headset::handshake>(packet))
					break;
			}
			if (std::chrono::steady_clock::now() > deadline)
				throw std::runtime_error("no stream handshake from the server");
		}
		log(std::format("connected, {} stream", stream ? "UDP" : "TCP only"));
	}

	from_headset::headset_info_packet headset_info()
	{
		from_headset::headset_info_packet info{
		        .render_eye_width = opt.width,
		        .render_eye_height = opt.height,
		        .stream_eye_width = uint16_t(opt.width * opt.stream_scale),
		        .stream_eye_height = uint16_t(opt.height * opt.stream_scale),
		        .available_refresh_rates = opt.refresh_rates,
		        .settings = {
		                .preferred_refresh_rate = opt.refresh_rates.front(),
		                .minimum_refresh_rate = 0,
		                .fps_divider = 1,
		                .bitrate_bps = 50'000'000,
		                .mirror_gamepad = false,
		                .enabled_body_parts = (uint32_t(from_headset::body_part_mask::max) - 1),
		                .frame_synthesis = opt.frame_synthesis,
		        },
		        .hand_tracking = opt.hand_tracking,
		        .eye_gaze = opt.eye_gaze,
		        .palm_pose = opt.palm_pose,
		        .user_presence = opt.user_presence,
		        .passthrough = opt.passthrough,
		        .face_tracking = parse_face(opt.face),
		        .body_tracking = parse_body(opt.body),
		        .num_generic_trackers = opt.generic_trackers,
		        .bit_depth = 8,
		        .frame_synthesis_format = opt.frame_synthesis ? wivrn::frame_synthesis::format_hash() : 0,
		        .frame_synthesis_motion_vector_width = opt.motion_vector_size[0],
		        .frame_synthesis_motion_vector_height = opt.motion_vector_size[1],
		        .system_name = opt.name,
		        .language = "en",
		        .country = "US",
		        .variant = "",
		};
		for (const auto & codec: opt.codecs)
			info.supported_codecs.push_back({.codec = parse_codec(codec), .supports_10bit = false});
		return info;
	}

	XrPosef head_pose(XrTime t)
	{
		XrPosef pose{.orientation = {0, 0, 0, 1}, .position = {0, 1.6, 0}};
		if (opt.motion)
		{
			double s = (t - start_time) / 1e9;
			pose.orientation = yaw(0.3 * std::sin(s));
			pose.position.x = 0.05 * std::sin(0.7 * s);
		}
		return pose;
	}

	static XrPosef offset(const XrPosef & base, XrVector3f d)
	{
		// only used for poses with a yaw rotation: rotate d around Y
		float angle = 2 * std::atan2(base.orientation.y, base.orientation.w);
		float c = std::cos(angle), s = std::sin(angle);
		return {
		        .orientation = base.orientation,
		        .position = {
		                base.position.x + c * d.x + s * d.z,
		                base.position.y + d.y,
		                base.position.z - s * d.x + c * d.z,
		        },
		};
	}

	from_headset::tracking::pose make_pose(device_id device, XrPosef pose)
	{
		return {
		        .pose = pose,
		        .linear_velocity = {},
		        .angular_velocity = {},
		        .device = device,
		        .flags = uint8_t(from_headset::orientation_valid | from_headset::position_valid |
		                         from_headset::orientation_tracked | from_headset::position_tracked |
		                         from_headset::linear_velocity_valid | from_headset::angular_velocity_valid),
		};
	}

	void add_device(from_headset::tracking & tracking, device_id device, XrTime t)
	{
		auto head = head_pose(t);
		switch (device)
		{
			case device_id::HEAD:
				tracking.device_poses.push_back(make_pose(device, head));
				break;
			case device_id::LEFT_GRIP:
			case device_id::LEFT_AIM:
			case device_id::LEFT_PALM:
			case device_id::LEFT_PINCH_POSE:
			case device_id::LEFT_POKE:
				if (controllers != interaction_profile::none)
					tracking.device_poses.push_back(make_pose(device, offset(head, {-0.2, -0.3, -0.35})));
				break;
			case device_id::RIGHT_GRIP:
			case device_id::RIGHT_AIM:
			case device_id::RIGHT_PALM:
			case device_id::RIGHT_PINCH_POSE:
			case device_id::RIGHT_POKE:
				if (controllers != interaction_profile::none)
					tracking.device_poses.push_back(make_pose(device, offset(head, {0.2, -0.3, -0.35})));
				break;
			case device_id::EYE_GAZE:
				// Relative to the head, like the headset: looking straight ahead
				if (opt.eye_gaze)
					tracking.device_poses.push_back(make_pose(device, {.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}}));
				break;
			case device_id::FACE:
				add_face(tracking, t);
				break;
			default:
				break;
		}
	}

	void add_face(from_headset::tracking & tracking, XrTime t)
	{
		switch (parse_face(opt.face))
		{
			case from_headset::face_type::none:
				break;
			case from_headset::face_type::android: {
				from_headset::tracking::android_face face{
				        .state = XR_FACE_TRACKING_STATE_TRACKING_ANDROID,
				        .sample_time = t,
				        .is_calibrated = true,
				        .is_valid = true,
				};
				face.parameters.fill(0.1);
				face.confidences.fill(1);
				tracking.face = face;
				break;
			}
			case from_headset::face_type::fb2: {
				from_headset::tracking::fb_face2 face{
				        .time = t,
				        .is_valid = true,
				        .is_eye_following_blendshapes_valid = true,
				};
				face.weights.fill(0.1);
				face.confidences.fill(1);
				tracking.face = face;
				break;
			}
			case from_headset::face_type::htc: {
				from_headset::tracking::htc_face face{
				        .eye_sample_time = t,
				        .lip_sample_time = t,
				        .eye_active = true,
				        .lip_active = true,
				};
				face.eye.fill(0.1);
				face.lip.fill(0.1);
				tracking.face = face;
				break;
			}
		}
	}

	void send_hand(from_headset::hand_tracking::hand_id hand, XrTime produced, XrTime t)
	{
		auto head = head_pose(t);
		float side = hand == from_headset::hand_tracking::left ? -1 : 1;
		std::array<from_headset::hand_tracking::pose, XR_HAND_JOINT_COUNT_EXT> joints;
		for (size_t i = 0; i < joints.size(); ++i)
		{
			// A flat hand pointing forward, joints spread along -Z
			auto pose = offset(head, {side * (0.2f + 0.01f * (i % 5)), -0.3f, -0.35f - 0.01f * (i / 5)});
			joints[i] = {
			        .position = pose.position,
			        .orientation = pack(pose.orientation),
			        .linear_velocity = {},
			        .angular_velocity = {},
			        .radius = 100, // 1 cm
			        .flags = uint8_t(from_headset::orientation_valid | from_headset::position_valid |
			                         from_headset::orientation_tracked | from_headset::position_tracked |
			                         from_headset::linear_velocity_valid | from_headset::angular_velocity_valid),
			};
		}
		send_stream(from_headset::hand_tracking{
		        .production_timestamp = produced,
		        .timestamp = t,
		        .hand = hand,
		        .joints = joints,
		});
		++stats.hand_tracking_sent;
	}

	static constexpr uint8_t all_valid = from_headset::orientation_valid | from_headset::position_valid |
	                                     from_headset::orientation_tracked | from_headset::position_tracked;

	// Body joints: the root 0.6 m below the head, joint i is i cm above the root
	template <size_t N>
	void fill_meta_joints(auto & joints, XrTime t)
	{
		auto root = offset(head_pose(t), {0, -0.6, 0});
		joints.root = {
		        .position = root.position,
		        .orientation = pack(root.orientation),
		        .flags = all_valid,
		};
		for (size_t i = 0; i < N - 1; ++i)
			joints.joints[i] = {
			        .position = {0, int16_t(100 * (i + 1)), 0},
			        .orientation = pack(XrQuaternionf{0, 0, 0, 1}),
			        .flags = all_valid,
			};
	}

	void send_body(XrTime produced, XrTime t)
	{
		switch (parse_body(opt.body))
		{
			case from_headset::body_type::none:
				return;
			case from_headset::body_type::fb: {
				from_headset::meta_body::fb_joints joints;
				fill_meta_joints<XR_BODY_JOINT_COUNT_FB>(joints, t);
				send_stream(from_headset::meta_body{.production_timestamp = produced, .timestamp = t, .confidence = 1, .joints = joints});
				break;
			}
			case from_headset::body_type::meta: {
				from_headset::meta_body::meta_joints joints;
				fill_meta_joints<XR_FULL_BODY_JOINT_COUNT_META>(joints, t);
				send_stream(from_headset::meta_body{.production_timestamp = produced, .timestamp = t, .confidence = 1, .joints = joints});
				break;
			}
			case from_headset::body_type::bd: {
				from_headset::bd_body body{.all_tracked = true, .production_timestamp = produced, .timestamp = t};
				auto root = offset(head_pose(t), {0, -0.6, 0});
				for (size_t i = 0; i < body.joints.size(); ++i)
					body.joints[i] = {
					        .position = {root.position.x, root.position.y + 0.01f * i, root.position.z},
					        .orientation = pack(root.orientation),
					        .flags = all_valid,
					};
				send_stream(std::move(body));
				break;
			}
			case from_headset::body_type::htc: {
				from_headset::htc_body body{.production_timestamp = produced, .timestamp = t};
				for (size_t i = 0; i < body.poses.size(); ++i)
					body.poses[i] = {
					        .pose = offset(head_pose(t), {0.1f * i, -1.0f, 0}),
					        .linear_velocity = {},
					        .angular_velocity = {},
					        .flags = uint8_t(i < opt.generic_trackers ? all_valid : 0),
					};
				send_stream(std::move(body));
				break;
			}
		}
		++stats.body_tracking_sent;
	}

	void send_body_skeleton()
	{
		auto make = [](auto & joints) {
			for (size_t i = 0; i < joints.size(); ++i)
				joints[i] = XrBodySkeletonJointFB{
				        .joint = int32_t(i),
				        .parentJoint = i == 0 ? -1 : int32_t(i - 1),
				        .pose = {.orientation = {0, 0, 0, 1}, .position = {0, 0.01f * i, 0}},
				};
		};
		switch (parse_body(opt.body))
		{
			case from_headset::body_type::fb: {
				from_headset::meta_body_skeleton::fb_skeleton skeleton;
				make(skeleton.joints);
				control.send(from_headset::meta_body_skeleton{.skeleton = skeleton});
				break;
			}
			case from_headset::body_type::meta: {
				from_headset::meta_body_skeleton::meta_skeleton skeleton;
				make(skeleton.joints);
				control.send(from_headset::meta_body_skeleton{.skeleton = skeleton});
				break;
			}
			default:
				break;
		}
	}

	void send_tracking()
	{
		XrTime t0 = now();

		// Group the requested samples by prediction, like the headset does
		std::map<XrDuration, std::vector<device_id>> samples;
		samples[0].push_back(device_id::HEAD);
		for (const auto & s: pattern)
			samples[s.prediction_ns].push_back(s.device);

		for (const auto & [prediction, devices]: samples)
		{
			XrTime t = t0 + prediction;
			auto head = head_pose(t);
			from_headset::tracking tracking{
			        .interaction_profiles = {controllers, controllers, interaction_profile::none},
			        .production_timestamp = t0,
			        .timestamp = t,
			        .view_flags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
			                      XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT,
			        .state_flags = 0,
			};
			// views relative to the view space
			for (int eye = 0; eye < 2; ++eye)
			{
				tracking.views[eye] = {
				        .pose = {.orientation = {0, 0, 0, 1}, .position = {eye == 0 ? -0.032f : 0.032f, 0, 0}},
				        .fov = {.angleLeft = -0.785398f, .angleRight = 0.785398f, .angleUp = 0.785398f, .angleDown = -0.785398f},
				};
			}

			std::set<device_id> done;
			for (auto device: devices)
			{
				if (device == device_id::BODY)
				{
					send_body(t0, t);
					continue;
				}
				if (device == device_id::LEFT_HAND or device == device_id::RIGHT_HAND)
				{
					if (opt.hand_tracking)
						send_hand(device == device_id::LEFT_HAND ? from_headset::hand_tracking::left : from_headset::hand_tracking::right, t0, t);
					continue;
				}
				if (done.insert(device).second)
					add_device(tracking, device, t);
			}
			// The head is always located when the views are
			if (not done.contains(device_id::HEAD))
				add_device(tracking, device_id::HEAD, t);
			(void)head;

			send_stream(std::move(tracking));
			++stats.tracking_sent;
		}

		if (parse_body(opt.body) == from_headset::body_type::htc)
			send_body(t0, t0);
	}

	void send_initial_state()
	{
		control.send(headset_info());
		for (uint8_t view = 0; view < 2; ++view)
			control.send(visibility_mask(view));
		send_body_skeleton();
		control.send(from_headset::battery{.charge = 0.8, .present = true, .charging = false});
		control.send(from_headset::session_state_changed{.state = XR_SESSION_STATE_FOCUSED});
		if (opt.user_presence)
			control.send(from_headset::user_presence_changed{.present = true, .change_time = now()});
	}

	// Shards of the frames being received, kept to extract the frame synthesis data
	std::map<std::pair<uint64_t, uint8_t>, std::map<uint16_t, std::vector<uint8_t>>> pending_shards;

	// Extract and check the auxiliary data of a complete frame, as the client does
	// (shard_accumulator): a shard starting with a start code begins a NAL unit.
	void on_frame_complete(std::pair<uint64_t, uint8_t> key)
	{
		auto node = pending_shards.extract(key);
		if (node.empty() or not stats.video or key.second >= stats.video->codec.size())
			return;
		const auto & shards = node.mapped();
		if (shards.rbegin()->first + 1 != shards.size())
			return; // lost shards
		auto codec = stats.video->codec[key.second];
		if (not wivrn::frame_synthesis::supports_sei(codec))
			return;

		wivrn::frame_synthesis::sei_reassembler aux(codec);
		bool in_aux = false;
		for (const auto & [index, payload]: shards)
		{
			if (wivrn::frame_synthesis::starts_with_start_code(payload))
				in_aux = wivrn::frame_synthesis::is_aux_sei_start(codec, payload);
			if (in_aux)
				aux.push(payload);
		}
		if (not aux.has_data())
			return;
		auto blob = aux.finish();
		try
		{
			if (not blob)
				throw std::runtime_error("incomplete");
			wivrn::frame_synthesis::parse(*blob);
			++stats.aux_frames;
			stats.aux_bytes += blob->size();
		}
		catch (std::exception & e)
		{
			if (stats.aux_errors++ == 0)
				log(std::format("invalid frame synthesis data in frame {}: {}", key.first, e.what()));
		}
	}

	void on_shard(to_headset::video_stream_data_shard && shard)
	{
		++stats.video_shards;
		stats.video_bytes += shard.payload.size();
		stats.frames.insert(shard.frame_idx);
		auto key = std::make_pair(shard.frame_idx, shard.stream_item_idx);
		if (shard.view_info)
			display_times[key] = shard.view_info->display_time;

		if (opt.frame_synthesis)
		{
			pending_shards[key][shard.shard_idx].assign(shard.payload.begin(), shard.payload.end());
			if (shard.timing_info)
				on_frame_complete(key);
			while (pending_shards.size() > 64)
				pending_shards.erase(pending_shards.begin());
		}

		if (shard.timing_info)
		{
			// Last shard of this frame: acknowledge it as decoded and displayed now
			XrTime t = now();
			XrTime displayed = t;
			if (auto it = display_times.find(key); it != display_times.end())
				displayed = std::max(t, it->second);

			control.send(from_headset::feedback{
			        .frame_index = shard.frame_idx,
			        .stream_index = shard.stream_item_idx,
			        .encode_begin = shard.timing_info->encode_begin,
			        .encode_end = shard.timing_info->encode_end,
			        .send_begin = shard.timing_info->send_begin,
			        .send_end = shard.timing_info->send_end,
			        .received_first_packet = t,
			        .received_last_packet = t,
			        .sent_to_decoder = t,
			        .received_from_decoder = t,
			        .blitted = t,
			        .displayed = displayed,
			        .times_displayed = 1,
			});
			++stats.feedback_sent;

			// Forget old frames
			while (display_times.size() > 64)
				display_times.erase(display_times.begin());
		}
	}

	template <typename T>
	void count()
	{
		int status = 0;
		char * demangled = abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, &status);
		std::string name = status == 0 ? demangled : typeid(T).name();
		free(demangled);
		if (auto pos = name.rfind("::"); pos != std::string::npos)
			name = name.substr(pos + 2);
		++stats.packets[name];
	}

	void handle(to_headset::packets && packet)
	{
		std::visit(
		        utils::overloaded{
		                [&](to_headset::video_stream_data_shard && shard) {
			                count<to_headset::video_stream_data_shard>();
			                on_shard(std::move(shard));
		                },
		                [&](to_headset::timesync_query && query) {
			                count<to_headset::timesync_query>();
			                send_stream(from_headset::timesync_response{.query = query.query, .response = now()});
			                ++stats.timesync_answered;
		                },
		                [&](to_headset::tracking_control && control) {
			                count<to_headset::tracking_control>();
			                pattern = std::move(control.pattern);
			                ++stats.tracking_control_received;
		                },
		                [&](to_headset::video_stream_description && desc) {
			                count<to_headset::video_stream_description>();
			                log(std::format("video stream {}x{} codec {} {} fps",
			                                desc.width,
			                                desc.height,
			                                int(desc.codec[0]),
			                                desc.frame_rate));
			                stats.video = desc;
		                },
		                [&](to_headset::refresh_rate_change && change) {
			                count<to_headset::refresh_rate_change>();
			                log(std::format("refresh rate change {} -> {}", refresh_rate, change.hz));
			                control.send(from_headset::refresh_rate_changed{.from = refresh_rate, .to = change.hz});
			                refresh_rate = change.hz;
			                ++stats.refresh_rate_changes;
		                },
		                [&](to_headset::haptics &&) {
			                count<to_headset::haptics>();
			                ++stats.haptics_received;
		                },
		                [&](to_headset::server_message && msg) {
			                count<to_headset::server_message>();
			                log("server message: " + msg.msg);
		                },
		                [&]<typename T>(T &&) { count<T>(); },
		        },
		        std::move(packet));
	}

	void drain(auto & socket)
	{
		while (auto packet = socket.receive_pending())
			handle(std::move(*packet));
	}

public:
	virtual_headset(options opt) :
	        opt(std::move(opt)),
	        control(resolve(this->opt.host, this->opt.port)),
	        refresh_rate(this->opt.refresh_rates.front()),
	        controllers(parse_controllers(this->opt.controllers))
	{
		handshake();
		send_initial_state();
		if (not this->opt.ready_file.empty())
			std::ofstream(this->opt.ready_file) << "connected\n";
	}

	void write_stats()
	{
		if (opt.stats_file.empty())
			return;
		// Write then rename, readers never see a partial file
		auto tmp = opt.stats_file + ".tmp";
		std::ofstream(tmp) << stats.to_json().dump(2) << "\n";
		std::rename(tmp.c_str(), opt.stats_file.c_str());
	}

	int run()
	{
		XrTime next_tracking = now();
		XrTime next_stats = opt.stats_interval > 0 ? now() : 0;
		XrTime end = opt.duration > 0 ? now() + XrDuration(opt.duration * 1e9) : 0;
		int ret = 0;
		try
		{
			while (not quit)
			{
				XrTime t = now();
				if (end and t >= end)
				{
					stats.exit_reason = "duration elapsed";
					break;
				}
				if (next_stats and t >= next_stats)
				{
					write_stats();
					next_stats = t + XrDuration(opt.stats_interval * 1e9);
				}
				if (t >= next_tracking)
				{
					send_tracking();
					XrDuration period = 1e9 / refresh_rate;
					next_tracking += period;
					if (next_tracking < t)
						next_tracking = t + period;
				}

				drain(control);
				if (stream)
					drain(stream);

				pollfd fds[2] = {
				        {.fd = control.get_fd(), .events = POLLIN},
				        {.fd = stream ? stream.get_fd() : -1, .events = POLLIN},
				};
				int timeout_ms = std::max<int>(0, (next_tracking - now()) / 1'000'000);
				int r = ::poll(fds, 2, timeout_ms);
				if (r < 0)
				{
					if (errno == EINTR)
						continue;
					throw std::system_error(errno, std::system_category());
				}
				if (fds[0].revents & (POLLHUP | POLLERR))
					throw socket_shutdown();
				if (fds[0].revents & POLLIN)
					if (auto packet = control.receive())
						handle(std::move(*packet));
				if (fds[1].revents & POLLIN)
					if (auto packet = stream.receive())
						handle(std::move(*packet));
			}
			if (quit and stats.exit_reason.empty())
				stats.exit_reason = "signal";
		}
		catch (socket_shutdown &)
		{
			stats.exit_reason = "server disconnected";
			log("server disconnected");
		}
		catch (std::exception & e)
		{
			stats.exit_reason = std::string("error: ") + e.what();
			log(stats.exit_reason);
			ret = 1;
		}

		log(std::format("{} video frames ({} shards, {} bytes), {} feedback, {} tracking, {} timesync",
		                stats.frames.size(),
		                stats.video_shards,
		                stats.video_bytes,
		                stats.feedback_sent,
		                stats.tracking_sent,
		                stats.timesync_answered));
		write_stats();
		return ret;
	}
};
} // namespace

int main(int argc, char * argv[])
{
	options opt;
	CLI::App app{"Headless WiVRn headset for testing the server (requires wivrn-server --no-encrypt)"};
	app.add_option("--host", opt.host, "server address (numeric)")->capture_default_str();
	app.add_option("--port", opt.port, "server port")->capture_default_str();
	app.add_flag("--tcp-only", opt.tcp_only, "do not use the UDP stream socket");
	app.add_option("--name", opt.name, "headset name")->capture_default_str();
	app.add_option("--width", opt.width, "recommended render width per eye")->capture_default_str();
	app.add_option("--height", opt.height, "recommended render height per eye")->capture_default_str();
	app.add_option("--stream-scale", opt.stream_scale, "stream size relative to the render size")->capture_default_str();
	app.add_option("--refresh-rate", opt.refresh_rates, "available refresh rates, the first one is used")->capture_default_str();
	app.add_option("--codec", opt.codecs, "supported codecs (h264, h265, av1, raw)")->capture_default_str();
	app.add_option("--hand-tracking", opt.hand_tracking, "report hand tracking")->capture_default_str();
	app.add_option("--eye-gaze", opt.eye_gaze, "report eye gaze")->capture_default_str();
	app.add_option("--palm-pose", opt.palm_pose, "report palm pose")->capture_default_str();
	app.add_option("--user-presence", opt.user_presence, "report user presence")->capture_default_str();
	app.add_option("--passthrough", opt.passthrough, "report passthrough")->capture_default_str();
	app.add_option("--face", opt.face, "face tracking: none, android, fb2, htc")->capture_default_str();
	app.add_option("--body", opt.body, "body tracking: none, fb, meta, bd, htc")->capture_default_str();
	app.add_option("--generic-trackers", opt.generic_trackers, "number of generic trackers (htc body tracking)")->capture_default_str();
	app.add_option("--frame-synthesis", opt.frame_synthesis, "receive the XR_EXT_frame_synthesis data of the application")->capture_default_str();
	app.add_option("--motion-vector-size", opt.motion_vector_size, "recommended XR_EXT_frame_synthesis motion vector size (0 0: the colour size)")->expected(2)->capture_default_str();
	app.add_option("--controllers", opt.controllers, "controller profile: none, simple, touch, touch-plus, touch-pro, hand, index")->capture_default_str();
	app.add_flag("--motion", opt.motion, "move the head slowly instead of keeping it still");
	app.add_option("--duration", opt.duration, "exit after this many seconds (0: until the server disconnects)")->capture_default_str();
	app.add_option("--ready-file", opt.ready_file, "file written once connected");
	app.add_option("--stats-file", opt.stats_file, "JSON statistics written on exit");
	app.add_option("--stats-interval", opt.stats_interval, "also write the statistics every this many seconds");

	CLI11_PARSE(app, argc, argv);

	std::signal(SIGINT, [](int) { quit = true; });
	std::signal(SIGTERM, [](int) { quit = true; });

	// Retry while the server starts
	for (int attempt = 0;; ++attempt)
	{
		try
		{
			virtual_headset headset(opt);
			return headset.run();
		}
		catch (std::system_error & e)
		{
			if (e.code().value() != ECONNREFUSED or attempt >= 50 or quit)
			{
				log(std::string("connection failed: ") + e.what());
				return 2;
			}
			std::this_thread::sleep_for(200ms);
		}
		catch (std::exception & e)
		{
			log(std::string("handshake failed: ") + e.what());
			return 2;
		}
	}
}
