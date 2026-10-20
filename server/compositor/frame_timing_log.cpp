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

#include "frame_timing_log.h"

#include "os/os_time.h"
#include "util/u_logging.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>

namespace wivrn
{

frame_timing_log::frame_timing_log()
{
	const char * path = std::getenv("WIVRN_FRAME_TIMING_LOG");
	if (not path or not *path)
		return;
	out.open(path, std::ios::out | std::ios::trunc);
	if (not out)
	{
		U_LOG_W("cannot open WIVRN_FRAME_TIMING_LOG %s", path);
		return;
	}
	U_LOG_I("writing frame timing to %s", path);
	// Times in ns: received_unix_ns = host wall clock when the feedback arrived
	// (to select a time window), the others = headset clock, 0 when unknown.
	// pose_sample: the headset sampled the newest head pose the application's
	// views were derived from, predicted for pose_target; pose_received: it
	// arrived on the server; app_wake_up: xrWaitFrame returned to the
	// application; views_located: it located its views for app_display_time;
	// app_delivered: xrEndFrame; app_gpu_done: its rendering finished;
	// compositor_begin: the compositor started the frame
	// to encode for display_time; then the video_encoder and client feedback
	// times; displayed: predicted display time of the first headset frame
	// showing it.
	out << "received_unix_ns,frame,stream,display_time,displayed,pose_sample,"
	       "pose_target,pose_received,views_located,app_display_time,app_wake_up,app_delivered,app_gpu_done,compositor_begin,"
	       "encode_begin,encode_end,send_begin,send_end,received_first_packet,received_last_packet,"
	       "sent_to_decoder,received_from_decoder,blitted\n";
}

void frame_timing_log::on_tracking(const from_headset::tracking & tracking)
{
	if (not out.is_open() or not std::ranges::contains(tracking.device_poses, device_id::HEAD, &from_headset::tracking::pose::device))
		return;
	auto now = os_monotonic_get_ns();
	std::lock_guard lock(mutex);
	samples[next_sample++ % samples.size()] = {tracking.production_timestamp, tracking.timestamp, now};
}

void frame_timing_log::on_view_poses(int64_t at_timestamp, int64_t production_timestamp)
{
	if (not out.is_open())
		return;
	auto now = os_monotonic_get_ns();
	std::lock_guard lock(mutex);
	poses[next_pose++ % poses.size()] = {at_timestamp, production_timestamp, now};
}

void frame_timing_log::on_app_frame(int64_t display_time, int64_t wake_up, int64_t delivered, int64_t gpu_done)
{
	if (not out.is_open())
		return;
	std::lock_guard lock(mutex);
	app_frames[next_app_frame++ % app_frames.size()] = {display_time, wake_up, delivered, gpu_done};
}

void frame_timing_log::on_frame(uint64_t frame_index, XrTime display_time, int64_t pose_time, const clock_offset & offset)
{
	if (not out.is_open())
		return;
	auto now = os_monotonic_get_ns();
	std::lock_guard lock(mutex);
	entry e{.frame_index = frame_index, .display_time = display_time};
	if (offset)
	{
		e.compositor_begin = offset.to_headset(now);
		e.app_display_time = offset.to_headset(pose_time);
		for (size_t i = 1; i <= app_frames.size() and i <= next_app_frame; ++i)
		{
			const auto & a = app_frames[(next_app_frame - i) % app_frames.size()];
			if (a.display_time != pose_time)
				continue;
			e.app_wake_up = a.wake_up ? offset.to_headset(a.wake_up) : 0;
			e.app_delivered = a.delivered ? offset.to_headset(a.delivered) : 0;
			e.app_gpu_done = offset.to_headset(a.gpu_done);
			break;
		}
		// The latest request for that time: the views the application rendered with
		for (size_t i = 1; i <= poses.size() and i <= next_pose; ++i)
		{
			const auto & p = poses[(next_pose - i) % poses.size()];
			if (p.at_timestamp != pose_time)
				continue;
			e.pose_sample = offset.to_headset(p.production);
			e.views_located = offset.to_headset(p.located);
			// The tracking packet of that sample; its production time went
			// through two clock conversions, take the nearest one
			const tracking_sample * best = nullptr;
			for (size_t j = 1; j <= samples.size() and j <= next_sample; ++j)
			{
				const auto & s = samples[(next_sample - j) % samples.size()];
				if (not best or std::abs(s.production - e.pose_sample) < std::abs(best->production - e.pose_sample))
					best = &s;
			}
			if (best and std::abs(best->production - e.pose_sample) < 1'000'000)
			{
				e.pose_sample = best->production;
				e.pose_target = best->target;
				e.pose_received = offset.to_headset(best->received);
			}
			break;
		}
	}
	frames[frame_index % frames.size()] = e;
}

void frame_timing_log::on_feedback(const from_headset::feedback & feedback)
{
	// The client sends feedback each time it shows a frame, keep the first one
	if (not out.is_open() or feedback.times_displayed != 1 or not feedback.displayed)
		return;
	std::lock_guard lock(mutex);
	const auto & e = frames[feedback.frame_index % frames.size()];
	if (e.frame_index != feedback.frame_index)
		return;
	const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	out << std::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
	                   now,
	                   feedback.frame_index,
	                   int(feedback.stream_index),
	                   e.display_time,
	                   feedback.displayed,
	                   e.pose_sample,
	                   e.pose_target,
	                   e.pose_received,
	                   e.views_located,
	                   e.app_display_time,
	                   e.app_wake_up,
	                   e.app_delivered,
	                   e.app_gpu_done,
	                   e.compositor_begin,
	                   feedback.encode_begin,
	                   feedback.encode_end,
	                   feedback.send_begin,
	                   feedback.send_end,
	                   feedback.received_first_packet,
	                   feedback.received_last_packet,
	                   feedback.sent_to_decoder,
	                   feedback.received_from_decoder,
	                   feedback.blitted);
}

} // namespace wivrn
