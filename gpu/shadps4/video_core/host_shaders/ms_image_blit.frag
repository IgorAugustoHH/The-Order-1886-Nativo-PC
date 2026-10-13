// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

#if defined(SRC_MSAA)
layout (binding = 0, set = 0) uniform texture2DMS in_tex;
#else
layout (binding = 0, set = 0) uniform texture2D in_tex;
#endif

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 out_color;

void main()
{
#if defined(PACK_DEPTH_SAMPLES)
    // Keep every depth sample in its corresponding color channel. Averaging
    // here would destroy the data consumed by the guest's custom MSAA shaders.
    out_color = vec4(0.0);
    int samples = min(textureSamples(in_tex), 4);
    for (int sample_index = 0; sample_index < samples; ++sample_index) {
        out_color[sample_index] = texelFetch(in_tex, ivec2(gl_FragCoord.xy), sample_index).r;
    }
#elif defined(SRC_MSAA)
    out_color = texelFetch(in_tex, ivec2(gl_FragCoord.xy), gl_SampleID);
#else
    out_color = texelFetch(in_tex, ivec2(gl_FragCoord.xy), 0);
#endif
}
