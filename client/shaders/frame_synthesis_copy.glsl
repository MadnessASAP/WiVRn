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

// Copy of the received frame synthesis images (motion vectors and depth,
// colour formats) into the runtime's motion vector (colour) and depth
// (depth format) swapchain images, resampled to their size.
#version 450

#ifdef VERT_SHADER

layout(location = 0) out vec2 outUV;

void main()
{
	// Full screen triangle
	vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	outUV = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
#endif

#ifdef FRAG_SHADER

layout(set = 0, binding = 0) uniform sampler2D motion;
layout(set = 0, binding = 1) uniform sampler2D depth;

layout(location = 0) in vec2 inUV;

layout(location = 0) out vec4 outMotion;

void main()
{
	// Raw values: the scale and offset are passed to the runtime.
	// 2D motion vectors are sampled from a two component image, z is 0.
	outMotion = vec4(texture(motion, inUV).xyz, 0.0);
	gl_FragDepth = texture(depth, inUV).r;
}
#endif
