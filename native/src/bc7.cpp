// SPDX-License-Identifier: GPL-3.0-or-later
//
// BC7, the block compression most of the game's textures use: 16 bytes for 4x4 pixels, in one
// of eight modes (the position of the first set bit). A mode says how many subsets the block's
// pixels fall into (a partition, from a fixed table), how precise each subset's two colour
// endpoints are, and how many bits each pixel's index has, which picks a point between its
// subset's endpoints. The tables are BPTC's, as Direct3D's and OpenGL's specifications give them.
//
// No dependencies on the rest of the loader, so it can be checked on its own.

#include <cstdint>

namespace mensis::shade {
namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct Mode { int subsets, partition_bits, rotation_bits, selection_bit, color_bits, alpha_bits, endpoint_pbits, shared_pbits, index_bits, index2_bits; };
const Mode MODES[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}};

// two subsets: bit i is pixel i's subset
const std::uint16_t PARTITION2[64] = {
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec, 0xfe80,
    0xe800, 0xffe8, 0xff00, 0xfff0, 0xf000, 0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310,
    0x3100, 0x8cce, 0x088c, 0x3110, 0x6666, 0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c, 0xaaaa,
    0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696, 0xa55a, 0x73ce, 0x13c8, 0x324c, 0x3bdc,
    0x6996, 0xc33c, 0x9966, 0x0660, 0x0272, 0x04e4, 0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6,
    0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22};

const u8 PARTITION3[64][16] = {
    {0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2}, {0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1}, {0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1}, {0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2}, {0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2}, {0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1}, {0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},
    {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2}, {0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2}, {0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2}, {0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
    {0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2}, {0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2}, {0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2}, {0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},
    {0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2}, {0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0}, {0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2}, {0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},
    {0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2}, {0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1}, {0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2}, {0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
    {0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0}, {0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2}, {0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0}, {0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},
    {0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2}, {0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2}, {0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1}, {0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},
    {0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2}, {0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1}, {0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2}, {0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
    {0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0}, {0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0}, {0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0}, {0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},
    {0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1}, {0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2}, {0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1}, {0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},
    {0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1}, {0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1}, {0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1}, {0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
    {0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2}, {0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1}, {0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2}, {0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},
    {0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2}, {0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2}, {0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2}, {0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},
    {0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2}, {0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2}, {0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2}, {0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
    {0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1}, {0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2}, {0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2}, {0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0}};

// The anchor pixels: each subset's first pixel, whose index has one bit fewer
const u8 ANCHOR2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8,
                        8, 15, 2, 8, 2, 2, 8, 8, 2, 2, 15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15,
                        15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15};
const u8 ANCHOR3A[64] = {3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3,
                         6, 10, 5, 8, 8, 6, 8, 5, 15, 15, 8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5,
                         15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3};
const u8 ANCHOR3B[64] = {15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8,
                         15, 8, 3, 15, 6, 10, 15, 15, 10, 8, 15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15,
                         3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8};

const u8 WEIGHTS2[4] = {0, 21, 43, 64};
const u8 WEIGHTS3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
const u8 WEIGHTS4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Bits {
    const u8* p;
    u32 at = 0;
    u32 take(u32 n) {
        u32 v = 0;
        for (u32 i = 0; i < n; i++, at++) v |= u32((p[at >> 3] >> (at & 7)) & 1) << i;
        return v;
    }
};

u8 interpolate(u32 a, u32 b, u32 index, u32 bits) {
    u32 w = bits == 2 ? WEIGHTS2[index] : bits == 3 ? WEIGHTS3[index] : WEIGHTS4[index];
    return u8(((64 - w) * a + w * b + 32) >> 6);
}

}  // namespace

// Pixel (x, y) of a BC7 block, as 8-bit RGBA; false for a block that isn't valid (all 0)
bool bc7_texel(const unsigned char* block, u32 x, u32 y, unsigned char out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    int mode = 0;
    while (mode < 8 && !((block[0] >> mode) & 1)) mode++;
    if (mode == 8) return false;
    const Mode& m = MODES[mode];
    Bits b{block, u32(mode) + 1};
    u32 partition = b.take(u32(m.partition_bits));
    u32 rotation = b.take(u32(m.rotation_bits));
    u32 selection = b.take(u32(m.selection_bit));
    int ns = m.subsets;
    u32 ep[3][2][4] = {};  // subset, endpoint, channel
    for (int c = 0; c < 3; c++)
        for (int s = 0; s < ns; s++)
            for (int e = 0; e < 2; e++) ep[s][e][c] = b.take(u32(m.color_bits));
    if (m.alpha_bits)
        for (int s = 0; s < ns; s++)
            for (int e = 0; e < 2; e++) ep[s][e][3] = b.take(u32(m.alpha_bits));
    u32 pbit[3][2] = {};
    if (m.endpoint_pbits)
        for (int s = 0; s < ns; s++)
            for (int e = 0; e < 2; e++) pbit[s][e] = b.take(1);
    if (m.shared_pbits)
        for (int s = 0; s < ns; s++) pbit[s][0] = pbit[s][1] = b.take(1);
    bool pb = m.endpoint_pbits || m.shared_pbits;
    for (int s = 0; s < ns; s++)
        for (int e = 0; e < 2; e++)
            for (int c = 0; c < 4; c++) {
                u32 bits = u32(c < 3 ? m.color_bits : m.alpha_bits);
                if (!bits) {
                    ep[s][e][c] = 255;
                    continue;
                }
                u32 v = ep[s][e][c];
                if (pb) v = (v << 1) | pbit[s][e], bits++;
                v <<= 8 - bits;
                ep[s][e][c] = v | (v >> bits);
            }
    u32 pixel = y * 4 + x;
    auto subset_of = [&](u32 i) -> u32 {
        if (ns == 2) return (PARTITION2[partition] >> i) & 1;
        if (ns == 3) return PARTITION3[partition][i];
        return 0;
    };
    auto is_anchor = [&](u32 i) {
        if (i == 0) return true;
        if (ns == 2) return i == ANCHOR2[partition];
        if (ns == 3) return i == ANCHOR3A[partition] || i == ANCHOR3B[partition];
        return false;
    };
    // the indices: the anchors' have a bit fewer; all come in pixel order
    u32 index = 0, index2 = 0;
    for (u32 i = 0; i < 16; i++) {
        u32 n = u32(m.index_bits) - (is_anchor(i) ? 1 : 0);
        u32 v = b.take(n);
        if (i == pixel) index = v;
    }
    if (m.index2_bits)
        for (u32 i = 0; i < 16; i++) {
            u32 v = b.take(u32(m.index2_bits) - (i == 0 ? 1 : 0));
            if (i == pixel) index2 = v;
        }
    u32 s = subset_of(pixel);
    u32 cbits = u32(m.index_bits), abits = u32(m.index_bits), ci = index, ai = index;
    if (m.index2_bits) {
        if (selection) cbits = u32(m.index2_bits), ci = index2;
        else abits = u32(m.index2_bits), ai = index2;
    }
    for (int c = 0; c < 3; c++) out[c] = interpolate(ep[s][0][c], ep[s][1][c], ci, cbits);
    out[3] = m.alpha_bits ? interpolate(ep[s][0][3], ep[s][1][3], ai, abits) : 255;
    if (rotation) {
        unsigned char t = out[3];
        out[3] = out[rotation - 1];
        out[rotation - 1] = t;
    }
    return true;
}

}  // namespace mensis::shade
