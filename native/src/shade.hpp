// SPDX-License-Identifier: GPL-3.0-or-later
//
// Running the GPU's shaders on the CPU (shade.cpp), and what draws and dispatches need of it
// (draw.cpp): a wavefront of 64 threads, the memory the GPU may touch, the formats of buffers,
// images and render targets, and where an image's pixels are.
#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include "loader.hpp"

namespace mensis::shade {

constexpr int LANES = 64;
using Vec = std::array<u32, LANES>;

// The memory the GPU may read and write: the game's heap and its image, as they were when a
// draw or dispatch began. Anything else reads as 0 and isn't written.
struct Memory {
    std::vector<std::pair<u64, u64>> ranges;
    void snapshot();
    bool ok(u64 at, u64 bytes) const {
        for (auto& [lo, hi] : ranges)
            if (at >= lo && at + bytes <= hi && at + bytes >= at) return true;
        return false;
    }
};

// What a shader hands out, and what it is handed for interpolation
struct Io {
    virtual ~Io() = default;
    // exp: target (mrt0-7 = 0-7, mrtz 8, null 9, pos0-3 12-15, param0-31 32-63), its four
    // registers (null where a channel isn't enabled), compressed (two registers of halves),
    // and the lanes it applies to
    virtual void exp(u32 target, const Vec* const regs[4], bool compr, u64 lanes) = 0;
    // v_interp: attribute attr's channel chan for this wave's primitive: its value at the first
    // vertex (p0), and the differences to the second (p10) and third (p20)
    virtual void attr(u32 attr, u32 chan, float& p0, float& p10, float& p20) {
        (void)attr, (void)chan;
        p0 = p10 = p20 = 0;
    }
};

// One wavefront: 64 threads in step, the scalar registers they share, each one's vector
// registers, and which of them run (exec).
struct Wave {
    std::array<u32, 256> s{};  // SGPRs s0-s103, then the special registers at their codes:
                               // vcc 106-107, m0 124, exec 126-127
    std::vector<Vec> v = std::vector<Vec>(256);
    bool scc = false;
    std::vector<unsigned char>* lds = nullptr;  // the work group's local data share
    u64 exec() const { return s[126] | (u64(s[127]) << 32); }
    void set_exec(u64 m) { s[126] = u32(m), s[127] = u32(m >> 32); }
    u64 vcc() const { return s[106] | (u64(s[107]) << 32); }
    void set_vcc(u64 m) { s[106] = u32(m), s[107] = u32(m >> 32); }
};

enum class Stop { end, barrier, fault };
struct Result { Stop stop; u64 pc; };

// Run the wave from pc until s_endpgm (end), s_barrier (resume at pc), or something it can't
// do (fault, noted)
Result run(Wave& w, u64 pc, Io& io, const Memory& mem);

// What couldn't be done, counted for the summary
void note(const std::string& what, u64 n = 1);
std::map<std::string, u64> notes();

// ---- formats ----

// An image or buffer element's layout: the bits of each component, from the lowest (x)
struct Layout { int bits[4]; int count; int bytes; };
// data formats (images', buffers' and render targets' share their numbers); count 0 = unknown
Layout layout(u32 dfmt);
bool is_block(u32 dfmt);  // block-compressed (BC1-7): 4x4 pixels an element
// The number formats: unorm 0, snorm 1, uscaled 2, sscaled 3, uint 4, sint 5, snorm_nz 6,
// float 7, srgb 9. Values as the shader sees them: 32-bit floats, or integers for uint/sint.
void unpack(u32 dfmt, u32 nfmt, const unsigned char* p, u32 out[4]);
void pack(u32 dfmt, u32 nfmt, const u32 in[4], unsigned char* p);
float f(u32 bits);
u32 u(float x);
float half(u32 h);
u32 to_half(float x);

// An image descriptor (T#), eight dwords
struct Image {
    u64 base = 0;
    u32 dfmt = 0, nfmt = 0, width = 1, height = 1, depth = 1, pitch = 1;
    u32 sel[4] = {4, 5, 6, 7};  // dst_sel: 0 zero, 1 one, 4-7 x y z w
    u32 tiling = 0, type = 9, base_level = 0, base_array = 0, last_array = 0;
    explicit Image(const u32* t);
    Image() = default;
    bool array() const { return type == 12 || type == 13 || type == 15; }
};

// Where element (x, y) of slice `slice` is (an element is a pixel, or a 4x4 block), 0 if
// outside. tiling is the PS4's tile mode index: linear (8), micro-tiled in 8x8 tiles (the
// 1D modes, exactly as the PS4 lays them out), or macro-tiled (the 2D modes), which the loader
// keeps in rows: the console's arrangement of those isn't reproduced, and as only the GPU (the
// loader, here) writes and reads them, rows serve as well.
u64 element_address(u64 base, u32 tiling, u32 bytes, u32 pitch, u32 height, u32 x, u32 y, u32 slice);

// Pixel (x, y) of a BC7 block as 8-bit RGBA (bc7.cpp); false (and 0) for an invalid block
bool bc7_texel(const unsigned char* block, u32 x, u32 y, unsigned char out[4]);

}  // namespace mensis::shade
