// SPDX-License-Identifier: GPL-3.0-or-later
//
// Draws on the PC's GPU, through Vulkan (vkdraw.cpp): the game's vertex and pixel shaders,
// translated (translate.cpp), into render targets (up to 8 at once) in the game's memory,
// each kept on the GPU from draw to draw and brought back into the memory when something
// touches it (the default, where there is Vulkan); or into copies, to compare with what the
// CPU drew (--gpu-check). If the vertex shader isn't translated, the vertices are the CPU's, passed on
// by a trivial one.

#pragma once

#include <string>
#include <vector>

#include "loader.hpp"

namespace mensis::vkdraw {

struct Texture {
    u32 type = 9;  // the T#'s: 8 1D, 9 2D, 10 3D, 11 a cube (as a 2D array of its faces), 12 1D array, 13 2D array
    u32 width = 1, height = 1, layers = 1;  // layers: an array's slices, or a 3D image's depth
    u32 sampler[4] = {};  // the S#, if it is sampled
    // Kept on the GPU between draws under this key (the T# and a hash of the memory it reads),
    // if it isn't empty; then the texels are only needed if has_texture(key) is false
    std::string key;
    // The texels: as 32-bit floats, each as the shader reads it, slice by slice, row by row;
    // or, if format (a VkFormat) isn't 0, as they are in memory, `pitch` texels to a row, `bpp`
    // bytes each, with the T#'s dst_sel as swizzle
    std::vector<float> rgba;
    u32 format = 0, pitch = 0, bpp = 4, swizzle[4] = {4, 5, 6, 7};
    bool integer = false;  // rgba holds integers' bits: a texture of 32-bit uints, read nearest
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
    std::vector<std::vector<u32>> ps_buffers;  // the pixel shader's buffers, as the vertex shader's
    std::vector<Texture> textures;
    // The render targets, by their place (0-7), all the same size, `bpp` bytes a pixel in
    // rows: each the game's memory (`address`), kept on the GPU; or, if address is 0, read
    // from `target` and the result written to `result`
    struct Target {
        u32 format = 0;  // a VkFormat; 0: nothing at this place
        u32 bpp = 4;
        u64 address = 0;
        const unsigned char* target = nullptr;
        unsigned char* result = nullptr;
        u32 blend = 0, mask = 15;  // CB_BLENDn_CONTROL, and which of R, G, B, A are written
    };
    std::vector<Target> targets;
    u32 width = 0, height = 0;
    u32 rows = 0;  // the kept targets' rows in memory: past their height, to a multiple of 8 (0: their height)
    float blend_color[4] = {};
    int scissor[4] = {};  // x0, y0, x1, y1
};

// A compute dispatch: its GLSL, its U (the store places in it filled in here), the groups,
// and each buffer store's bytes in memory, which must be in a render target kept on the GPU
struct Dispatch {
    std::string cs;
    std::vector<u32> values;
    u32 store_slot = 0;
    u32 groups[3] = {1, 1, 1};
    struct Store {
        u64 address = 0, bytes = 0;
        u32 records = 0;  // the buffer's elements
    };
    std::vector<Store> stores;
};
bool dispatch(Dispatch& d, std::string& why);

bool has_texture(const std::string& key);
// Whether a pixel shader can have its triangle's corners' attributes and its barycentrics
// (VK_KHR_fragment_shader_barycentric), to interpolate as the console does
bool barycentric();
// A fault at addr (a write, or a read): if it is in a render target kept on the GPU, the
// picture goes back into the memory, or the memory is let be written, and true: the access
// can be tried again
bool touched(u64 addr, bool write);
// Draws it; false, with the reason, if it couldn't
bool run(Draw& d, std::string& why);
std::string device();  // the GPU it runs on, and what the validation layer reported
std::string report();  // what is kept on the GPU, and how long the draws took there

}  // namespace mensis::vkdraw
