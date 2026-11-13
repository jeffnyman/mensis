// SPDX-License-Identifier: GPL-3.0-or-later
//
// One draw on the PC's GPU, through Vulkan (vkdraw.cpp), for --gpu-check: the game's vertex
// and pixel shaders, translated (translate.cpp), into a copy of the render target, read back
// to compare with what the CPU drew. If the vertex shader isn't translated, the vertices are
// the CPU's, passed on by a trivial one.

#pragma once

#include <string>
#include <vector>

#include "loader.hpp"

namespace mensis::vkdraw {

struct Texture {
    u32 type = 9;  // the T#'s: 8 1D, 9 2D, 10 3D, 12 1D array, 13 2D array
    u32 width = 1, height = 1, layers = 1;  // layers: an array's slices, or a 3D image's depth
    std::vector<float> rgba;  // each texel as the shader reads it, slice by slice, row by row
    u32 sampler[4] = {};  // the S#, if it is sampled
};

struct Draw {
    std::string vs, ps;  // GLSL
    u32 attributes = 0;
    std::vector<float> vertices;  // the CPU's: each vertex's position (clip space), then attributes
    // Or the vertex shader on the GPU too (indices not empty): each triangle's three vertices'
    // indices (a rectangle's three corners, with the geometry shader gs adding the fourth),
    // the vertex shader's values and buffers (a uvec4 an element), the console's viewport as
    // the vertex shader applies it (x' = a x + b w for x, y, z), and culling
    std::vector<u32> indices;
    std::string gs;
    std::vector<u32> vs_values;
    std::vector<std::vector<u32>> vs_buffers;
    float viewport[8] = {};
    u32 cull = 0;  // PA_SU_SC_MODE_CNTL: bit 0 the front faces, bit 1 the back, bit 2 front is clockwise
    std::vector<u32> values;  // the shader's U: user SGPRs and what its scalar loads read
    std::vector<Texture> textures;
    u32 width = 0, height = 0;
    u32 format = 0;  // a VkFormat
    std::vector<unsigned char> target;  // 4 bytes a pixel, in rows: before, and after
    std::vector<unsigned char> before;  // a copy of it before (not used here: for comparing)
    u32 blend = 0, mask = 15;  // CB_BLEND0_CONTROL, and which of R, G, B, A are written
    float blend_color[4] = {};
    int scissor[4] = {};  // x0, y0, x1, y1
};

// Draws it; false, with the reason, if it couldn't
bool run(Draw& d, std::string& why);
std::string device();  // the GPU it runs on, and what the validation layer reported

}  // namespace mensis::vkdraw
