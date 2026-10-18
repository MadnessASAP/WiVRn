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

// Android system tracing (ATrace) for the headset client. The events appear in
// Perfetto captures of the headset that enable the atrace data source with the
// client package in atrace_apps (see docs/profiling.md). Cheap when no capture
// is running, no-op on other platforms.

#pragma once

#include <cstdint>

#ifdef __ANDROID__
#include <android/trace.h>
#endif

namespace wivrn::atrace
{
// Synchronous section on the calling thread, ends at scope exit.
// name must be a null terminated string, it is copied.
class section
{
public:
	explicit section([[maybe_unused]] const char * name)
	{
#ifdef __ANDROID__
		ATrace_beginSection(name);
#endif
	}
	~section()
	{
#ifdef __ANDROID__
		ATrace_endSection();
#endif
	}
	section(const section &) = delete;
	section & operator=(const section &) = delete;
};

// Section that may begin and end on different threads, matched by name and cookie
inline void async_begin([[maybe_unused]] const char * name, [[maybe_unused]] int32_t cookie)
{
#ifdef __ANDROID__
	ATrace_beginAsyncSection(name, cookie);
#endif
}

inline void async_end([[maybe_unused]] const char * name, [[maybe_unused]] int32_t cookie)
{
#ifdef __ANDROID__
	ATrace_endAsyncSection(name, cookie);
#endif
}

inline void counter([[maybe_unused]] const char * name, [[maybe_unused]] int64_t value)
{
#ifdef __ANDROID__
	ATrace_setCounter(name, value);
#endif
}

inline bool enabled()
{
#ifdef __ANDROID__
	return ATrace_isEnabled();
#else
	return false;
#endif
}
} // namespace wivrn::atrace
