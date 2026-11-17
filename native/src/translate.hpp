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
        bool compare = false;  // a depth-compare sample (image_sample_c...)
    };
    std::vector<Image> images;
    struct Buffer {  // each buffer load's: the V#, the bytes into an element, the format
        u32 v[4] = {};
        u32 offset = 0, dfmt = 0, nfmt = 0;
        // or a compute shader's image store, as a store into its memory (a texel an element,
        // pitch of them to a row): the image's size, else 0
        u32 width = 0, height = 0, pitch = 0;
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
    std::vector<u32> image_types;  // each image instruction's T# type (8 1D ... 13 2D array); bindings 1-15, then 40 on
    std::vector<bool> unnormalized;  // and whether its sampler's coordinates are in texels
    std::vector<u32> image_slot;  // and which texture it reads (one for each descriptor); empty: one each
    std::vector<bool> integer;  // and whether its texels are integers (read as they are: usampler); empty: none
    // and, for a depth-compare sample, its sampler's compare function (bits 0-2), whether it
    // filters bilinearly (8), and whether the reference is clamped to 0-1 (16, a unorm
    // texture); 0 for any other; empty: none
    std::vector<u32> compare;
    std::vector<u32> buffer_slot;  // each buffer load's buffer (one for each descriptor, offset and format); empty: one each
    u32 add_tid = 0;  // which buffer loads' descriptors add the lane to the index (not done)
    // The GPU gives a pixel shader its corners' attributes and barycentrics: v_interp is then
    // done as the console does it, P0 + i (P1 - P0) + j (P2 - P0), so that an attribute the
    // same at all three corners comes out exactly that (else as the GPU interpolates)
    bool barycentric = false;
    u32 targets = 0xff;  // the render targets drawn into: an export to another goes nowhere
    // DB_SHADER_CONTROL: bit 0 on, the depth exported (mrtz's x) is the pixel's depth; an
    // export of stencil values or a coverage mask (bits 1, 2, 8) isn't done
    u32 db_shader = 0;
    std::string key() const;  // all of it, for a translation kept to be used again
};

struct VsState {
    u32 user_count = 0;
    u32 attributes = 0;  // the pixel shader's
    u32 input_cntl[32] = {};  // SPI_PS_INPUT_CNTL_n: which parameter each of them is
    u32 add_tid = 0;
    std::string key() const;  // all of it, for a translation kept to be used again
};

// A buffer format a GPU image can hold as it is in memory, an element a texel: its bytes, or
// 0 if not. 8-bit RGBA (unorm); 32-bit integers or floats, one, two or four; 16-bit floats,
// two or four; 11- and 10-bit floats.
u32 element_bytes(u32 dfmt, u32 nfmt);

// A compute shader's: its user SGPRs, which others it is given (COMPUTE_PGM_RSRC2), the
// threads in a group, and its buffer loads and stores by index. Each store, and each load of
// memory kept on the GPU, goes to an image of the kept memory, viewed as uints (its element,
// 4, 8 or 16 bytes, a texel), packed and unpacked exactly as the CPU's interpreter does it;
// any other load reads the buffer's elements as the CPU unpacked them. Where in U each one's
// place is: three values each (the first element of the image it reads or writes, the
// image's width, the elements in the buffer), filled in by vkdraw once it knows the image.
struct CsState {
    u32 user_count = 0, rsrc2 = 0;
    u32 threads[3] = {1, 1, 1};
    struct Access {
        u32 dfmt = 0, nfmt = 0;
        bool image = true;  // a load: of kept memory (else of the CPU's unpacked elements)
        // or of memory as it is, 32-bit values (one to four an element), U holding where an
        // element starts and how far apart they are, in dwords, and how many there are
        bool raw = false;
        u32 sel[4] = {4, 5, 6, 7};  // a load's dst_sel
        u32 width = 0, height = 0, pitch = 0;  // a store's, if into an image (its texels, pitch to a row)
    };
    std::vector<Access> stores, loads;
    u32 store_slot = 0, load_slot = 0;
};

struct Glsl {
    std::string text, why;
    u64 id = 0;  // (for whoever keeps it: the text's hash, made once, to tell it apart quickly)
    u32 attributes = 0;  // how many interpolated inputs (attributes 0 to n-1) it reads
    u32 buffers = 0;  // how many buffer loads (a vertex shader's)
    u32 outputs = 0;  // a pixel shader's: the render targets it exports to, a bit each
};
Glsl pixel_shader(const Program& p, const PsState& s);

// The vertex shader: its position through the console's viewport (push constants), and the
// pixel shader's attributes from its parameters. Bindings: U at 16, the buffers from 17 (up to
// 15, then from 72, up to 31 in all; a pixel shader's are from 56).
Glsl vertex_shader(const Program& p, const VsState& s);
// For a rectangle list: each three corners in, the fourth (b + c - a) added
std::string rect_geometry_shader(u32 attributes);

// The compute shader. Bindings: the images it stores into from 32, those it loads from 36, the
// unpacked buffers it loads from 56.
Glsl compute_shader(const Program& p, const CsState& s);

// A trivial vertex shader that passes on a position and n attributes, computed on the CPU
std::string pass_vertex_shader(u32 attributes);

}  // namespace mensis::translate
