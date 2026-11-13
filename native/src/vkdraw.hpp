// SPDX-License-Identifier: GPL-3.0-or-later
//
// Draws on the PC's GPU, through Vulkan (vkdraw.cpp): the game's vertex and pixel shaders,
// translated (translate.cpp), into a render target uploaded from the game's memory and read
// back into it (the default, where there is Vulkan), or into a copy, to compare with what the
// CPU drew (--gpu-check). If the vertex shader isn't translated, the vertices are the CPU's,
// passed on by a trivial one.

#pragma once

#include <string>
#include <vector>

#include "loader.hpp"

namespace mensis::vkdraw {

struct Texture {
    u32 type = 9;  // the T#'s: 8 1D, 9 2D, 10 3D, 12 1D array, 13 2D array
    u32 width = 1, height = 1, layers = 1;  // layers: an array's slices, or a 3D image's depth
    u32 sampler[4] = {};  // the S#, if it is sampled
    // Kept on the GPU between draws under this key (the T# and a hash of the memory it reads),
    // if it isn't empty; then the texels are only needed if has_texture(key) is false
    std::string key;
    // The texels: as 32-bit floats, each as the shader reads it, slice by slice, row by row;
    // or, if format (a VkFormat) isn't 0, as they are in memory, `pitch` texels to a row, with
    // the T#'s dst_sel as swizzle
    std::vector<float> rgba;
    u32 format = 0, pitch = 0, swizzle[4] = {4, 5, 6, 7};
    const unsigned char* raw = nullptr;
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
    // The render target, 4 bytes a pixel in rows: read from `target`, the result written to
    // `result` (the same, to draw into the game's memory)
    const unsigned char* target = nullptr;
    unsigned char* result = nullptr;
    u32 blend = 0, mask = 15;  // CB_BLEND0_CONTROL, and which of R, G, B, A are written
    float blend_color[4] = {};
    int scissor[4] = {};  // x0, y0, x1, y1
};

bool has_texture(const std::string& key);
// Draws it; false, with the reason, if it couldn't
bool run(Draw& d, std::string& why);
std::string device();  // the GPU it runs on, and what the validation layer reported
std::string report();  // what is kept on the GPU, and how long the draws took there

}  // namespace mensis::vkdraw
