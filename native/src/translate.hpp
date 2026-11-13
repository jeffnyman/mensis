// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's shaders translated for a PC's GPU (translate.cpp): GCN instructions into GLSL,
// which the Vulkan SDK's compiler (shaderc) turns into SPIR-V. So far pixel shaders, and only
// those without branches; what can't be translated says why, and the CPU (shade.cpp) still
// draws everything.

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
Program decode(u64 pc, const shade::Memory& mem);

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
    std::string why;  // empty if all of them were found
};
Values evaluate(const Program& p, const u32* user, u32 user_count, const shade::Memory& mem);

// What else the translation depends on, from the draw's registers
struct PsState {
    u32 user_count = 0;
    u32 ena = 0, addr = 0;  // SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR
    u32 input_cntl[32] = {};  // SPI_PS_INPUT_CNTL_n
    u32 col_format = 0;  // SPI_SHADER_COL_FORMAT
    std::vector<u32> image_types;  // each image instruction's T# type (8 1D ... 13 2D array)
    std::vector<bool> unnormalized;  // and whether its sampler's coordinates are in texels
    std::string key() const;
};

struct Glsl {
    std::string text, why;
    u32 attributes = 0;  // how many interpolated inputs (attributes 0 to n-1) it reads
};
Glsl pixel_shader(const Program& p, const PsState& s);

// A trivial vertex shader that passes on a position and n attributes, computed on the CPU
std::string vertex_shader(u32 attributes);

}  // namespace mensis::translate
