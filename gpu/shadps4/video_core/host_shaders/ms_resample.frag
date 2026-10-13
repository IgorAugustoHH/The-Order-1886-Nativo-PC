// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (The Order: 1886): nearest resample of a multisampled scene target between the native
// and the reduced size, sample by sample (blits do not take multisampled images). Run with
// sample shading at the same sample count as the source. DEPTH writes gl_FragDepth, else color.

#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

layout (binding = 0, set = 0) uniform texture2DMS src;

layout (location = 0) in vec2 uv;
#ifndef DEPTH
layout (location = 0) out vec4 color;
#endif

void main() {
    const ivec2 size = textureSize(src);
    const ivec2 coord = min(ivec2(uv * vec2(size)), size - 1);
    const vec4 value = texelFetch(src, coord, gl_SampleID);
#ifdef DEPTH
    gl_FragDepth = value.r;
#else
    color = value;
#endif
}
