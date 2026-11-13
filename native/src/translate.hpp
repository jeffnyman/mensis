// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's shaders translated for a PC's GPU (translate.cpp): GCN instructions into GLSL,
// which the Vulkan SDK's compiler (shaderc) turns into SPIR-V. So far pixel, vertex and
// compute shaders, and only those without branches; what can't be translated says why, and
// is left to the CPU (shade.cpp).

#pragma once

#include <string>
#include <vector>

#include "loader.hpp"
#include "shade.hpp"

namespace mensis::translate {

// A shader's instructions, decoded once
struct Inst {
    u32 enc = 0;  // the format: see translate.cpp
    u32 w0 = 0, w1 = 0, lit = 0;
    u64 at = 0;  // its address
};
struct Program {
    std::vector<Inst> code;
    std::string why;  // why it can't be translated; empty if it can
};
// A vertex shader's call to its fetch shader is followed: the fetch shader's address is in
// its user SGPRs, which are needed for that
Program decode(u64 pc, const shade::Memory& mem, const u32* user = nullptr, u32 user_count = 0);

// What a draw hands the shader: its user SGPRs, and what its scalar loads read, found by
// following its scalar instructions with the draw's values. The descriptors of each image
// instruction, in order: an image's (T#) and, for sampling, a sampler's (S#).
struct Values {
    std::vector<u32> u;  // s0-s15 as the draw sets them, then each scalar load's dwords in turn
    struct Image {
        u32 t[8] = {};
        u32 s[4] = {};
    };
    std::vector<Image> images;
    struct Buffer {  // each buffer load's: the V#, the bytes into an element, the format
        u32 v[4] = {};
        u32 offset = 0, dfmt = 0, nfmt = 0;
    };
    std::vector<Buffer> buffers;
    std::vector<Buffer> stores;  // each buffer store's, the same way
    std::string why;  // empty if all of them were found
};
// compute: the SGPRs after the user ones (the group's number) differ from group to group
Values evaluate(const Program& p, const u32* user, u32 user_count, const shade::Memory& mem, bool compute = false);

// What else the translation depends on, from the draw's registers
struct PsState {
    u32 user_count = 0;
    u32 ena = 0, addr = 0;  // SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR
    u32 input_cntl[32] = {};  // SPI_PS_INPUT_CNTL_n
    u32 col_format = 0;  // SPI_SHADER_COL_FORMAT
    std::vector<u32> image_types;  // each image instruction's T# type (8 1D ... 13 2D array)
    std::vector<bool> unnormalized;  // and whether its sampler's coordinates are in texels
    u32 add_tid = 0;  // which buffer loads' descriptors add the lane to the index (not done)
    std::string key() const;
};

struct VsState {
    u32 user_count = 0;
    u32 attributes = 0;  // the pixel shader's
    u32 input_cntl[32] = {};  // SPI_PS_INPUT_CNTL_n: which parameter each of them is
    u32 add_tid = 0;
};

// A compute shader's: its user SGPRs, which others it is given (COMPUTE_PGM_RSRC2), the
// threads in a group, and where in U each buffer store's place is (three values each: the
// first pixel it writes, the image's width, and the elements in the buffer), filled in by
// vkdraw once it knows which image it writes
struct CsState {
    u32 user_count = 0, rsrc2 = 0;
    u32 threads[3] = {1, 1, 1};
    u32 store_slot = 0;
};

struct Glsl {
    std::string text, why;
    u32 attributes = 0;  // how many interpolated inputs (attributes 0 to n-1) it reads
    u32 buffers = 0;  // how many buffer loads (a vertex shader's)
};
Glsl pixel_shader(const Program& p, const PsState& s);

// The vertex shader: its position through the console's viewport (push constants), and the
// pixel shader's attributes from its parameters. Bindings: U at 16, the buffers from 17.
Glsl vertex_shader(const Program& p, const VsState& s);
// For a rectangle list: each three corners in, the fourth (b + c - a) added
std::string rect_geometry_shader(u32 attributes);

// The compute shader. Its buffer stores (by index, of 8-bit RGBA) go into storage images at
// bindings 32 onwards, viewed as 32-bit uints, each element a texel, as in memory.
Glsl compute_shader(const Program& p, const CsState& s);

// A trivial vertex shader that passes on a position and n attributes, computed on the CPU
std::string pass_vertex_shader(u32 attributes);

}  // namespace mensis::translate
