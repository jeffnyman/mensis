// SPDX-License-Identifier: GPL-3.0-or-later
//
// The GPU's shaders, run on the CPU: an interpreter for AMD's GCN (second generation, "Sea
// Islands"), as AMD's reference for its instruction set describes it.
//
// A wave is 64 threads ("lanes") in step. Scalar instructions (s_) run once, on the SGPRs the
// lanes share; vector instructions (v_) run for each lane exec lets run, on that lane's VGPRs.
// Branches are scalar: a wave whose lanes disagree runs both sides, with exec switching lanes
// off and on (s_and_saveexec_b64 ... s_mov_b64 exec). Memory comes through descriptors the
// shader is given: a buffer's (V#: where, how big, its format), an image's (T#: where, its
// size, format and tiling) and a sampler's (S#: how to filter and wrap).
//
// What isn't done is counted (note) and shows in the summary, rather than stopping the run:
// mip levels past the first, depth comparison, a few rarer instructions.

#include "shade.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>

namespace mensis::shade {
namespace {

std::mutex g_notes_lock;
std::map<std::string, u64> g_notes;

u32 sext(u32 v, int bits) { return u32(std::int32_t(v << (32 - bits)) >> (32 - bits)); }

double dbl(u64 b) {
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}
u64 ubl(double d) {
    u64 b;
    std::memcpy(&b, &d, 8);
    return b;
}

}  // namespace

float f(u32 bits) {
    float r;
    std::memcpy(&r, &bits, 4);
    return r;
}
u32 u(float x) {
    u32 r;
    std::memcpy(&r, &x, 4);
    return r;
}

float half(u32 h) {  // bit by bit: a normal half's exponent and mantissa move into place
    u32 sign = (h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    if (e == 0) {
        float v = float(m) * (1.0f / 16777216.0f);  // a denormal: m * 2^-24
        return sign ? -v : v;
    }
    if (e == 31) return f(sign | 0x7f800000 | (m << 13));
    return f(sign | ((e + 112) << 23) | (m << 13));
}

namespace {
u32 to_half_mode(float x, bool toward_zero) {  // rounded to nearest even, or toward zero
    u32 b = u(x), sign = (b >> 16) & 0x8000, e = (b >> 23) & 0xff, m = b & 0x7fffff;
    if (e == 255) return sign | 0x7c00 | (m ? 0x200 : 0);
    int he = int(e) - 112;  // the half's exponent
    if (he >= 31) return sign | (toward_zero ? 0x7bff : 0x7c00);
    u32 q, shift;
    if (he <= 0) {  // a denormal half, or 0
        if (he < -10) return sign;
        m |= 0x800000;
        shift = u32(14 - he);
    } else {
        shift = 13;
    }
    q = m >> shift;
    if (!toward_zero) {
        u32 rem = m & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (q & 1))) q++;
    }
    if (he <= 0) return sign | q;  // rounding up into the smallest normal is still right
    if (q == 0x400) q = 0, he++;
    if (he >= 31) return sign | (toward_zero ? 0x7bff : 0x7c00);
    return sign | (u32(he) << 10) | q;
}
}  // namespace

u32 to_half(float x) { return to_half_mode(x, false); }

void note(const std::string& what, u64 n) {
    std::lock_guard lock(g_notes_lock);
    g_notes[what] += n;
}

std::map<std::string, u64> notes() {
    std::lock_guard lock(g_notes_lock);
    return g_notes;
}

std::atomic<u32> g_generation{1};  // a draw's or dispatch's number, for the block cache

void Memory::snapshot() {
    g_generation++;
    ranges.clear();
    {
        std::lock_guard lock(M.heap_lock);
        ranges.emplace_back(M.heap_base, M.heap_top);
    }
    for (auto r : M.image) ranges.push_back(r);
}

// ---- formats ----

Layout layout(u32 dfmt) {
    switch (dfmt) {
        case 1: return {{8}, 1, 1};
        case 2: return {{16}, 1, 2};
        case 3: return {{8, 8}, 2, 2};
        case 4: return {{32}, 1, 4};
        case 5: return {{16, 16}, 2, 4};
        case 6: return {{11, 11, 10}, 3, 4};  // "10_11_11": the names go from the top bits down
        case 7: return {{10, 11, 11}, 3, 4};
        case 8: return {{2, 10, 10, 10}, 4, 4};
        case 9: return {{10, 10, 10, 2}, 4, 4};
        case 10: return {{8, 8, 8, 8}, 4, 4};
        case 11: return {{32, 32}, 2, 8};
        case 12: return {{16, 16, 16, 16}, 4, 8};
        case 13: return {{32, 32, 32}, 3, 12};
        case 14: return {{32, 32, 32, 32}, 4, 16};
        case 16: return {{5, 6, 5}, 3, 2};
        case 17: return {{5, 5, 5, 1}, 4, 2};
        case 18: return {{1, 5, 5, 5}, 4, 2};
        case 19: return {{4, 4, 4, 4}, 4, 2};
        case 20: return {{24, 8}, 2, 4};
        case 21: return {{8, 24}, 2, 4};
        case 35: case 38: return {{}, 4, 8};  // BC1, BC4: 8 bytes a 4x4 block
        case 36: case 37: case 39: case 40: case 41: return {{}, 4, 16};
        default: return {{}, 0, 0};
    }
}

bool is_block(u32 dfmt) { return dfmt >= 35 && dfmt <= 41; }

namespace {

float small_float(u32 raw, int bits) {  // the unsigned 11- and 10-bit floats: 5 exponent bits
    int mbits = bits - 5;
    u32 e = raw >> mbits, m = raw & ((1u << mbits) - 1);
    if (e == 0) return std::ldexp(float(m), -14 - mbits);
    if (e == 31) return m ? NAN : INFINITY;
    return std::ldexp(float(m | (1u << mbits)), int(e) - 15 - mbits);
}

u32 to_small_float(float x, int bits) {
    int mbits = bits - 5;
    if (!(x > 0)) return 0;
    if (std::isinf(x)) return 31u << mbits;
    int e;
    std::frexp(x, &e);  // x in [2^(e-1), 2^e)
    int be = e - 1 + 15;
    if (be <= 0) return u32(std::nearbyint(std::ldexp(x, 14 + mbits)));
    u32 q = u32(std::nearbyint(std::ldexp(x, mbits - (e - 1))));  // 1.m scaled
    if (q >> (mbits + 1)) {
        q >>= 1;
        be++;
    }
    if (be >= 31) return (30u << mbits) | ((1u << mbits) - 1);
    return (u32(be) << mbits) | (q & ((1u << mbits) - 1));
}

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float linear_to_srgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

bool is_int(u32 nfmt) { return nfmt == 4 || nfmt == 5; }

}  // namespace

void unpack(u32 dfmt, u32 nfmt, const unsigned char* p, u32 out[4]) {
    Layout l = layout(dfmt);
    out[0] = out[1] = out[2] = 0;
    out[3] = is_int(nfmt) ? 1 : u(1.0f);
    if (!l.count || is_block(dfmt)) return;
    u64 word = 0;
    std::memcpy(&word, p, std::min(l.bytes, 8));
    int shift = 0;
    for (int c = 0; c < l.count; c++) {
        int b = l.bits[c];
        u32 raw;
        if (b == 32) {
            std::memcpy(&raw, p + shift / 8, 4);
        } else {
            raw = u32(word >> shift) & ((1u << b) - 1);
        }
        shift += b;
        float max = float((1ull << b) - 1);
        switch (nfmt) {
            case 0: out[c] = u(float(raw) / max); break;
            case 9: out[c] = u(c < 3 ? srgb_to_linear(float(raw) / max) : float(raw) / max); break;
            case 1: case 6: out[c] = u(std::max(float(std::int32_t(sext(raw, b))) / float((1u << (b - 1)) - 1), -1.0f)); break;
            case 2: out[c] = u(float(raw)); break;
            case 3: out[c] = u(float(std::int32_t(sext(raw, b)))); break;
            case 4: out[c] = raw; break;
            case 5: out[c] = sext(raw, b); break;
            case 7:
                out[c] = b == 32 ? raw : b == 16 ? u(half(raw)) : b == 11 || b == 10 ? u(small_float(raw, b)) : u(float(raw));
                break;
            default: out[c] = raw;
        }
    }
}

void pack(u32 dfmt, u32 nfmt, const u32 in[4], unsigned char* p) {
    Layout l = layout(dfmt);
    if (!l.count || is_block(dfmt)) return;
    u64 word = 0;
    int shift = 0;
    for (int c = 0; c < l.count; c++) {
        int b = l.bits[c];
        u32 max = b == 32 ? 0xffffffffu : (1u << b) - 1;
        float x = f(in[c]);
        u32 raw;
        switch (nfmt) {
            case 0: raw = u32(std::nearbyint(std::clamp(std::isnan(x) ? 0.0f : x, 0.0f, 1.0f) * float(max))); break;
            case 9: raw = u32(std::nearbyint((c < 3 ? linear_to_srgb(x) : std::clamp(x, 0.0f, 1.0f)) * float(max))); break;
            case 1: case 6: {
                float m = float((1u << (b - 1)) - 1);
                raw = u32(std::int32_t(std::nearbyint(std::clamp(x, -1.0f, 1.0f) * m)));
                break;
            }
            case 2: raw = u32(std::clamp(x, 0.0f, float(max))); break;
            case 3: raw = u32(std::int32_t(x)); break;
            case 4: raw = std::min(in[c], max); break;
            case 5: raw = in[c]; break;
            case 7: raw = b == 32 ? in[c] : b == 16 ? to_half(x) : b == 11 || b == 10 ? to_small_float(x, b) : u32(x); break;
            default: raw = in[c];
        }
        if (b == 32) {
            std::memcpy(p + shift / 8, &raw, 4);
        } else {
            word |= u64(raw & max) << shift;
        }
        shift += b;
    }
    if (l.count && l.bits[0] != 32) std::memcpy(p, &word, size_t(l.bytes));
}

Image::Image(const u32* t) {
    base = (u64(t[0]) | (u64(t[1] & 0xff) << 32)) << 8;
    dfmt = (t[1] >> 20) & 0x3f;
    nfmt = (t[1] >> 26) & 0xf;
    width = (t[2] & 0x3fff) + 1;
    height = ((t[2] >> 14) & 0x3fff) + 1;
    for (int c = 0; c < 4; c++) sel[c] = (t[3] >> (3 * c)) & 7;
    base_level = (t[3] >> 12) & 0xf;
    tiling = (t[3] >> 20) & 0x1f;
    type = (t[3] >> 28) & 0xf;
    depth = (t[4] & 0x1fff) + 1;
    pitch = ((t[4] >> 13) & 0x3fff) + 1;
    base_array = t[5] & 0x1fff;
    last_array = (t[5] >> 13) & 0x1fff;
}

namespace {

// The order of the 64 elements in an 8x8 micro tile: which bit of the index each bit of x and
// y becomes. A displayable layout depends on the element's size; the others interleave x and y.
u32 micro_index(u32 tiling, u32 bytes, u32 x, u32 y) {
    auto bit = [](u32 v, int i) { return (v >> i) & 1; };
    bool display = tiling >= 8 && tiling <= 12;
    if (!display) return bit(x, 0) | bit(y, 0) << 1 | bit(x, 1) << 2 | bit(y, 1) << 3 | bit(x, 2) << 4 | bit(y, 2) << 5;
    switch (bytes) {
        case 1: return bit(x, 0) | bit(x, 1) << 1 | bit(x, 2) << 2 | bit(y, 1) << 3 | bit(y, 0) << 4 | bit(y, 2) << 5;
        case 2: return bit(x, 0) | bit(x, 1) << 1 | bit(x, 2) << 2 | bit(y, 0) << 3 | bit(y, 1) << 4 | bit(y, 2) << 5;
        case 4: return bit(x, 0) | bit(x, 1) << 1 | bit(y, 0) << 2 | bit(x, 2) << 3 | bit(y, 1) << 4 | bit(y, 2) << 5;
        case 8: return bit(x, 0) | bit(y, 0) << 1 | bit(x, 1) << 2 | bit(x, 2) << 3 | bit(y, 1) << 4 | bit(y, 2) << 5;
        default: return bit(y, 0) | bit(x, 0) << 1 | bit(x, 1) << 2 | bit(x, 2) << 3 | bit(y, 1) << 4 | bit(y, 2) << 5;
    }
}

}  // namespace

u64 element_address(u64 base, u32 tiling, u32 bytes, u32 pitch, u32 height, u32 x, u32 y, u32 slice) {
    if (x >= pitch || !bytes) return 0;
    bool micro = tiling == 5 || tiling == 9 || tiling == 13 || tiling == 19;
    if (micro) {
        u64 tiles_x = (pitch + 7) / 8, tiles_y = (height + 7) / 8, tile = 64ull * bytes;
        return base + slice * tiles_x * tiles_y * tile + ((y / 8) * tiles_x + x / 8) * tile +
               u64(micro_index(tiling, bytes, x & 7, y & 7)) * bytes;
    }
    u64 rows = (u64(height) + 7) / 8 * 8;
    return base + (slice * rows + y) * pitch * bytes + u64(x) * bytes;
}

void Image::prepare(const Memory& mem) {
    Layout l = layout(dfmt);
    bytes = u32(l.bytes);
    block = is_block(dfmt);
    epitch = block ? (std::max(pitch, width) + 3) / 4 : std::max(pitch, width);
    eheight = block ? (height + 3) / 4 : height;
    u64 slices = std::max({depth, last_array + 1, type == 11 ? 6u : 1u});
    bool micro = tiling == 5 || tiling == 9 || tiling == 13 || tiling == 19;
    size = micro ? (u64(epitch) + 7) / 8 * ((u64(eheight) + 7) / 8) * 64 * bytes * slices
                 : u64(epitch) * ((u64(eheight) + 7) / 8 * 8) * bytes * slices;
    all_ok = bytes && mem.ok(base, size);
}

namespace {

// ---- block compression (BC1-5), one texel of a 4x4 block ----

void rgb565(u32 c, float o[3]) {
    o[0] = float((c >> 11) & 31) / 31.0f;
    o[1] = float((c >> 5) & 63) / 63.0f;
    o[2] = float(c & 31) / 31.0f;
}

void bc_color(const unsigned char* b, u32 i, bool four, float o[4]) {
    u32 c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    u32 idx = ((b[4] | b[5] << 8 | b[6] << 16 | u32(b[7]) << 24) >> (2 * i)) & 3;
    float a[3], z[3];
    rgb565(c0, a);
    rgb565(c1, z);
    o[3] = 1;
    for (int k = 0; k < 3; k++) {
        if (four || c0 > c1) {
            float p[4] = {a[k], z[k], (2 * a[k] + z[k]) / 3, (a[k] + 2 * z[k]) / 3};
            o[k] = p[idx];
        } else {
            float p[4] = {a[k], z[k], (a[k] + z[k]) / 2, 0};
            o[k] = p[idx];
        }
    }
    if (!four && c0 <= c1 && idx == 3) o[3] = 0;
}

float bc_alpha(const unsigned char* b, u32 i) {  // BC3's alpha, BC4 and BC5's channels
    float a0 = b[0] / 255.0f, a1 = b[1] / 255.0f;
    u64 bits = 0;
    for (int k = 0; k < 6; k++) bits |= u64(b[2 + k]) << (8 * k);
    u32 idx = (bits >> (3 * i)) & 7;
    if (idx == 0) return a0;
    if (idx == 1) return a1;
    if (b[0] > b[1]) return ((8 - idx) * a0 + (idx - 1) * a1) / 7;
    if (idx == 6) return 0;
    if (idx == 7) return 1;
    return ((6 - idx) * a0 + (idx - 1) * a1) / 5;
}

bool block_texel(u32 dfmt, const unsigned char* b, u32 px, u32 py, float o[4]) {
    u32 i = py * 4 + px;
    o[0] = o[1] = o[2] = 0;
    o[3] = 1;
    switch (dfmt) {
        case 35: bc_color(b, i, false, o); return true;
        case 36: bc_color(b + 8, i, true, o); o[3] = float((b[i / 2] >> (4 * (i & 1))) & 15) / 15.0f; return true;
        case 37: bc_color(b + 8, i, true, o); o[3] = bc_alpha(b, i); return true;
        case 38: o[0] = bc_alpha(b, i); return true;
        case 39: o[0] = bc_alpha(b, i); o[1] = bc_alpha(b + 8, i); return true;
        case 41: {
            unsigned char c[4];
            bc7_texel(b, px, py, c);
            for (int k = 0; k < 4; k++) o[k] = float(c[k]) / 255.0f;
            return true;
        }
        default: return false;
    }
}

// ---- images ----

struct Sampler {
    u32 clamp[3] = {0, 0, 0};
    bool unnormalized = false;
    u32 mag = 0, min = 0, border = 0;
    explicit Sampler(const u32* s) {
        for (int k = 0; k < 3; k++) clamp[k] = (s[0] >> (3 * k)) & 7;
        unnormalized = (s[0] >> 15) & 1;
        mag = (s[2] >> 20) & 3;
        min = (s[2] >> 22) & 3;
        border = (s[3] >> 30) & 3;
    }
};

// One texel of slice `slice`, as the shader would see it before dst_sel: floats, or integers
// for uint/sint formats. Outside the image: 0.
// A compressed block's 16 pixels, decoded once: a small cache for each thread, emptied for
// each draw or dispatch (a texture's memory may change between them). Null if the format
// isn't decoded.
struct BlockCache {
    u64 tag[1024];
    u32 generation[1024];
    float px[1024][64];
};

const float* cached_block(u32 dfmt, u64 at) {
    thread_local std::unique_ptr<BlockCache> cache;
    if (!cache) {
        cache = std::make_unique<BlockCache>();
        std::memset(cache->generation, 0, sizeof cache->generation);
    }
    u32 g = g_generation.load(std::memory_order_relaxed);
    size_t i = ((at >> 3) ^ (at >> 13)) & 1023;
    u64 tag = at ^ (u64(dfmt) << 56);
    float* px = cache->px[i];
    if (cache->tag[i] == tag && cache->generation[i] == g) return px;
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++)
            if (!block_texel(dfmt, reinterpret_cast<const unsigned char*>(at), x, y, px + (y * 4 + x) * 4)) return nullptr;
    cache->tag[i] = tag;
    cache->generation[i] = g;
    return px;
}

// One texel of slice `slice`, as the shader would see it before dst_sel: floats, or integers
// for uint/sint formats. Outside the image: 0. The image is prepared.
void texel(const Image& t, const Memory& mem, int x, int y, u32 slice, u32 o[4]) {
    o[0] = o[1] = o[2] = 0;
    o[3] = is_int(t.nfmt) ? 1 : u(1.0f);
    if (x < 0 || y < 0 || u32(x) >= t.width || u32(y) >= t.height) return;
    if (!t.bytes) {
        note(std::format("images of data format {}", t.dfmt));
        return;
    }
    if (t.block) {
        u64 at = element_address(t.base, t.tiling, t.bytes, t.epitch, t.eheight, u32(x) / 4, u32(y) / 4, slice);
        if (!at || (!t.all_ok && !mem.ok(at, t.bytes))) return;
        const float* c = cached_block(t.dfmt, at);
        if (!c) {
            note(std::format("images compressed as BC{} (not decoded; read as 0)", t.dfmt - 34));
            return;
        }
        c += ((u32(y) & 3) * 4 + (u32(x) & 3)) * 4;
        for (int k = 0; k < 4; k++) o[k] = u(t.nfmt == 9 && k < 3 ? srgb_to_linear(c[k]) : c[k]);
        return;
    }
    u64 at = element_address(t.base, t.tiling, t.bytes, t.epitch, t.eheight, u32(x), u32(y), slice);
    if (!at || (!t.all_ok && !mem.ok(at, t.bytes))) return;
    const auto* p = reinterpret_cast<const unsigned char*>(at);
    if (t.dfmt == 10 && t.nfmt == 0) {  // 8-bit RGBA, the usual
        for (int k = 0; k < 4; k++) o[k] = u(float(p[k]) * (1.0f / 255.0f));
        return;
    }
    if (t.nfmt == 7 && (t.dfmt == 4 || t.dfmt == 11 || t.dfmt == 14)) {  // 32-bit floats: as they are
        std::memcpy(o, p, t.bytes);
        return;
    }
    unpack(t.dfmt, t.nfmt, p, o);
}

void swizzle(const u32 sel[4], bool integer, const u32 in[4], u32 out[4]) {
    for (int c = 0; c < 4; c++) {
        u32 s = sel[c];
        out[c] = s >= 4 ? in[s - 4] : s == 1 ? (integer ? 1 : u(1.0f)) : 0;
    }
}

// Texel coordinate along one axis, as the sampler's addressing mode places it: wrap (0),
// mirror (1), clamp to the edge (2 and the "last texel" modes), or the border (6, 7: -1)
int address(int i, int n, u32 mode) {
    if (i >= 0 && i < n) return i;
    switch (mode) {
        case 0: return ((i % n) + n) % n;
        case 1: {
            int p = ((i % (2 * n)) + 2 * n) % (2 * n);
            return p < n ? p : 2 * n - 1 - p;
        }
        case 6: case 7: case 4: case 5: return -1;
        default: return std::clamp(i, 0, n - 1);
    }
}

void fetch_addressed(const Image& t, const Memory& mem, const Sampler& s, int x, int y, u32 slice, u32 o[4]) {
    int ax = address(x, int(t.width), s.clamp[0]), ay = address(y, int(t.height), s.clamp[1]);
    if (ax < 0 || ay < 0) {
        float b = s.border == 2 ? 1.0f : 0.0f, a = s.border == 0 ? 0.0f : 1.0f;
        o[0] = o[1] = o[2] = u(b);
        o[3] = u(a);
        return;
    }
    texel(t, mem, ax, ay, slice, o);
}

// Filtered: point (0) or bilinear (1 and anisotropic, done as bilinear). x and y in texels.
void filtered(const Image& t, const Memory& mem, const Sampler& s, u32 filter, float x, float y, u32 slice, u32 o[4]) {
    if (filter == 0 || is_int(t.nfmt)) {
        fetch_addressed(t, mem, s, int(std::floor(x)), int(std::floor(y)), slice, o);
        return;
    }
    float fx = x - 0.5f, fy = y - 0.5f;
    int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    float ax = fx - float(x0), ay = fy - float(y0);
    u32 c[4][4];
    fetch_addressed(t, mem, s, x0, y0, slice, c[0]);
    fetch_addressed(t, mem, s, x0 + 1, y0, slice, c[1]);
    fetch_addressed(t, mem, s, x0, y0 + 1, slice, c[2]);
    fetch_addressed(t, mem, s, x0 + 1, y0 + 1, slice, c[3]);
    for (int k = 0; k < 4; k++) {
        float top = f(c[0][k]) * (1 - ax) + f(c[1][k]) * ax;
        float bottom = f(c[2][k]) * (1 - ax) + f(c[3][k]) * ax;
        o[k] = u(top * (1 - ay) + bottom * ay);
    }
}

// ---- operands ----

u32 inline_float(u32 code) {
    static const u32 v[] = {0x3f000000, 0xbf000000, 0x3f800000, 0xbf800000,
                            0x40000000, 0xc0000000, 0x40800000, 0xc0800000};
    return v[code - 240];
}

u32 sget(const Wave& w, u32 code, u32 lit) {
    if (code < 128) return w.s[code];
    if (code <= 192) return code - 128;
    if (code <= 208) return u32(192 - int(code));
    if (code >= 240 && code <= 247) return inline_float(code);
    switch (code) {
        case 251: return w.vcc() == 0;
        case 252: return w.exec() == 0;
        case 253: return w.scc;
        case 255: return lit;
        default: return 0;
    }
}

u64 sget64(const Wave& w, u32 code, u32 lit, bool f64 = false) {
    if (code < 128) return w.s[code] | (u64(w.s[(code + 1) & 255]) << 32);
    if (code <= 192) return code - 128;
    if (code <= 208) return u64(i64(192 - int(code)));
    if (code >= 240 && code <= 247) {
        static const double d[] = {0.5, -0.5, 1, -1, 2, -2, 4, -4};
        return f64 ? ubl(d[code - 240]) : inline_float(code);
    }
    if (code == 255) return f64 ? u64(lit) << 32 : lit;
    return sget(w, code, lit);
}

void sset(Wave& w, u32 code, u32 v) {
    if (code < 128) w.s[code] = v;
}
void sset64(Wave& w, u32 code, u64 v) {
    if (code < 127) {
        w.s[code] = u32(v);
        w.s[code + 1] = u32(v >> 32);
    }
}

struct Src {
    const Vec* v = nullptr;
    u32 k = 0;
    u32 operator[](int l) const { return v ? (*v)[size_t(l)] : k; }
};
Src vsrc(const Wave& w, u32 code, u32 lit) {
    if (code >= 256) return {&w.v[code - 256], 0};
    return {nullptr, sget(w, code, lit)};
}
struct Src64 {
    const Vec* lo = nullptr;
    const Vec* hi = nullptr;
    u64 k = 0;
    u64 operator[](int l) const { return lo ? (*lo)[size_t(l)] | (u64((*hi)[size_t(l)]) << 32) : k; }
};
Src64 vsrc64(const Wave& w, u32 code, u32 lit, bool f64) {
    if (code >= 256) return {&w.v[code - 256], &w.v[(code - 255) & 255], 0};
    return {nullptr, nullptr, sget64(w, code, lit, f64)};
}

// ---- vector ALU ----

// Ops whose result is a float, for clamp and the output modifier
bool float_result(u32 op) {
    if (op >= 0x103 && op <= 0x108) return true;
    if (op >= 0x10d && op <= 0x110) return true;
    if (op == 0x11f || op == 0x120 || op == 0x121 || op == 0x12b) return true;
    if (op == 0x140 || op == 0x141 || op == 0x14b || op == 0x151 || op == 0x154 || op == 0x157) return true;
    if (op >= 0x144 && op <= 0x147) return true;
    if (op == 0x15f || op == 0x16d || op == 0x16f) return true;
    if (op == 0x185 || op == 0x186 || op == 0x18b || op == 0x18e || op == 0x18f) return true;
    if (op >= 0x191 && op <= 0x194) return true;
    if (op >= 0x1a0 && op <= 0x1b6 && op != 0x1af && op != 0x1b0 && op != 0x1b1 && op != 0x1b2 && op != 0x1b4) return true;
    return op == 0x1c0 || op == 0x1c5 || op == 0x1c6;
}

float legacy_mul(float a, float b) { return a == 0 || b == 0 ? 0.0f : a * b; }

int cube_face(float x, float y, float z) {
    float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
    if (az >= ax && az >= ay) return z < 0 ? 5 : 4;
    if (ay >= ax) return y < 0 ? 3 : 2;
    return x < 0 ? 1 : 0;
}

float med3(float a, float b, float c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }

// One lane of a 32-bit vector op (VOP3 numbering): a, b, c its sources, d what its destination
// held. ok is cleared for an op not done here.
u32 alu(u32 op, u32 a, u32 b, u32 c, u32 d, bool& ok) {
    float fa = f(a), fb = f(b), fc = f(c);
    auto i24 = [](u32 v) { return i64(std::int32_t(v << 8) >> 8); };
    switch (op) {
        case 0x103: return u(fa + fb);
        case 0x104: return u(fa - fb);
        case 0x105: return u(fb - fa);
        case 0x106: return u(legacy_mul(fa, fb) + f(d));
        case 0x107: return u(legacy_mul(fa, fb));
        case 0x108: return u(fa * fb);
        case 0x109: return u32(i24(a) * i24(b));
        case 0x10a: return u32((i24(a) * i24(b)) >> 32);
        case 0x10b: return (a & 0xffffff) * (b & 0xffffff);
        case 0x10c: return u32((u64(a & 0xffffff) * (b & 0xffffff)) >> 32);
        case 0x10d: return fa < fb ? a : b;
        case 0x10e: return fa >= fb ? a : b;
        case 0x10f: return u(std::fmin(fa, fb));
        case 0x110: return u(std::fmax(fa, fb));
        case 0x111: return u32(std::min(std::int32_t(a), std::int32_t(b)));
        case 0x112: return u32(std::max(std::int32_t(a), std::int32_t(b)));
        case 0x113: return std::min(a, b);
        case 0x114: return std::max(a, b);
        case 0x115: return a >> (b & 31);
        case 0x116: return b >> (a & 31);
        case 0x117: return u32(std::int32_t(a) >> (b & 31));
        case 0x118: return u32(std::int32_t(b) >> (a & 31));
        case 0x119: return a << (b & 31);
        case 0x11a: return b << (a & 31);
        case 0x11b: return a & b;
        case 0x11c: return a | b;
        case 0x11d: return a ^ b;
        case 0x11e: return ((1u << (a & 31)) - 1) << (b & 31);
        case 0x11f: return u(fa * fb + f(d));
        case 0x122: return u32(std::popcount(a)) + b;
        case 0x12b: return u(std::ldexp(fa, std::int32_t(b)));
        case 0x12d: return (u32(std::int32_t(std::nearbyint(std::clamp(fa, -1.0f, 1.0f) * 32767))) & 0xffff) |
                           (u32(std::int32_t(std::nearbyint(std::clamp(fb, -1.0f, 1.0f) * 32767))) << 16);
        case 0x12e: return u32(std::nearbyint(std::clamp(fa, 0.0f, 1.0f) * 65535)) |
                           (u32(std::nearbyint(std::clamp(fb, 0.0f, 1.0f) * 65535)) << 16);
        case 0x12f: return to_half_mode(fa, true) | (to_half_mode(fb, true) << 16);
        case 0x130: return std::min(a, 0xffffu) | (std::min(b, 0xffffu) << 16);
        case 0x131: return (u32(std::clamp(std::int32_t(a), -32768, 32767)) & 0xffff) |
                           (u32(std::clamp(std::int32_t(b), -32768, 32767)) << 16);
        case 0x140: return u(legacy_mul(fa, fb) + fc);
        case 0x141: return u(fa * fb + fc);
        case 0x142: return u32(i24(a) * i24(b) + i64(std::int32_t(c)));
        case 0x143: return (a & 0xffffff) * (b & 0xffffff) + c;
        case 0x144: return u(float(cube_face(fa, fb, fc)));
        case 0x145: {
            static const float sx[] = {0, 0, 1, 1, 1, -1}, sz[] = {-1, 1, 0, 0, 0, 0};
            int k = cube_face(fa, fb, fc);
            return u(sx[k] * fa + sz[k] * fc);
        }
        case 0x146: {
            int k = cube_face(fa, fb, fc);
            return u(k == 2 ? fc : k == 3 ? -fc : -fb);
        }
        case 0x147: {
            int k = cube_face(fa, fb, fc);
            return u(2 * (k < 2 ? fa : k < 4 ? fb : fc));
        }
        case 0x148: {
            u32 off = b & 31, width = c & 31;
            return width ? (a >> off) & ((1u << width) - 1) : 0;
        }
        case 0x149: {
            u32 off = b & 31, width = c & 31;
            return width ? sext(a >> off, int(width)) : 0;
        }
        case 0x14a: return (a & b) | (~a & c);
        case 0x14b: return u(std::fma(fa, fb, fc));
        case 0x14d: {
            u32 r = 0;
            for (int k = 0; k < 32; k += 8) r |= ((((a >> k) & 255) + ((b >> k) & 255) + ((c >> k) & 1)) >> 1) << k;
            return r;
        }
        case 0x14e: return u32(((u64(a) << 32) | b) >> (c & 31));
        case 0x14f: return u32(((u64(a) << 32) | b) >> (8 * (c & 3)));
        case 0x151: return u(std::fmin(std::fmin(fa, fb), fc));
        case 0x152: return u32(std::min({std::int32_t(a), std::int32_t(b), std::int32_t(c)}));
        case 0x153: return std::min({a, b, c});
        case 0x154: return u(std::fmax(std::fmax(fa, fb), fc));
        case 0x155: return u32(std::max({std::int32_t(a), std::int32_t(b), std::int32_t(c)}));
        case 0x156: return std::max({a, b, c});
        case 0x157: return u(med3(fa, fb, fc));
        case 0x158: {
            std::int32_t x = std::int32_t(a), y = std::int32_t(b), z = std::int32_t(c);
            return u32(std::max(std::min(x, y), std::min(std::max(x, y), z)));
        }
        case 0x159: return std::max(std::min(a, b), std::min(std::max(a, b), c));
        case 0x15a: case 0x15b: {
            u32 s = 0;
            for (int k = 0; k < 32; k += 8) {
                int x = int((a >> k) & 255), y = int((b >> k) & 255);
                s += u32(std::abs(x - y));
            }
            return op == 0x15a ? s + c : (s << 16) + c;
        }
        case 0x15c: {
            u32 s = 0;
            for (int k = 0; k < 32; k += 16) s += u32(std::abs(int((a >> k) & 0xffff) - int((b >> k) & 0xffff)));
            return s + c;
        }
        case 0x15d: return (a > b ? a - b : b - a) + c;
        case 0x15e: {
            u32 shift = (b & 3) * 8;
            u32 byte = u32(std::clamp(std::nearbyint(fa), 0.0f, 255.0f));
            return (c & ~(255u << shift)) | (byte << shift);
        }
        case 0x15f:  // div_fixup: the quotient, unless the division was a special case
            if (std::isnan(fb) || std::isnan(fc)) return u(NAN);
            if (fb == 0) return fc == 0 ? u(NAN) : u(std::copysign(INFINITY, fb) * (std::signbit(fc) ? -1.0f : 1.0f));
            if (std::isinf(fb)) return std::isinf(fc) ? u(NAN) : u(0.0f * fc * fb);
            if (std::isinf(fc)) return u(fc / fb);
            return a;
        case 0x169: return a * b;
        case 0x16a: return u32((u64(a) * b) >> 32);
        case 0x16b: return u32(std::int32_t(a) * std::int32_t(b));
        case 0x16c: return u32((i64(std::int32_t(a)) * std::int32_t(b)) >> 32);
        case 0x16f: return u(std::fma(fa, fb, fc));
        // VOP1
        case 0x180: return d;
        case 0x181: case 0x189: return a;
        case 0x185: return u(float(std::int32_t(a)));
        case 0x186: return u(float(a));
        case 0x187: return std::isnan(fa) ? 0 : fa <= 0 ? 0 : fa >= 4294967295.0f ? 0xffffffffu : u32(fa);
        case 0x188: return std::isnan(fa) ? 0 : fa <= -2147483648.0f ? 0x80000000u : fa >= 2147483647.0f ? 0x7fffffffu : u32(std::int32_t(fa));
        case 0x18a: return to_half(fa);
        case 0x18b: return u(half(a & 0xffff));
        case 0x18c: return u32(std::int32_t(std::floor(fa + 0.5f)));
        case 0x18d: return u32(std::int32_t(std::floor(fa)));
        case 0x18e: return u(float(std::int32_t(sext(a & 15, 4))) / 16.0f);
        case 0x191: case 0x192: case 0x193: case 0x194: return u(float((a >> (8 * (op - 0x191))) & 255));
        case 0x1a0: return u(std::fmin(fa - std::floor(fa), f(0x3f7fffff)));
        case 0x1a1: return u(std::trunc(fa));
        case 0x1a2: return u(std::ceil(fa));
        case 0x1a3: return u(std::nearbyint(fa));
        case 0x1a4: return u(std::floor(fa));
        case 0x1a5: case 0x1c6: return u(std::exp2(fa));
        case 0x1a6: case 0x1a7: case 0x1c5: return u(std::log2(fa));
        case 0x1a8: case 0x1a9: case 0x1aa: case 0x1ab: return u(1.0f / fa);
        case 0x1ac: case 0x1ad: case 0x1ae: return u(1.0f / std::sqrt(fa));
        case 0x1b3: return u(std::sqrt(fa));
        case 0x1b5: return u(std::sin(fa * 6.28318530718f));
        case 0x1b6: return u(std::cos(fa * 6.28318530718f));
        case 0x1b7: return ~a;
        case 0x1b8: {
            u32 r = 0;
            for (int k = 0; k < 32; k++) r |= ((a >> k) & 1) << (31 - k);
            return r;
        }
        case 0x1b9: return a ? u32(std::countl_zero(a)) : 0xffffffffu;
        case 0x1ba: return a ? u32(std::countr_zero(a)) : 0xffffffffu;
        case 0x1bb: {
            u32 x = a ^ u32(std::int32_t(a) >> 31);
            return x ? u32(std::countl_zero(x)) : 0xffffffffu;
        }
        case 0x1bf: {
            int e = 0;
            if (std::isfinite(fa) && fa != 0) std::frexp(fa, &e);
            return u32(e);
        }
        case 0x1c0: {
            int e;
            return std::isfinite(fa) ? u(std::frexp(fa, &e)) : a;
        }
        case 0x1c1: return d;  // v_clrexcp
        default: ok = false; return 0;
    }
}

bool is64(u32 op) {
    switch (op) {
        case 0x14c: case 0x160: case 0x161: case 0x162: case 0x163: case 0x164: case 0x165:
        case 0x166: case 0x167: case 0x168: case 0x16e: case 0x170: case 0x176: case 0x177:
        case 0x183: case 0x184: case 0x18f: case 0x190: case 0x195: case 0x196: case 0x197:
        case 0x198: case 0x199: case 0x19a: case 0x1af: case 0x1b0: case 0x1b1: case 0x1b2:
        case 0x1b4: case 0x1bc: case 0x1bd: case 0x1be:
            return true;
        default: return false;
    }
}

// A 64-bit op's widths, in dwords: the result, then the sources
void widths64(u32 op, int& d, int& a, int& b, int& c) {
    d = a = b = c = 2;
    switch (op) {
        case 0x161: case 0x162: case 0x163: case 0x168: b = 1; break;
        case 0x176: case 0x177: a = b = 1; break;
        case 0x183: case 0x18f: case 0x195: case 0x1bc: d = 1; break;
        case 0x184: case 0x190: case 0x196: a = 1; break;
        default: break;
    }
}

u64 alu64(u32 op, u64 a, u64 b, u64 c, bool& ok) {
    double da = dbl(a), db = dbl(b), dc = dbl(c);
    switch (op) {
        case 0x14c: return ubl(std::fma(da, db, dc));
        case 0x160:
            if (db == 0 || std::isnan(db) || std::isnan(dc) || std::isinf(db) || std::isinf(dc)) return ubl(dc / db);
            return a;
        case 0x161: return a << (b & 63);
        case 0x162: return a >> (b & 63);
        case 0x163: return u64(i64(a) >> (b & 63));
        case 0x164: return ubl(da + db);
        case 0x165: return ubl(da * db);
        case 0x166: return ubl(std::fmin(da, db));
        case 0x167: return ubl(std::fmax(da, db));
        case 0x168: return ubl(std::ldexp(da, std::int32_t(u32(b))));
        case 0x16e: return a;
        case 0x170: return ubl(std::fma(da, db, dc));
        case 0x176: return u64(u32(a)) * u32(b) + c;
        case 0x177: return u64(i64(std::int32_t(u32(a))) * std::int32_t(u32(b)) + i64(c));
        case 0x183: return std::isnan(da) ? 0 : u32(std::int32_t(std::clamp(da, -2147483648.0, 2147483647.0)));
        case 0x184: return ubl(double(std::int32_t(u32(a))));
        case 0x18f: return u(float(da));
        case 0x190: return ubl(double(f(u32(a))));
        case 0x195: return std::isnan(da) ? 0 : u32(std::clamp(da, 0.0, 4294967295.0));
        case 0x196: return ubl(double(u32(a)));
        case 0x197: return ubl(std::trunc(da));
        case 0x198: return ubl(std::ceil(da));
        case 0x199: return ubl(std::nearbyint(da));
        case 0x19a: return ubl(std::floor(da));
        case 0x1af: case 0x1b0: return ubl(1.0 / da);
        case 0x1b1: case 0x1b2: return ubl(1.0 / std::sqrt(da));
        case 0x1b4: return ubl(std::sqrt(da));
        case 0x1bc: {
            int e = 0;
            if (std::isfinite(da) && da != 0) std::frexp(da, &e);
            return u32(e);
        }
        case 0x1bd: {
            int e;
            return std::isfinite(da) ? ubl(std::frexp(da, &e)) : a;
        }
        case 0x1be: return ubl(da - std::floor(da));
        default: ok = false; return 0;
    }
}

bool fcmp(u32 cond, double a, double b) {
    bool un = std::isnan(a) || std::isnan(b);
    switch (cond) {
        case 0: return false;
        case 1: return a < b;
        case 2: return a == b;
        case 3: return a <= b;
        case 4: return a > b;
        case 5: return !un && a != b;
        case 6: return a >= b;
        case 7: return !un;
        case 8: return un;
        case 9: return !(a >= b);
        case 10: return un || a == b;
        case 11: return !(a > b);
        case 12: return !(a <= b);
        case 13: return !(a == b);
        case 14: return !(a < b);
        default: return true;
    }
}

template <class T>
bool icmp(u32 cond, T a, T b) {
    switch (cond) {
        case 0: return false;
        case 1: return a < b;
        case 2: return a == b;
        case 3: return a <= b;
        case 4: return a > b;
        case 5: return a != b;
        case 6: return a >= b;
        default: return true;
    }
}

bool fclass(double x, u32 mask) {
    int k;
    if (std::isnan(x)) k = 1;  // a quiet NaN (signalling ones aren't told apart here)
    else if (std::isinf(x)) k = x < 0 ? 2 : 9;
    else if (x == 0) k = std::signbit(x) ? 5 : 6;
    else if (std::fpclassify(x) == FP_SUBNORMAL) k = x < 0 ? 4 : 7;
    else k = x < 0 ? 3 : 8;
    return (mask >> k) & 1;
}

struct Vop {
    u32 op, vdst, sdst;
    u32 src[3];
    u32 lit = 0;
    u32 abs = 0, neg = 0, omod = 0;
    bool clamp = false;
};

// A vector op, in any of its encodings (VOP1, VOP2, VOPC as VOP3's numbering)
void vop(Wave& w, const Vop& o) {
    u64 exec = w.exec();
    u32 op = o.op;
    u32 andm[3], xorm[3];
    for (int k = 0; k < 3; k++) {
        andm[k] = (o.abs >> k) & 1 ? 0x7fffffffu : 0xffffffffu;
        xorm[k] = (o.neg >> k) & 1 ? 0x80000000u : 0;
    }
    if (op < 0x100) {  // comparisons
        u32 group = op >> 4, cond = op & 15;
        bool x = group & 1;
        u64 r = 0;
        bool wide = group == 2 || group == 3 || group == 6 || group == 7 || group == 10 || group == 11 || group >= 14;
        if (group >= 8 && cond > 8) {
            note(std::format("vector compare {:#x}", op));
            return;
        }
        bool klass = group >= 8 && group < 12 && cond == 8;
        if (wide && !klass) {
            bool fl = group < 8;
            Src64 a = vsrc64(w, o.src[0], o.lit, fl), b = vsrc64(w, o.src[1], o.lit, fl);
            for (int l = 0; l < LANES; l++) {
                if (!((exec >> l) & 1)) continue;
                u64 x0 = a[l], x1 = b[l];
                if (fl) {
                    if ((o.abs & 1)) x0 &= ~(1ull << 63);
                    if ((o.abs & 2)) x1 &= ~(1ull << 63);
                    if ((o.neg & 1)) x0 ^= 1ull << 63;
                    if ((o.neg & 2)) x1 ^= 1ull << 63;
                }
                bool t = fl ? fcmp(cond, dbl(x0), dbl(x1)) : group < 12 ? icmp(cond, i64(x0), i64(x1)) : icmp(cond, x0, x1);
                r |= u64(t) << l;
            }
        } else {
            Src a = vsrc(w, o.src[0], o.lit), b = vsrc(w, o.src[1], o.lit);
            Src64 a64 = klass && group >= 10 ? vsrc64(w, o.src[0], o.lit, true) : Src64{};
            for (int l = 0; l < LANES; l++) {
                if (!((exec >> l) & 1)) continue;
                u32 x0 = (a[l] & andm[0]) ^ xorm[0], x1 = (b[l] & andm[1]) ^ xorm[1];
                bool t;
                if (klass) t = fclass(group >= 10 ? dbl(a64[l]) : double(f(x0)), x1);
                else if (group < 8) t = fcmp(cond, f(x0), f(x1));
                else if (group < 12) t = icmp(cond, std::int32_t(x0), std::int32_t(x1));
                else t = icmp(cond, x0, x1);
                r |= u64(t) << l;
            }
        }
        sset64(w, o.sdst, r);
        if (x) w.set_exec(r);
        return;
    }
    if (is64(op)) {
        int dw, aw, bw, cw;
        widths64(op, dw, aw, bw, cw);
        bool fl = op != 0x161 && op != 0x162 && op != 0x163 && op != 0x176 && op != 0x177;
        auto get = [&](int k, int width, int l, const Src& s32, const Src64& s64) -> u64 {
            u64 v = width == 2 ? s64[l] : s32[l];
            if (fl && width == 2) {
                if ((o.abs >> k) & 1) v &= ~(1ull << 63);
                if ((o.neg >> k) & 1) v ^= 1ull << 63;
            } else if (fl) {
                v = (u32(v) & andm[k]) ^ xorm[k];
            }
            return v;
        };
        Src a32 = vsrc(w, o.src[0], o.lit), b32 = vsrc(w, o.src[1], o.lit), c32 = vsrc(w, o.src[2], o.lit);
        Src64 a64 = vsrc64(w, o.src[0], o.lit, fl), b64 = vsrc64(w, o.src[1], o.lit, fl), c64 = vsrc64(w, o.src[2], o.lit, fl);
        u64 carry = 0;
        bool ok = true;
        for (int l = 0; l < LANES && ok; l++) {
            if (!((exec >> l) & 1)) continue;
            u64 x = get(0, aw, l, a32, a64), y = get(1, bw, l, b32, b64), z = get(2, cw, l, c32, c64);
            u64 r = alu64(op, x, y, z, ok);
            if (op == 0x176) carry |= u64((r < z)) << l;
            w.v[o.vdst][size_t(l)] = u32(r);
            if (dw == 2) w.v[(o.vdst + 1) & 255][size_t(l)] = u32(r >> 32);
        }
        if (!ok) note(std::format("vector op {:#x}", op));
        if (op == 0x16e || op == 0x176 || op == 0x177) sset64(w, o.sdst, op == 0x176 ? carry : 0);
        return;
    }
    Src a = vsrc(w, o.src[0], o.lit), b = vsrc(w, o.src[1], o.lit), c = vsrc(w, o.src[2], o.lit);
    Vec& d = w.v[o.vdst];
    switch (op) {
        case 0x100: {  // v_cndmask_b32: src2 is the lane mask
            u64 m = sget64(w, o.src[2], o.lit);
            for (int l = 0; l < LANES; l++)
                if ((exec >> l) & 1) d[size_t(l)] = (m >> l) & 1 ? (b[l] & andm[1]) ^ xorm[1] : (a[l] & andm[0]) ^ xorm[0];
            return;
        }
        case 0x101:  // v_readlane_b32 sdst, vsrc, lane
            sset(w, o.sdst, o.src[0] >= 256 ? w.v[o.src[0] - 256][sget(w, o.src[1], o.lit) & 63] : sget(w, o.src[0], o.lit));
            return;
        case 0x102:  // v_writelane_b32
            d[sget(w, o.src[1], o.lit) & 63] = sget(w, o.src[0], o.lit);
            return;
        case 0x182: {  // v_readfirstlane_b32
            int l = exec ? std::countr_zero(exec) : 0;
            sset(w, o.sdst, a[l]);
            return;
        }
        case 0x123: case 0x124: {  // v_mbcnt_lo / hi: set bits of the mask in the lanes below this one
            for (int l = 0; l < LANES; l++) {
                if (!((exec >> l) & 1)) continue;
                u32 m = op == 0x123 ? (l >= 32 ? 0xffffffffu : (1u << l) - 1) : (l <= 32 ? 0 : (1u << (l - 32)) - 1);
                d[size_t(l)] = u32(std::popcount(a[l] & m)) + b[l];
            }
            return;
        }
        case 0x1c2: case 0x1c3: case 0x1c4: {  // v_movreld, movrels, movrelsd: indexed by m0
            u32 m0 = w.s[124];
            for (int l = 0; l < LANES; l++) {
                if (!((exec >> l) & 1)) continue;
                u32 v = op == 0x1c2 ? a[l] : w.v[(o.src[0] - 256 + m0) & 255][size_t(l)];
                w.v[(op == 0x1c3 ? o.vdst : o.vdst + m0) & 255][size_t(l)] = v;
            }
            return;
        }
        case 0x125: case 0x126: case 0x127: case 0x128: case 0x129: case 0x12a: {  // with a carry
            u64 cin = op >= 0x128 ? sget64(w, o.src[2], o.lit) : 0, cout = 0;
            for (int l = 0; l < LANES; l++) {
                if (!((exec >> l) & 1)) continue;
                u64 x = a[l], y = b[l], ci = (cin >> l) & 1, r;
                bool carry;
                switch (op) {
                    case 0x125: r = x + y; carry = r >> 32; break;
                    case 0x126: r = x - y; carry = y > x; break;
                    case 0x127: r = y - x; carry = x > y; break;
                    case 0x128: r = x + y + ci; carry = r >> 32; break;
                    case 0x129: r = x - y - ci; carry = y + ci > x; break;
                    default: r = y - x - ci; carry = x + ci > y; break;
                }
                d[size_t(l)] = u32(r);
                cout |= u64(carry) << l;
            }
            sset64(w, o.sdst, cout);
            return;
        }
        case 0x16d:  // v_div_scale_f32: no scaling (the quotient's range is left to v_div_fixup)
            for (int l = 0; l < LANES; l++)
                if ((exec >> l) & 1) d[size_t(l)] = (a[l] & andm[0]) ^ xorm[0];
            sset64(w, o.sdst, 0);
            return;
        default: break;
    }
    bool fr = float_result(op);
    // the usual ops: straight loops over the lanes, the sources gathered first
    {
        alignas(32) u32 A[LANES], B[LANES], C[LANES], R[LANES];
        auto gather = [&](const Src& s, int k, u32* out) {
            if (s.v)
                for (int l = 0; l < LANES; l++) out[l] = ((*s.v)[size_t(l)] & andm[k]) ^ xorm[k];
            else
                for (int l = 0; l < LANES; l++) out[l] = (s.k & andm[k]) ^ xorm[k];
        };
        bool fast = true;
        auto F = [](u32 x) { return f(x); };
        switch (op) {
            case 0x103: case 0x104: case 0x105: case 0x108: case 0x10f: case 0x110: case 0x11b: case 0x11c:
            case 0x11d: case 0x115: case 0x116: case 0x119: case 0x11a: case 0x12f:
                gather(a, 0, A), gather(b, 1, B);
                break;
            case 0x11f: gather(a, 0, A), gather(b, 1, B); for (int l = 0; l < LANES; l++) C[l] = d[size_t(l)]; break;
            case 0x141: case 0x14b: gather(a, 0, A), gather(b, 1, B), gather(c, 2, C); break;
            case 0x181: case 0x185: case 0x186: case 0x187: case 0x188: case 0x1a4: case 0x1a0: gather(a, 0, A); break;
            default: fast = false;
        }
        if (fast) {
            switch (op) {
                case 0x103: for (int l = 0; l < LANES; l++) R[l] = u(F(A[l]) + F(B[l])); break;
                case 0x104: for (int l = 0; l < LANES; l++) R[l] = u(F(A[l]) - F(B[l])); break;
                case 0x105: for (int l = 0; l < LANES; l++) R[l] = u(F(B[l]) - F(A[l])); break;
                case 0x108: for (int l = 0; l < LANES; l++) R[l] = u(F(A[l]) * F(B[l])); break;
                case 0x10f: for (int l = 0; l < LANES; l++) R[l] = u(std::fmin(F(A[l]), F(B[l]))); break;
                case 0x110: for (int l = 0; l < LANES; l++) R[l] = u(std::fmax(F(A[l]), F(B[l]))); break;
                case 0x11b: for (int l = 0; l < LANES; l++) R[l] = A[l] & B[l]; break;
                case 0x11c: for (int l = 0; l < LANES; l++) R[l] = A[l] | B[l]; break;
                case 0x11d: for (int l = 0; l < LANES; l++) R[l] = A[l] ^ B[l]; break;
                case 0x115: for (int l = 0; l < LANES; l++) R[l] = A[l] >> (B[l] & 31); break;
                case 0x116: for (int l = 0; l < LANES; l++) R[l] = B[l] >> (A[l] & 31); break;
                case 0x119: for (int l = 0; l < LANES; l++) R[l] = A[l] << (B[l] & 31); break;
                case 0x11a: for (int l = 0; l < LANES; l++) R[l] = B[l] << (A[l] & 31); break;
                case 0x12f: for (int l = 0; l < LANES; l++) R[l] = to_half_mode(F(A[l]), true) | (to_half_mode(F(B[l]), true) << 16); break;
                case 0x11f: case 0x141: for (int l = 0; l < LANES; l++) R[l] = u(F(A[l]) * F(B[l]) + F(C[l])); break;
                case 0x14b: for (int l = 0; l < LANES; l++) R[l] = u(std::fma(F(A[l]), F(B[l]), F(C[l]))); break;
                case 0x181: for (int l = 0; l < LANES; l++) R[l] = A[l]; break;
                case 0x185: for (int l = 0; l < LANES; l++) R[l] = u(float(std::int32_t(A[l]))); break;
                case 0x186: for (int l = 0; l < LANES; l++) R[l] = u(float(A[l])); break;
                case 0x1a4: for (int l = 0; l < LANES; l++) R[l] = u(std::floor(F(A[l]))); break;
                case 0x1a0: for (int l = 0; l < LANES; l++) R[l] = u(std::fmin(F(A[l]) - std::floor(F(A[l])), f(0x3f7fffff))); break;
                default: {  // the conversions to integers, with their clamping
                    bool ok2 = true;
                    for (int l = 0; l < LANES; l++) R[l] = alu(op, A[l], 0, 0, 0, ok2);
                }
            }
            if (fr && (o.omod || o.clamp))
                for (int l = 0; l < LANES; l++) {
                    float x = f(R[l]);
                    if (o.omod == 1) x *= 2;
                    if (o.omod == 2) x *= 4;
                    if (o.omod == 3) x *= 0.5f;
                    if (o.clamp) x = std::clamp(x, 0.0f, 1.0f);
                    R[l] = u(x);
                }
            if (exec == ~0ull)
                for (int l = 0; l < LANES; l++) d[size_t(l)] = R[l];
            else
                for (int l = 0; l < LANES; l++)
                    if ((exec >> l) & 1) d[size_t(l)] = R[l];
            return;
        }
    }
    bool ok = true;
    for (int l = 0; l < LANES; l++) {
        if (!((exec >> l) & 1)) continue;
        u32 r = alu(op, (a[l] & andm[0]) ^ xorm[0], (b[l] & andm[1]) ^ xorm[1], (c[l] & andm[2]) ^ xorm[2], d[size_t(l)], ok);
        if (!ok) break;
        if (fr && (o.omod || o.clamp)) {
            float x = f(r);
            if (o.omod == 1) x *= 2;
            if (o.omod == 2) x *= 4;
            if (o.omod == 3) x *= 0.5f;
            if (o.clamp) x = std::clamp(x, 0.0f, 1.0f);
            r = u(x);
        }
        d[size_t(l)] = r;
    }
    if (!ok) note(std::format("vector op {:#x}", op));
}

// ---- scalar ALU ----

bool sop2(Wave& w, u32 op, u32 dst, u32 s0, u32 s1, u32 lit) {
    bool wide = (op >= 11 && op <= 29 && (op & 1)) || op == 31 || op == 33 || op == 35 || op == 37 || op == 41 || op == 42;
    if (op == 11) wide = true;
    if (wide) {
        u64 a = sget64(w, s0, lit), b = sget64(w, s1, lit), r;
        switch (op) {
            case 11: r = w.scc ? a : b; sset64(w, dst, r); return true;
            case 15: r = a & b; break;
            case 17: r = a | b; break;
            case 19: r = a ^ b; break;
            case 21: r = a & ~b; break;
            case 23: r = a | ~b; break;
            case 25: r = ~(a & b); break;
            case 27: r = ~(a | b); break;
            case 29: r = ~(a ^ b); break;
            case 31: r = a << (b & 63); break;
            case 33: r = a >> (b & 63); break;
            case 35: r = u64(i64(a) >> (b & 63)); break;
            case 37: r = ((1ull << (a & 63)) - 1) << (b & 63); sset64(w, dst, r); return true;
            case 41: case 42: {
                u32 off = u32(b) & 63, width = (u32(b) >> 16) & 0x7f;
                r = width ? (a >> off) & (width >= 64 ? ~0ull : (1ull << width) - 1) : 0;
                if (op == 42 && width && width < 64 && ((r >> (width - 1)) & 1)) r |= ~0ull << width;
                break;
            }
            default: return false;
        }
        sset64(w, dst, r);
        w.scc = r != 0;
        return true;
    }
    u32 a = sget(w, s0, lit), b = sget(w, s1, lit), r;
    switch (op) {
        case 0: { u64 s = u64(a) + b; r = u32(s); w.scc = s >> 32; break; }
        case 1: r = a - b; w.scc = b > a; break;
        case 2: r = a + b; w.scc = ((~(a ^ b)) & (a ^ r)) >> 31; break;
        case 3: r = a - b; w.scc = ((a ^ b) & (a ^ r)) >> 31; break;
        case 4: { u64 s = u64(a) + b + w.scc; r = u32(s); w.scc = s >> 32; break; }
        case 5: { u64 t = u64(b) + w.scc; r = u32(a - t); w.scc = t > a; break; }
        case 6: w.scc = std::int32_t(a) < std::int32_t(b); r = w.scc ? a : b; break;
        case 7: w.scc = a < b; r = w.scc ? a : b; break;
        case 8: w.scc = std::int32_t(a) > std::int32_t(b); r = w.scc ? a : b; break;
        case 9: w.scc = a > b; r = w.scc ? a : b; break;
        case 10: r = w.scc ? a : b; break;
        case 14: r = a & b; w.scc = r != 0; break;
        case 16: r = a | b; w.scc = r != 0; break;
        case 18: r = a ^ b; w.scc = r != 0; break;
        case 20: r = a & ~b; w.scc = r != 0; break;
        case 22: r = a | ~b; w.scc = r != 0; break;
        case 24: r = ~(a & b); w.scc = r != 0; break;
        case 26: r = ~(a | b); w.scc = r != 0; break;
        case 28: r = ~(a ^ b); w.scc = r != 0; break;
        case 30: r = a << (b & 31); w.scc = r != 0; break;
        case 32: r = a >> (b & 31); w.scc = r != 0; break;
        case 34: r = u32(std::int32_t(a) >> (b & 31)); w.scc = r != 0; break;
        case 36: r = ((1u << (a & 31)) - 1) << (b & 31); break;
        case 38: r = a * b; break;
        case 39: case 40: {
            u32 off = b & 31, width = (b >> 16) & 0x7f;
            r = width ? (a >> off) & (width >= 32 ? ~0u : (1u << width) - 1) : 0;
            if (op == 40 && width && width < 32) r = sext(r, int(width));
            w.scc = r != 0;
            break;
        }
        case 44: r = u32(std::abs(std::int32_t(a - b))); w.scc = r != 0; break;
        default: return false;
    }
    sset(w, dst, r);
    return true;
}

bool sopk(Wave& w, u32 op, u32 dst, u32 imm) {
    u32 si = sext(imm, 16);
    u32 d = sget(w, dst, 0);
    switch (op) {
        case 0: sset(w, dst, si); return true;
        case 2: if (w.scc) sset(w, dst, si); return true;
        case 3: w.scc = std::int32_t(d) == std::int32_t(si); return true;
        case 4: w.scc = std::int32_t(d) != std::int32_t(si); return true;
        case 5: w.scc = std::int32_t(d) > std::int32_t(si); return true;
        case 6: w.scc = std::int32_t(d) >= std::int32_t(si); return true;
        case 7: w.scc = std::int32_t(d) < std::int32_t(si); return true;
        case 8: w.scc = std::int32_t(d) <= std::int32_t(si); return true;
        case 9: w.scc = d == imm; return true;
        case 10: w.scc = d != imm; return true;
        case 11: w.scc = d > imm; return true;
        case 12: w.scc = d >= imm; return true;
        case 13: w.scc = d < imm; return true;
        case 14: w.scc = d <= imm; return true;
        case 15: { u32 r = d + si; w.scc = ((~(d ^ si)) & (d ^ r)) >> 31; sset(w, dst, r); return true; }
        case 16: sset(w, dst, d * si); return true;
        case 18: sset(w, dst, 0); note("s_getreg_b32 (read as 0)"); return true;
        case 19: case 21: return true;  // s_setreg: the wave's modes (rounding, denormals), not kept
        default: return false;
    }
}

bool sop1(Wave& w, u32 op, u32 dst, u32 s0, u32 lit, u64& pc, u64 next) {
    u32 a = sget(w, s0, lit);
    u64 a64 = sget64(w, s0, lit);
    switch (op) {
        case 3: sset(w, dst, a); return true;
        case 4: sset64(w, dst, a64); return true;
        case 5: if (w.scc) sset(w, dst, a); return true;
        case 6: if (w.scc) sset64(w, dst, a64); return true;
        case 7: { u32 r = ~a; sset(w, dst, r); w.scc = r != 0; return true; }
        case 8: { u64 r = ~a64; sset64(w, dst, r); w.scc = r != 0; return true; }
        case 9: case 10: {
            u64 r = 0;
            for (int k = 0; k < 64; k += 4)
                if ((a64 >> k) & 15) r |= 15ull << k;
            if (op == 9) r &= 0xffffffff;
            if (op == 9) sset(w, dst, u32(r)); else sset64(w, dst, r);
            w.scc = r != 0;
            return true;
        }
        case 11: { u32 r = 0; for (int k = 0; k < 32; k++) r |= ((a >> k) & 1) << (31 - k); sset(w, dst, r); return true; }
        case 13: case 14: case 15: case 16: {  // s_bcnt0 / s_bcnt1, of 32 or 64 bits
            bool wide = op == 14 || op == 16;
            int ones = wide ? std::popcount(a64) : std::popcount(a);
            int n = op <= 14 ? (wide ? 64 : 32) - ones : ones;
            sset(w, dst, u32(n));
            w.scc = n != 0;
            return true;
        }
        case 17: case 18: {  // first 0 bit
            u64 v = op == 17 ? (~a & 0xffffffffull) : ~a64;
            sset(w, dst, v ? u32(std::countr_zero(v)) : 0xffffffffu);
            return true;
        }
        case 19: case 20: {
            u64 v = op == 19 ? a : a64;
            sset(w, dst, v ? u32(std::countr_zero(v)) : 0xffffffffu);
            return true;
        }
        case 21: sset(w, dst, a ? u32(std::countl_zero(a)) : 0xffffffffu); return true;
        case 22: sset(w, dst, a64 ? u32(std::countl_zero(a64)) : 0xffffffffu); return true;
        case 23: { u32 x = a ^ u32(std::int32_t(a) >> 31); sset(w, dst, x ? u32(std::countl_zero(x)) : 0xffffffffu); return true; }
        case 25: sset(w, dst, sext(a & 0xff, 8)); return true;
        case 26: sset(w, dst, sext(a & 0xffff, 16)); return true;
        case 27: sset(w, dst, sget(w, dst, 0) & ~(1u << (a & 31))); return true;
        case 28: sset64(w, dst, sget64(w, dst, 0) & ~(1ull << (a & 63))); return true;
        case 29: sset(w, dst, sget(w, dst, 0) | (1u << (a & 31))); return true;
        case 30: sset64(w, dst, sget64(w, dst, 0) | (1ull << (a & 63))); return true;
        case 31: sset64(w, dst, next); return true;
        case 32: pc = a64; return true;
        case 33: sset64(w, dst, next); pc = a64; return true;
        case 36: case 37: case 38: case 39: case 40: case 41: case 42: case 43: {
            u64 e = w.exec(), r;
            switch (op) {
                case 36: r = a64 & e; break;
                case 37: r = a64 | e; break;
                case 38: r = a64 ^ e; break;
                case 39: r = a64 & ~e; break;
                case 40: r = a64 | ~e; break;
                case 41: r = ~(a64 & e); break;
                case 42: r = ~(a64 | e); break;
                default: r = ~(a64 ^ e); break;
            }
            sset64(w, dst, e);
            w.set_exec(r);
            w.scc = r != 0;
            return true;
        }
        case 44: case 45: {
            u64 r = 0;
            for (int k = 0; k < 16; k++)
                if ((a64 >> (4 * k)) & 15) r |= 1ull << k;
            if (op == 44) sset(w, dst, u32(r)); else sset64(w, dst, r);
            w.scc = r != 0;
            return true;
        }
        case 46: sset(w, dst, w.s[(s0 + w.s[124]) & 127]); return true;
        case 47: sset64(w, dst, sget64(w, (s0 + w.s[124]) & 127, 0)); return true;
        case 48: sset(w, (dst + w.s[124]) & 127, a); return true;
        case 49: sset64(w, (dst + w.s[124]) & 127, a64); return true;
        case 52: { u32 r = u32(std::abs(std::int32_t(a))); sset(w, dst, r); w.scc = r != 0; return true; }
        case 53: sset(w, dst, a); return true;
        default: return false;
    }
}

bool sopc(Wave& w, u32 op, u32 s0, u32 s1, u32 lit) {
    u32 a = sget(w, s0, lit), b = sget(w, s1, lit);
    if (op <= 5) { w.scc = icmp(op == 0 ? 2 : op == 1 ? 5 : op == 2 ? 4 : op == 3 ? 6 : op == 4 ? 1 : 3, std::int32_t(a), std::int32_t(b)); return true; }
    if (op <= 11) { u32 k = op - 6; w.scc = icmp(k == 0 ? 2 : k == 1 ? 5 : k == 2 ? 4 : k == 3 ? 6 : k == 4 ? 1 : 3, a, b); return true; }
    switch (op) {
        case 12: w.scc = !((a >> (b & 31)) & 1); return true;
        case 13: w.scc = (a >> (b & 31)) & 1; return true;
        case 14: w.scc = !((sget64(w, s0, lit) >> (b & 63)) & 1); return true;
        case 15: w.scc = (sget64(w, s0, lit) >> (b & 63)) & 1; return true;
        case 16: return true;
        default: return false;
    }
}

// ---- memory ----

template <class T>
T load(const Memory& mem, u64 at) {
    T v{};
    if (mem.ok(at, sizeof(T))) std::memcpy(&v, reinterpret_cast<const void*>(at), sizeof(T));
    return v;
}
template <class T>
void store(const Memory& mem, u64 at, T v) {
    if (mem.ok(at, sizeof(T))) std::memcpy(reinterpret_cast<void*>(at), &v, sizeof(T));
}

struct Buffer {  // a buffer descriptor (V#)
    u64 base;
    u32 stride, records, sel[4], nfmt, dfmt;
    bool add_tid, swizzled;
    explicit Buffer(const u32* v) {
        base = v[0] | (u64(v[1] & 0xfff) << 32);  // 44 bits: the 4 above are something else
        stride = (v[1] >> 16) & 0x3fff;
        swizzled = (v[1] >> 31) & 1;
        records = v[2];
        for (int c = 0; c < 4; c++) sel[c] = (v[3] >> (3 * c)) & 7;
        nfmt = (v[3] >> 12) & 7;
        dfmt = (v[3] >> 15) & 15;
        add_tid = (v[3] >> 23) & 1;
    }
};

void smrd(Wave& w, const Memory& mem, u32 op, u32 dst, u32 base, bool imm, u32 off, u32 lit) {
    if (op == 0x1e) {  // s_memtime
        sset64(w, dst, u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - M.t0).count() / 10));
        return;
    }
    if (op == 0x1d || op == 0x1f) return;  // cache invalidations
    u32 count = 1u << (op & 7);
    bool buffer = op >= 8;
    u64 offset = imm ? u64(off) * 4 : off == 255 ? lit : sget(w, off, lit);
    u64 at = buffer ? Buffer(&w.s[base]).base + offset : (sget64(w, base, 0) & ~3ull) + offset;
    for (u32 k = 0; k < count; k++) sset(w, dst + k, load<u32>(mem, at + 4 * k));
}

void buffer_op(Wave& w, const Memory& mem, u32 w0, u32 w1) {
    bool typed = (w0 >> 26) == 0x3a;
    u32 op = typed ? (w0 >> 16) & 7 : (w0 >> 18) & 0x7f;
    bool offen = (w0 >> 12) & 1, idxen = (w0 >> 13) & 1, addr64 = (w0 >> 15) & 1;
    u32 inst_off = w0 & 0xfff, vaddr = w1 & 0xff, vdata = (w1 >> 8) & 0xff, srsrc = ((w1 >> 16) & 0x1f) * 4;
    u32 soffset = sget(w, w1 >> 24, 0);
    Buffer b(&w.s[srsrc]);
    if (b.swizzled) note("swizzled buffers (read as plain)");
    if (!typed && op >= 112) return;  // buffer_wbinvl1: a cache
    if (!typed && ((w0 >> 16) & 1)) {
        note("buffer loads into the LDS");
        return;
    }
    // what the op moves: a format, or raw bytes
    bool format = typed || op <= 7, is_store = typed ? op >= 4 : (op >= 4 && op <= 7) || (op >= 24 && op <= 31);
    u32 dfmt = typed ? (w0 >> 19) & 0xf : b.dfmt, nfmt = typed ? (w0 >> 23) & 7 : b.nfmt;
    u32 comps = format ? (op & 3) + 1 : 0, bytes = 0;
    bool atomic = !typed && op >= 48 && op < 112;
    if (!format && !atomic) {
        switch (op) {
            case 8: case 9: case 24: bytes = 1; break;
            case 10: case 11: case 26: bytes = 2; break;
            case 12: case 28: bytes = 4; break;
            case 13: case 29: bytes = 8; break;
            case 14: case 30: bytes = 16; break;
            case 15: case 31: bytes = 12; break;
            default: note(std::format("buffer op {}", op)); return;
        }
    }
    Layout l = layout(dfmt);
    if (format && !l.count) {
        if (!is_store)
            for (u32 c = 0; c < comps; c++)
                for (int ln = 0; ln < LANES; ln++)
                    if ((w.exec() >> ln) & 1) w.v[(vdata + c) & 255][size_t(ln)] = 0;
        return;
    }
    u64 exec = w.exec();
    for (int ln = 0; ln < LANES; ln++) {
        if (!((exec >> ln) & 1)) continue;
        u64 at;
        bool in_range = true;
        if (addr64) {
            at = b.base + (w.v[vaddr][size_t(ln)] | (u64(w.v[(vaddr + 1) & 255][size_t(ln)]) << 32)) + inst_off + soffset;
        } else {
            u32 index = idxen ? w.v[vaddr][size_t(ln)] : 0;
            if (b.add_tid) index += u32(ln);
            u32 voff = offen ? w.v[(vaddr + (idxen ? 1 : 0)) & 255][size_t(ln)] : 0;
            in_range = b.stride == 0 ? u64(voff) + inst_off < b.records : index < b.records;
            at = b.base + soffset + inst_off + voff + u64(b.stride) * index;
        }
        if (atomic) {
            u32 aop = op >= 80 ? op - 80 : op - 48;
            if (op >= 80 || !in_range || !mem.ok(at, 4)) {
                if (op >= 80) note("64-bit buffer atomics");
                continue;
            }
            auto* p = reinterpret_cast<std::atomic<u32>*>(at);
            u32 data = w.v[vdata][size_t(ln)], old = p->load(), nv;
            switch (aop) {
                case 0: nv = data; break;
                case 1: nv = old == w.v[(vdata + 1) & 255][size_t(ln)] ? data : old; break;
                case 2: nv = old + data; break;
                case 3: nv = old - data; break;
                case 5: nv = u32(std::min(std::int32_t(old), std::int32_t(data))); break;
                case 6: nv = std::min(old, data); break;
                case 7: nv = u32(std::max(std::int32_t(old), std::int32_t(data))); break;
                case 8: nv = std::max(old, data); break;
                case 9: nv = old & data; break;
                case 10: nv = old | data; break;
                case 11: nv = old ^ data; break;
                case 12: nv = old >= data ? 0 : old + 1; break;
                case 13: nv = old == 0 || old > data ? data : old - 1; break;
                default: note(std::format("buffer atomic {}", op)); continue;
            }
            p->store(nv);
            if ((w0 >> 14) & 1) w.v[vdata][size_t(ln)] = old;  // glc: the old value back
            continue;
        }
        if (format) {
            if (is_store) {
                u32 in[4] = {0, 0, 0, 0};
                for (u32 c = 0; c < comps; c++) in[c] = w.v[(vdata + c) & 255][size_t(ln)];
                if (in_range && mem.ok(at, u64(l.bytes))) pack(dfmt, nfmt, in, reinterpret_cast<unsigned char*>(at));
            } else {
                u32 raw[4] = {0, 0, 0, is_int(nfmt) ? 1u : u(1.0f)}, out[4];
                if (in_range && mem.ok(at, u64(l.bytes))) unpack(dfmt, nfmt, reinterpret_cast<const unsigned char*>(at), raw);
                swizzle(b.sel, is_int(nfmt), raw, out);
                for (u32 c = 0; c < comps; c++) w.v[(vdata + c) & 255][size_t(ln)] = out[c];
            }
            continue;
        }
        if (is_store) {
            if (!in_range || !mem.ok(at, bytes)) continue;
            if (bytes < 4) {
                u32 v = w.v[vdata][size_t(ln)];
                std::memcpy(reinterpret_cast<void*>(at), &v, bytes);
            } else {
                for (u32 k = 0; k < bytes / 4; k++) store<u32>(mem, at + 4 * k, w.v[(vdata + k) & 255][size_t(ln)]);
            }
        } else {
            bool ok = in_range && mem.ok(at, bytes);
            if (bytes < 4) {
                u32 v = 0;
                if (ok) std::memcpy(&v, reinterpret_cast<const void*>(at), bytes);
                if (op == 9) v = sext(v, 8);
                if (op == 11) v = sext(v, 16);
                w.v[vdata][size_t(ln)] = v;
            } else {
                for (u32 k = 0; k < bytes / 4; k++) w.v[(vdata + k) & 255][size_t(ln)] = ok ? load<u32>(mem, at + 4 * k) : 0;
            }
        }
    }
}

int coord_count(u32 type) {
    switch (type) {
        case 8: return 1;
        case 10: case 11: case 13: case 14: return 3;
        case 15: return 4;
        default: return 2;  // 2D, 1D array
    }
}
int deriv_dims(u32 type) { return type == 8 || type == 12 ? 1 : type == 10 ? 3 : 2; }

void image_op(Wave& w, const Memory& mem, u32 w0, u32 w1) {
    u32 op = (w0 >> 18) & 0x7f, dmask = (w0 >> 8) & 0xf;
    bool unorm = (w0 >> 12) & 1;
    u32 vaddr = w1 & 0xff, vdata = (w1 >> 8) & 0xff, srsrc = ((w1 >> 16) & 0x1f) * 4, ssamp = ((w1 >> 21) & 0x1f) * 4;
    Image t(&w.s[srsrc]);
    t.prepare(mem);
    u64 exec = w.exec();
    auto vaddr_at = [&](int k, int ln) { return w.v[(vaddr + u32(k)) & 255][size_t(ln)]; };
    auto write_out = [&](int ln, const u32 v[4]) {
        u32 r = 0;
        for (int c = 0; c < 4; c++)
            if ((dmask >> c) & 1) w.v[(vdata + r++) & 255][size_t(ln)] = v[c];
    };
    int n = coord_count(t.type);
    if (op <= 1 || (op >= 8 && op <= 9)) {  // image_load(_mip), image_store(_mip): integer coordinates
        bool store_op = op >= 8;
        Layout l = layout(t.dfmt);
        for (int ln = 0; ln < LANES; ln++) {
            if (!((exec >> ln) & 1)) continue;
            u32 x = vaddr_at(0, ln), y = n > 1 ? vaddr_at(1, ln) : 0, z = n > 2 ? vaddr_at(2, ln) : 0;
            if ((op & 1) && vaddr_at(n, ln) != 0) note("images' mip levels past the first");
            if (t.type == 12) z = y, y = 0;
            if (store_op) {
                if (x >= t.width || y >= t.height || !l.count || is_block(t.dfmt)) continue;
                u32 in[4] = {0, 0, 0, 0}, r = 0;
                for (int c = 0; c < 4; c++)
                    if ((dmask >> c) & 1) in[c] = w.v[(vdata + r++) & 255][size_t(ln)];
                u64 at = element_address(t.base, t.tiling, u32(l.bytes), std::max(t.pitch, t.width), t.height, x, y,
                                         t.type == 10 ? z : z + t.base_array);
                if (at && mem.ok(at, u64(l.bytes))) pack(t.dfmt, t.nfmt, in, reinterpret_cast<unsigned char*>(at));
            } else {
                u32 raw[4], out[4];
                texel(t, mem, int(x), int(y), t.type == 10 ? z : z + t.base_array, raw);
                swizzle(t.sel, is_int(t.nfmt), raw, out);
                write_out(ln, out);
            }
        }
        return;
    }
    if (op == 14) {  // image_get_resinfo
        for (int ln = 0; ln < LANES; ln++) {
            if (!((exec >> ln) & 1)) continue;
            u32 v[4] = {t.width, t.height, t.array() ? t.last_array - t.base_array + 1 : t.depth, 1};
            write_out(ln, v);
        }
        return;
    }
    bool sample = op >= 32 && op < 96, gather = op >= 64 && op < 96, cd = op >= 104 && op <= 111;
    if (!sample && !cd) {
        note(std::format("image op {}", op));
        return;
    }
    u32 k = sample ? (op - 32) & 31 : 0;
    bool offset = sample ? (k & 16) : op >= 108;
    u32 m = k & 7;
    bool compare = sample ? (k & 8) : op == 106 || op == 107 || op == 110 || op == 111;
    bool bias = m == 5 || m == 6, derivs = m == 2 || m == 3 || cd, lod_given = m == 4, zero_lod = m == 7;
    if (compare) note("depth-compare sampling (done without the compare)");
    Sampler s(&w.s[ssamp]);
    bool normalized = !unorm && !s.unnormalized;
    // the address registers, in order: offset, bias, z-compare, derivatives, coordinates, lod
    int at = 0;
    int r_offset = offset ? at++ : -1;
    int r_bias = bias ? at++ : -1;
    if (compare) at++;
    int r_derivs = derivs ? at : -1;
    if (derivs) at += 2 * deriv_dims(t.type);
    int r_coords = at;
    at += n;
    int r_lod = lod_given ? at : -1;
    float W = float(t.width), H = float(t.height);
    // implicit level of detail: from the 2x2 quad's differences in the coordinates
    std::array<float, LANES> lod{};
    if (!lod_given && !zero_lod && s.mag != s.min) {  // it only picks the filter
        for (int q = 0; q < LANES; q += 4) {
            float u0 = f(vaddr_at(r_coords, q)), v0 = n > 1 ? f(vaddr_at(r_coords + 1, q)) : 0;
            float dudx, dvdx, dudy, dvdy;
            if (derivs) {
                dudx = f(vaddr_at(r_derivs, q));
                dvdx = f(vaddr_at(r_derivs + 1, q));
                dudy = f(vaddr_at(r_derivs + 2, q));
                dvdy = f(vaddr_at(r_derivs + 3, q));
            } else {
                dudx = f(vaddr_at(r_coords, q + 1)) - u0;
                dudy = f(vaddr_at(r_coords, q + 2)) - u0;
                dvdx = n > 1 ? f(vaddr_at(r_coords + 1, q + 1)) - v0 : 0;
                dvdy = n > 1 ? f(vaddr_at(r_coords + 1, q + 2)) - v0 : 0;
            }
            if (t.type == 12) dvdx = dvdy = 0;
            float sx = normalized ? W : 1, sy = normalized ? H : 1;
            float px = std::hypot(dudx * sx, dvdx * sy), py = std::hypot(dudy * sx, dvdy * sy);
            float l = std::log2(std::max({px, py, 1e-20f}));
            for (int j = 0; j < 4; j++) lod[size_t(q + j)] = l + (bias ? f(vaddr_at(r_bias, q + j)) : 0);
        }
    }
    for (int ln = 0; ln < LANES; ln++) {
        if (!((exec >> ln) & 1)) continue;
        float x = f(vaddr_at(r_coords, ln)), y = n > 1 ? f(vaddr_at(r_coords + 1, ln)) : 0.5f;
        u32 slice = 0;
        if (t.type == 12) {  // 1D array: (x, slice)
            slice = u32(std::max(0.0f, std::nearbyint(y)));
            y = 0.5f;
            if (normalized) y = 0.5f / H;
        } else if (t.type == 13 || t.type == 11 || t.type == 10) {
            float z = f(vaddr_at(r_coords + 2, ln));
            if (t.type == 10) slice = u32(std::clamp(int(std::floor(z * (normalized ? float(t.depth) : 1))), 0, int(t.depth) - 1));
            else slice = u32(std::max(0.0f, std::nearbyint(z)));
            if (t.type == 11) {  // a cube's face: the coordinates come as 1 to 2
                x -= 1;
                y -= 1;
                if (!normalized) note("unnormalized cube sampling");
            }
        }
        if (t.array() || t.type == 11) slice = std::min(slice + t.base_array, std::max(t.last_array, t.base_array));
        if (t.type == 11 && t.last_array < 5) slice = std::min(slice, 5u);
        float tx = normalized ? x * W : x, ty = normalized ? y * H : y;
        if (offset) {
            u32 o = vaddr_at(r_offset, ln);
            tx += float(std::int32_t(sext(o & 0x3f, 6)));
            ty += float(std::int32_t(sext((o >> 8) & 0x3f, 6)));
        }
        float level = lod_given ? f(vaddr_at(r_lod, ln)) : zero_lod ? 0 : lod[size_t(ln)];
        if (lod_given && level >= 1) note("images' mip levels past the first");
        u32 filter = level <= 0 ? s.mag : s.min;
        u32 raw[4], out[4];
        if (gather) {
            int comp = dmask ? std::countr_zero(dmask) : 0;
            int x0 = int(std::floor(tx - 0.5f)), y0 = int(std::floor(ty - 0.5f));
            u32 c[4][4], sw[4];
            fetch_addressed(t, mem, s, x0, y0 + 1, slice, c[0]);
            fetch_addressed(t, mem, s, x0 + 1, y0 + 1, slice, c[1]);
            fetch_addressed(t, mem, s, x0 + 1, y0, slice, c[2]);
            fetch_addressed(t, mem, s, x0, y0, slice, c[3]);
            for (int j = 0; j < 4; j++) {
                swizzle(t.sel, is_int(t.nfmt), c[j], sw);
                out[j] = sw[comp];
            }
            for (int j = 0; j < 4; j++) w.v[(vdata + u32(j)) & 255][size_t(ln)] = out[j];
            continue;
        }
        filtered(t, mem, s, filter, tx, ty, slice, raw);
        swizzle(t.sel, is_int(t.nfmt), raw, out);
        write_out(ln, out);
    }
}

void ds_op(Wave& w, u32 w0, u32 w1) {
    u32 op = (w0 >> 18) & 0xff, off0 = w0 & 0xff, off1 = (w0 >> 8) & 0xff;
    if ((w0 >> 17) & 1) {
        note("GDS operations");
        return;
    }
    u32 addr = w1 & 0xff, d0 = (w1 >> 8) & 0xff, d1 = (w1 >> 16) & 0xff, dst = w1 >> 24;
    if (!w.lds || w.lds->empty()) {
        note("LDS operations outside a compute shader");
        return;
    }
    auto& lds = *w.lds;
    u64 exec = w.exec();
    auto rd = [&](u32 a) -> u32 {
        u32 v = 0;
        if (u64(a) + 4 <= lds.size()) std::memcpy(&v, &lds[a], 4);
        return v;
    };
    auto wr = [&](u32 a, u32 v) {
        if (u64(a) + 4 <= lds.size()) std::memcpy(&lds[a], &v, 4);
    };
    u32 off = off0 | (off1 << 8);
    for (int ln = 0; ln < LANES; ln++) {
        if (!((exec >> ln) & 1)) continue;
        u32 a = w.v[addr][size_t(ln)];
        u32 x = w.v[d0][size_t(ln)], y = w.v[d1][size_t(ln)];
        auto vd = [&](u32 k) -> u32& { return w.v[(dst + k) & 255][size_t(ln)]; };
        auto dat = [&](u32 base, u32 k) { return w.v[(base + k) & 255][size_t(ln)]; };
        switch (op) {
            case 13: wr(a + off, x); break;  // ds_write_b32
            case 14: wr(a + off0 * 4, x); wr(a + off1 * 4, y); break;
            case 15: wr(a + off0 * 256, x); wr(a + off1 * 256, y); break;
            case 30: if (a + off < lds.size()) lds[a + off] = (unsigned char)x; break;
            case 31: if (a + off + 2 <= lds.size()) std::memcpy(&lds[a + off], &x, 2); break;
            case 54: vd(0) = rd(a + off); break;  // ds_read_b32
            case 55: vd(0) = rd(a + off0 * 4); vd(1) = rd(a + off1 * 4); break;
            case 56: vd(0) = rd(a + off0 * 256); vd(1) = rd(a + off1 * 256); break;
            case 57: vd(0) = a + off < lds.size() ? sext(lds[a + off], 8) : 0; break;
            case 58: vd(0) = a + off < lds.size() ? lds[a + off] : 0; break;
            case 59: case 60: {
                u32 v = 0;
                if (a + off + 2 <= lds.size()) std::memcpy(&v, &lds[a + off], 2);
                vd(0) = op == 59 ? sext(v, 16) : v;
                break;
            }
            case 77: wr(a + off, x); wr(a + off + 4, dat(d0, 1)); break;  // ds_write_b64
            case 78: wr(a + off0 * 8, x); wr(a + off0 * 8 + 4, dat(d0, 1)); wr(a + off1 * 8, y); wr(a + off1 * 8 + 4, dat(d1, 1)); break;
            case 79: wr(a + off0 * 512, x); wr(a + off0 * 512 + 4, dat(d0, 1)); wr(a + off1 * 512, y); wr(a + off1 * 512 + 4, dat(d1, 1)); break;
            case 118: vd(0) = rd(a + off); vd(1) = rd(a + off + 4); break;  // ds_read_b64
            case 119: vd(0) = rd(a + off0 * 8); vd(1) = rd(a + off0 * 8 + 4); vd(2) = rd(a + off1 * 8); vd(3) = rd(a + off1 * 8 + 4); break;
            case 120: vd(0) = rd(a + off0 * 512); vd(1) = rd(a + off0 * 512 + 4); vd(2) = rd(a + off1 * 512); vd(3) = rd(a + off1 * 512 + 4); break;
            case 222: case 223: for (u32 k = 0; k < (op == 222 ? 3u : 4u); k++) wr(a + off + 4 * k, dat(d0, k)); break;
            case 254: case 255: for (u32 k = 0; k < (op == 254 ? 3u : 4u); k++) vd(k) = rd(a + off + 4 * k); break;
            default:
                if (op <= 12 || (op >= 32 && op <= 45) || op == 48) {  // the 32-bit atomics
                    u32 o = op >= 32 ? op - 32 : op, old = rd(a + off), nv;
                    switch (o) {
                        case 0: nv = old + x; break;
                        case 1: nv = old - x; break;
                        case 2: nv = x - old; break;
                        case 3: nv = old >= x ? 0 : old + 1; break;
                        case 4: nv = old == 0 || old > x ? x : old - 1; break;
                        case 5: nv = u32(std::min(std::int32_t(old), std::int32_t(x))); break;
                        case 6: nv = u32(std::max(std::int32_t(old), std::int32_t(x))); break;
                        case 7: nv = std::min(old, x); break;
                        case 8: nv = std::max(old, x); break;
                        case 9: nv = old & x; break;
                        case 10: nv = old | x; break;
                        case 11: nv = old ^ x; break;
                        case 12: nv = (old & ~x) | y; break;
                        case 13: nv = x; break;  // wrxchg
                        case 16: nv = old == x ? y : old; break;  // cmpst
                        default: note(std::format("LDS op {}", op)); return;
                    }
                    wr(a + off, nv);
                    if (op >= 32) vd(0) = old;
                } else {
                    note(std::format("LDS op {}", op));
                    return;
                }
        }
    }
}

}  // namespace

Result run(Wave& w, u64 pc, Io& io, const Memory& mem) {
    u64 code_lo = 0, code_hi = 0;
    for (u64 steps = 0; steps < 20'000'000; steps++) {
        if (pc < code_lo || pc + 8 > code_hi) {  // the range the code is in, found once
            code_lo = code_hi = 0;
            for (auto [lo, hi] : mem.ranges)
                if (pc >= lo && pc + 4 <= hi) code_lo = lo, code_hi = hi;
            if (!code_hi) {
                note("shader code outside the game's memory");
                return {Stop::fault, pc};
            }
        }
        const u32* p = reinterpret_cast<const u32*>(pc);
        u32 w0 = p[0];
        auto lit = [&]() { return pc + 8 <= code_hi ? p[1] : 0u; };
        u64 next = pc + 4;
        if (!(w0 >> 31)) {
            u32 top7 = w0 >> 25;
            Vop o{};
            if (top7 == 0x3f) {  // VOP1
                o.op = 0x180 + ((w0 >> 9) & 0xff);
                o.vdst = (w0 >> 17) & 0xff;
                o.sdst = o.vdst;
                o.src[0] = w0 & 0x1ff;
            } else if (top7 == 0x3e) {  // VOPC
                o.op = (w0 >> 17) & 0xff;
                o.sdst = 106;
                o.src[0] = w0 & 0x1ff;
                o.src[1] = 256 + ((w0 >> 9) & 0xff);
            } else {  // VOP2
                u32 op = (w0 >> 25) & 0x3f;
                o.op = 0x100 + op;
                o.vdst = (w0 >> 17) & 0xff;
                o.sdst = 106;
                o.src[0] = w0 & 0x1ff;
                o.src[1] = 256 + ((w0 >> 9) & 0xff);
                o.src[2] = 106;  // vcc: v_cndmask's mask, v_addc's carry
                if (op == 32) {  // v_madmk_f32: src0 * K + vsrc1
                    o.op = 0x141;
                    o.src[2] = o.src[1];
                    o.src[1] = 255;
                    o.lit = lit();
                    next += 4;
                } else if (op == 33) {  // v_madak_f32: src0 * vsrc1 + K
                    o.op = 0x141;
                    o.src[2] = 255;
                    o.lit = lit();
                    next += 4;
                }
            }
            if (o.src[0] == 255) {
                o.lit = lit();
                next += 4;
            }
            vop(w, o);
            pc = next;
            continue;
        }
        if ((w0 >> 30) == 2) {  // scalar
            u32 top9 = w0 >> 23;
            if (top9 == 0x17f) {  // SOPP
                u32 op = (w0 >> 16) & 0x7f;
                std::int16_t imm = std::int16_t(w0 & 0xffff);
                u64 target = next + i64(imm) * 4;
                bool take = false;
                switch (op) {
                    case 1: case 13: return {Stop::end, next};
                    case 2: take = true; break;
                    case 4: take = !w.scc; break;
                    case 5: take = w.scc; break;
                    case 6: take = w.vcc() == 0; break;
                    case 7: take = w.vcc() != 0; break;
                    case 8: take = w.exec() == 0; break;
                    case 9: take = w.exec() != 0; break;
                    case 10: return {Stop::barrier, next};
                    case 18: note("s_trap"); return {Stop::fault, pc};
                    default: break;  // s_nop, s_waitcnt, s_sendmsg, s_setprio...: nothing to do here
                }
                pc = take ? target : next;
                continue;
            }
            u32 l = 0;
            bool ok = true;
            if (top9 == 0x17d) {  // SOP1
                u32 op = (w0 >> 8) & 0xff, s0 = w0 & 0xff;
                if (s0 == 255) l = lit(), next += 4;
                u64 newpc = next;
                ok = sop1(w, op, (w0 >> 16) & 0x7f, s0, l, newpc, next);
                if (!ok) note(std::format("SOP1 op {}", op));
                pc = newpc;
                continue;
            }
            if (top9 == 0x17e) {  // SOPC
                u32 s0 = w0 & 0xff, s1 = (w0 >> 8) & 0xff;
                if (s0 == 255 || s1 == 255) l = lit(), next += 4;
                ok = sopc(w, (w0 >> 16) & 0x7f, s0, s1, l);
                if (!ok) note(std::format("SOPC op {}", (w0 >> 16) & 0x7f));
            } else if ((w0 >> 28) == 0xb) {  // SOPK
                u32 op = (w0 >> 23) & 0x1f;
                if (op == 21) next += 4;  // s_setreg_imm32_b32's literal
                if (op == 17) {
                    note("s_cbranch_i_fork");
                    return {Stop::fault, pc};
                }
                ok = sopk(w, op, (w0 >> 16) & 0x7f, w0 & 0xffff);
                if (!ok) note(std::format("SOPK op {}", op));
            } else {  // SOP2
                u32 op = (w0 >> 23) & 0x7f, s0 = w0 & 0xff, s1 = (w0 >> 8) & 0xff;
                if (s0 == 255 || s1 == 255) l = lit(), next += 4;
                if (op == 43) {
                    note("s_cbranch_g_fork");
                    return {Stop::fault, pc};
                }
                ok = sop2(w, op, (w0 >> 16) & 0x7f, s0, s1, l);
                if (!ok) note(std::format("SOP2 op {}", op));
            }
            pc = next;
            continue;
        }
        u32 enc = w0 >> 26;
        if (enc == 0x30 || enc == 0x31) {  // SMRD
            bool imm = (w0 >> 8) & 1;
            u32 off = w0 & 0xff, l = 0;
            if (!imm && off == 255) l = lit(), next += 4;
            smrd(w, mem, (w0 >> 22) & 0x1f, (w0 >> 15) & 0x7f, ((w0 >> 9) & 0x3f) * 2, imm, off, l);
            pc = next;
            continue;
        }
        if (enc == 0x32) {  // VINTRP
            u32 op = (w0 >> 16) & 3, attr = (w0 >> 10) & 0x3f, chan = (w0 >> 8) & 3, src = w0 & 0xff;
            u32 dst = (w0 >> 18) & 0xff;
            float p0, p10, p20;
            io.attr(attr, chan, p0, p10, p20);
            u64 exec = w.exec();
            for (int ln = 0; ln < LANES; ln++) {
                if (!((exec >> ln) & 1)) continue;
                u32& d = w.v[dst][size_t(ln)];
                if (op == 0) d = u(p10 * f(w.v[src][size_t(ln)]) + p0);
                else if (op == 1) d = u(p20 * f(w.v[src][size_t(ln)]) + f(d));
                else d = u(src == 0 ? p10 : src == 1 ? p20 : p0);
            }
            pc = next;
            continue;
        }
        // the two-dword formats
        if (pc + 8 > code_hi) return {Stop::fault, pc};
        u32 w1 = p[1];
        next = pc + 8;
        switch (enc) {
            case 0x34: {  // VOP3
                Vop o{};
                o.op = (w0 >> 17) & 0x1ff;
                o.src[0] = w1 & 0x1ff;
                o.src[1] = (w1 >> 9) & 0x1ff;
                o.src[2] = (w1 >> 18) & 0x1ff;
                o.neg = w1 >> 29;
                o.omod = (w1 >> 27) & 3;
                bool b = (o.op >= 0x125 && o.op <= 0x12a) || o.op == 0x16d || o.op == 0x16e || o.op == 0x176 || o.op == 0x177;
                o.vdst = w0 & 0xff;
                if (b) {
                    o.sdst = (w0 >> 8) & 0x7f;
                } else {
                    o.abs = (w0 >> 8) & 7;
                    o.clamp = (w0 >> 11) & 1;
                    o.sdst = o.vdst;  // a comparison's, v_readlane's and v_readfirstlane's result
                }
                vop(w, o);
                break;
            }
            case 0x36: ds_op(w, w0, w1); break;
            case 0x38: case 0x3a: buffer_op(w, mem, w0, w1); break;
            case 0x3c: image_op(w, mem, w0, w1); break;
            case 0x3e: {  // EXP
                u32 en = w0 & 0xf, target = (w0 >> 4) & 0x3f;
                bool compr = (w0 >> 10) & 1;
                const Vec* regs[4] = {};
                for (u32 k = 0; k < 4; k++) {
                    if (!((en >> k) & 1)) continue;
                    u32 r = compr ? (k < 2 ? w1 & 0xff : (w1 >> 8) & 0xff) : (w1 >> (8 * k)) & 0xff;
                    regs[k] = &w.v[r];
                }
                io.exp(target, regs, compr, w.exec());
                break;
            }
            default:
                note(std::format("instruction format {:#x}", enc));
                break;
        }
        pc = next;
    }
    note("a wave that ran for 20 million instructions (stopped)");
    return {Stop::fault, pc};
}

void buffer_element(const u32* v, u32 dfmt, u32 nfmt, u64 offset, u32 index, const Memory& mem, u32 out[4]) {
    Buffer b(v);
    u32 raw[4] = {0, 0, 0, is_int(nfmt) ? 1u : u(1.0f)};
    Layout l = layout(dfmt);
    bool in_range = b.stride == 0 ? offset < b.records : index < b.records;
    u64 at = b.base + offset + u64(b.stride) * index;
    if (l.count && in_range && mem.ok(at, u64(l.bytes))) unpack(dfmt, nfmt, reinterpret_cast<const unsigned char*>(at), raw);
    swizzle(b.sel, is_int(nfmt), raw, out);
}

void texel_rgba(const Image& t, const Memory& mem, int x, int y, u32 slice, u32 out[4]) {
    u32 raw[4];
    texel(t, mem, x, y, slice, raw);
    swizzle(t.sel, is_int(t.nfmt), raw, out);
}

}  // namespace mensis::shade
