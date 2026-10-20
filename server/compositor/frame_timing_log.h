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

// Per-frame timing for measurements: when WIVRN_FRAME_TIMING_LOG names a file,
// one CSV line is written for each frame the headset presents for the first
// time, with the times of each step from the head pose sample on the headset to
// the display of the first headset frame showing it (see the header line), all
// converted to the headset clock.

#pragma once

#include "driver/clock_offset.h"
#include "wivrn_packets.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <mutex>

namespace wivrn
{

class frame_timing_log
{
	// Server clock unless noted
	struct tracking_sample
	{
		XrTime production = 0; // headset clock
		XrTime target = 0;     // headset clock, the time the pose was predicted for
		int64_t received = 0;
	};

	struct view_poses
	{
		int64_t at_timestamp = 0;
		int64_t production = 0;
		int64_t located = 0;
	};

	struct app_frame
	{
		int64_t display_time = 0;
		int64_t wake_up = 0;
		int64_t delivered = 0;
		int64_t gpu_done = 0;
	};

	struct entry
	{
		uint64_t frame_index = -1;
		// headset clock
		XrTime display_time = 0;
		XrTime pose_sample = 0;
		XrTime pose_target = 0;
		XrTime pose_received = 0;
		XrTime views_located = 0;
		XrTime app_display_time = 0;
		XrTime app_wake_up = 0;
		XrTime app_delivered = 0;
		XrTime app_gpu_done = 0;
		XrTime compositor_begin = 0;
	};

	std::ofstream out;
	std::mutex mutex;
	// About 2.8 s of frames at 90 Hz, feedback for older frames is not logged
	std::array<entry, 256> frames;
	std::array<tracking_sample, 256> samples;
	size_t next_sample = 0;
	// Recent view pose requests, by requested time
	std::array<view_poses, 64> poses;
	size_t next_pose = 0;
	std::array<app_frame, 64> app_frames;
	size_t next_app_frame = 0;

public:
	frame_timing_log();

	bool enabled() const
	{
		return out.is_open();
	}

	// Session: a tracking packet with a head pose arrived
	void on_tracking(const from_headset::tracking &);
	// HMD: views were located for at_timestamp from a head pose sampled at
	// production_timestamp (both server clock)
	void on_view_poses(int64_t at_timestamp, int64_t production_timestamp);
	// Application pacer: the application frame for display_time woke up,
	// was delivered (xrEndFrame) and finished on the GPU (server clock)
	void on_app_frame(int64_t display_time, int64_t wake_up, int64_t delivered, int64_t gpu_done);
	// Compositor: frame_index is about to be encoded for display_time (headset
	// clock), with the views located for pose_time (server clock)
	void on_frame(uint64_t frame_index, XrTime display_time, int64_t pose_time, const clock_offset &);
	void on_feedback(const from_headset::feedback &);
};

} // namespace wivrn
