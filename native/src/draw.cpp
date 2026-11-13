// SPDX-License-Identifier: GPL-3.0-or-later
//
// Draws and dispatches, run on the CPU: what the GPU's fixed-function parts do around the
// shaders (shade.cpp).
//
// A draw: the vertex shader runs once for each vertex (64 to a wave), and exports a position
// and parameters. Vertices make triangles (a list, strip or fan; a "rectangle" is three corners
// of a parallelogram). Each triangle goes through the viewport to pixels, cut to the scissor,
// and its pixels run the pixel shader in 2x2 quads, 16 quads to a wave: a quad lets a shader
// compare its neighbours, which is how a texture's level of detail is found. The pixel shader
// interpolates the parameters (v_interp, from the triangle's three vertices) and exports a
// colour, which goes to the render target in its format, blended if blending is on.
//
// A dispatch: the compute shader runs once for each thread of each group, 64 threads to a
// wave; a group's waves share 64 KiB of local memory (LDS) and wait for each other at
// s_barrier.
//
// Not done yet: depth and stencil, clipping to the near plane (a triangle with a vertex
// behind the eye is skipped), multisampling, mip levels past the first.
//
// With --gpu-check, each draw saved to draws/ is drawn on the PC's GPU as well, its vertex
// and pixel shaders translated (translate.cpp, vkdraw.cpp), into a copy of the render target
// as it was before; then the CPU draws it, and the two are compared pixel by pixel. If the
// vertex shader can't be translated, the GPU is given the CPU's vertices instead.

#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#include "shade.hpp"
#include "translate.hpp"
#include "vkdraw.hpp"

namespace mensis {
namespace {

using namespace shade;

// ---- a pool of threads for the pixels and the groups ----

struct Pool {
    int n = 1;
    std::vector<std::thread> threads;
    std::mutex m;
    std::condition_variable cv, done;
    std::function<void(int)> job;
    u64 generation = 0;
    int remaining = 0;

    Pool() {
        n = int(std::clamp(std::thread::hardware_concurrency(), 1u, 16u));
        for (int i = 1; i < n; i++)
            threads.emplace_back([this, i] {
                u64 seen = 0;
                for (;;) {
                    std::function<void(int)> f;
                    {
                        std::unique_lock lock(m);
                        cv.wait(lock, [&] { return generation != seen; });
                        seen = generation;
                        f = job;
                    }
                    f(i);
                    std::lock_guard lock(m);
                    if (--remaining == 0) done.notify_all();
                }
            });
        for (auto& t : threads) t.detach();
    }
    // f(0) ... f(n - 1), at once; returns when all are done
    void run(const std::function<void(int)>& f) {
        {
            std::lock_guard lock(m);
            job = f;
            remaining = n - 1;
            generation++;
        }
        cv.notify_all();
        f(0);
        std::unique_lock lock(m);
        done.wait(lock, [&] { return remaining == 0; });
    }
};

Pool& pool() {
    static Pool p;
    return p;
}

std::mutex g_draw_lock;  // the GPU's queues draw one at a time: the pool is shared
std::atomic<u64> g_saved{0};
std::atomic<u64> g_draw_ns{0}, g_dispatch_ns{0};  // the time drawing and dispatching took

struct Timer {  // adds the time from here to the end of the scope
    std::atomic<u64>& to;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit Timer(std::atomic<u64>& t) : to(t) {}
    ~Timer() { to += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()); }
};
std::atomic<u64> g_draws{0}, g_triangles{0}, g_pixels{0}, g_ps_waves{0}, g_dispatches{0}, g_groups{0};

u32 reg(const GpuRegs& r, u32 i, u32 dflt) { return r.ctx_set[i] ? r.ctx[i] : dflt; }
float regf(const GpuRegs& r, u32 i, float dflt) { return r.ctx_set[i] ? f(r.ctx[i]) : dflt; }
u64 program(const GpuRegs& r, u32 lo) { return (u64(r.sh[lo]) << 8) | (u64(r.sh[lo + 1] & 0xff) << 40); }

// ---- vertices ----

struct Vertices {
    u32 params = 1;
    std::vector<float> pos;  // 4 a vertex
    std::vector<float> param;  // params * 4 a vertex
};

struct VsIo : Io {
    Vertices& out;
    u32 first = 0;
    explicit VsIo(Vertices& o) : out(o) {}
    void exp(u32 target, const Vec* const regs[4], bool, u64 lanes) override {
        for (int l = 0; l < LANES; l++) {
            if (!((lanes >> l) & 1)) continue;
            size_t v = first + u32(l);
            for (int c = 0; c < 4; c++) {
                float x = regs[c] ? f((*regs[c])[size_t(l)]) : (c == 3 ? 1.0f : 0.0f);
                if (target == 12) out.pos[v * 4 + size_t(c)] = x;
                else if (target >= 32 && target - 32 < out.params) out.param[(v * out.params + target - 32) * 4 + size_t(c)] = x;
            }
        }
    }
};

// ---- pixels ----

struct Target {  // a render target, from its CB_COLORn registers
    u64 base = 0;
    u32 pitch = 0, height = 0, format = 0, nfmt = 0, swap = 0, tiling = 0, bytes = 0, count = 0;
    u32 blend = 0, mask = 0;
    // worked out once a draw: the components' order in memory (COMP_SWAP), whether its pixels
    // are in rows (not micro-tiled), whether all of it is memory the GPU may write, and
    // whether it is 8-bit RGBA unorm (the usual), which has a short way through
    int order[4] = {0, 1, 2, 3};
    bool rows = true, all_ok = false, unorm8 = false;
    void prepare(const Memory& mem) {
        static const int swap4[4][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}, {3, 0, 1, 2}};
        static const int swap2[4][2] = {{0, 1}, {0, 3}, {1, 0}, {3, 0}};
        if (count >= 3)
            for (int c = 0; c < 4; c++) order[c] = swap4[swap][c];
        else if (count == 2)
            order[0] = swap2[swap][0], order[1] = swap2[swap][1];
        else
            order[0] = int(swap);
        rows = !(tiling == 5 || tiling == 9 || tiling == 13 || tiling == 19);
        u64 size = u64(pitch) * ((u64(height) + 7) / 8 * 8) * bytes;
        all_ok = count && mem.ok(base, size);
        unorm8 = format == 10 && nfmt == 0;
    }
    u64 address(u32 x, u32 y) const {
        return rows ? base + (u64(y) * pitch + x) * bytes : element_address(base, tiling, bytes, pitch, height, x, y, 0);
    }
};

struct Tri {
    float x[3], y[3], z[3], invw[3];
    float area;
    bool front;
    size_t v[3];  // the vertices, for the parameters
};

struct PsIo : Io {
    const GpuRegs& r;
    const Memory& mem;
    const Vertices& vs;
    const Tri* tri = nullptr;
    const Target* targets;
    u32 col_format;
    float blend_color[4];
    int px[LANES], py[LANES];
    u64 cover = 0;
    u64 written = 0;
    PsIo(const GpuRegs& r_, const Memory& m, const Vertices& v, const Target* t)
        : r(r_), mem(m), vs(v), targets(t) {
        col_format = reg(r, 0x1c5, 0);
        for (int c = 0; c < 4; c++) blend_color[c] = regf(r, 0x105 + u32(c), 0);
    }

    void attr(u32 attr, u32 chan, float& p0, float& p10, float& p20) override {
        u32 cntl = reg(r, 0x191 + attr, attr);  // SPI_PS_INPUT_CNTL_n: which parameter, and how
        u32 offset = cntl & 0x3f;
        if (offset & 0x20) {  // a constant instead
            u32 dv = (cntl >> 8) & 3;
            float v = chan == 3 ? (dv == 1 || dv == 3 ? 1.0f : 0.0f) : (dv >= 2 ? 1.0f : 0.0f);
            p0 = v;
            p10 = p20 = 0;
            return;
        }
        if (offset >= vs.params) {
            p0 = p10 = p20 = 0;
            return;
        }
        auto at = [&](size_t v) { return vs.param[(v * vs.params + offset) * 4 + chan]; };
        p0 = at(tri->v[0]);
        if ((cntl >> 10) & 1) {  // flat
            p10 = p20 = 0;
            return;
        }
        p10 = at(tri->v[1]) - p0;
        p20 = at(tri->v[2]) - p0;
    }

    static float blend_factor(u32 k, const float s[4], const float d[4], const float c[4], int ch) {
        switch (k) {
            case 0: return 0;
            case 1: return 1;
            case 2: return s[ch];
            case 3: return 1 - s[ch];
            case 4: return s[3];
            case 5: return 1 - s[3];
            case 6: return d[3];
            case 7: return 1 - d[3];
            case 8: return d[ch];
            case 9: return 1 - d[ch];
            case 10: return ch == 3 ? 1 : std::min(s[3], 1 - d[3]);
            case 13: return c[ch];
            case 14: return 1 - c[ch];
            case 19: return c[3];
            case 20: return 1 - c[3];
            default: note(std::format("blend factor {}", k)); return 1;
        }
    }
    static float combine(u32 fn, float s, float d) {
        switch (fn) {
            case 1: return s - d;
            case 2: return std::min(s, d);
            case 3: return std::max(s, d);
            case 4: return d - s;
            default: return s + d;
        }
    }

    void exp(u32 target, const Vec* const regs[4], bool compr, u64 lanes) override {
        if (target > 7) return;  // mrtz (depth), null
        const Target& t = targets[target];
        if (!t.count) return;
        u32 cf = (col_format >> (4 * target)) & 15;
        if (cf == 0) return;
        bool integer = t.nfmt == 4 || t.nfmt == 5;
        lanes &= cover;
        for (int l = 0; l < LANES; l++) {
            if (!((lanes >> l) & 1)) continue;
            // the exported value, as RGBA
            u32 rgba[4] = {0, 0, 0, integer ? 1u : u(1.0f)};
            auto reg_at = [&](int k) { return regs[k] ? (*regs[k])[size_t(l)] : 0u; };
            if (compr) {
                u32 lo = reg_at(0), hi = reg_at(2);
                if (!regs[0]) lo = reg_at(1);
                if (!regs[2]) hi = reg_at(3);
                u32 halves[4] = {lo & 0xffff, lo >> 16, hi & 0xffff, hi >> 16};
                for (int c = 0; c < 4; c++) {
                    u32 h = halves[c];
                    switch (cf) {
                        case 4: rgba[c] = u(half(h)); break;
                        case 5: rgba[c] = u(float(h) / 65535.0f); break;
                        case 6: rgba[c] = u(std::max(float(std::int16_t(h)) / 32767.0f, -1.0f)); break;
                        case 7: rgba[c] = h; break;
                        case 8: rgba[c] = u32(std::int32_t(std::int16_t(h))); break;
                        default: rgba[c] = u(half(h));
                    }
                }
            } else {
                switch (cf) {
                    case 1: rgba[0] = reg_at(0); break;
                    case 2: rgba[0] = reg_at(0), rgba[1] = reg_at(1); break;
                    case 3: rgba[0] = reg_at(0), rgba[3] = reg_at(3); break;
                    default: for (int c = 0; c < 4; c++) rgba[c] = reg_at(c);
                }
            }
            int x = px[l], y = py[l];
            if (x < 0 || y < 0 || u32(x) >= t.pitch || u32(y) >= t.height) continue;
            u64 at = t.address(u32(x), u32(y));
            if (!at || (!t.all_ok && !mem.ok(at, t.bytes))) continue;
            auto* p = reinterpret_cast<unsigned char*>(at);
            const int* order = t.order;
            u32 old_mem[4], old[4] = {0, 0, 0, integer ? 1u : u(1.0f)};
            bool need_old = (t.mask & 15) != 15 || ((t.blend >> 30) & 1);
            if (need_old) {
                if (t.unorm8)
                    for (int c = 0; c < 4; c++) old_mem[c] = u(float(p[c]) * (1.0f / 255.0f));
                else
                    unpack(t.format, t.nfmt, p, old_mem);
                for (u32 c = 0; c < t.count; c++) old[order[c]] = old_mem[c];
            }
            if (((t.blend >> 30) & 1) && !integer) {
                float s[4], d[4], o[4];
                for (int c = 0; c < 4; c++) s[c] = f(rgba[c]), d[c] = f(old[c]);
                u32 bc = t.blend;
                bool separate = (bc >> 29) & 1;
                u32 csrc = bc & 31, cfn = (bc >> 5) & 7, cdst = (bc >> 8) & 31;
                u32 asrc = separate ? (bc >> 16) & 31 : csrc, afn = separate ? (bc >> 21) & 7 : cfn,
                    adst = separate ? (bc >> 24) & 31 : cdst;
                for (int c = 0; c < 4; c++) {
                    u32 sf = c == 3 ? asrc : csrc, df = c == 3 ? adst : cdst, fn = c == 3 ? afn : cfn;
                    float a = s[c], b = d[c];
                    if (fn == 2 || fn == 3) o[c] = combine(fn, a, b);
                    else o[c] = combine(fn, a * blend_factor(sf, s, d, blend_color, c), b * blend_factor(df, s, d, blend_color, c));
                }
                for (int c = 0; c < 4; c++) rgba[c] = u(o[c]);
            }
            for (int c = 0; c < 4; c++)
                if (!((t.mask >> c) & 1)) rgba[c] = old[c];
            u32 mem_order[4] = {0, 0, 0, 0};
            for (u32 c = 0; c < t.count; c++) mem_order[c] = rgba[order[c]];
            if (t.unorm8) {
                for (int c = 0; c < 4; c++) {
                    float v = f(mem_order[c]);
                    v = v > 0 ? (v < 1 ? v : 1) : 0;  // and NaN to 0
                    p[c] = (unsigned char)(v * 255.0f + 0.5f);
                }
            } else {
                pack(t.format, t.nfmt, mem_order, p);
            }
            written++;
        }
    }
};

Target target(const GpuRegs& r, u32 k) {
    Target t;
    u32 b = 0x318 + k * 15;
    u32 info = reg(r, b + 4, 0);
    t.format = (info >> 2) & 0x1f;
    if (!t.format) return t;
    static const u32 nfmt[8] = {0, 1, 2, 3, 4, 5, 9, 7};
    t.nfmt = nfmt[(info >> 8) & 7];
    t.swap = (info >> 11) & 3;
    t.base = u64(reg(r, b, 0)) << 8;
    t.pitch = ((reg(r, b + 1, 0) & 0x7ff) + 1) * 8;
    u64 slice = (u64(reg(r, b + 2, 0) & 0x3fffff) + 1) * 64;
    t.height = u32(slice / t.pitch);
    t.tiling = reg(r, b + 5, 0) & 0x1f;
    Layout l = layout(t.format);
    if (!l.count) {
        note(std::format("render targets of format {}", t.format));
        t.format = 0;
        return t;
    }
    t.bytes = u32(l.bytes);
    t.count = u32(l.count);
    t.blend = reg(r, 0x1e0 + k, 0);
    t.mask = (reg(r, 0x8e, 0) >> (4 * k)) & (reg(r, 0x8f, 0xffffffff) >> (4 * k)) & 15;
    return t;
}

// The first two draws of each pixel shader (up to 80 in all): their first render target,
// saved after each to <out>/draws/ as it then looked, as a BMP (each pixel's colour as RGB,
// alpha dropped), with a line in index.txt
constexpr u64 SAVED = 80;
std::map<u64, int> g_saved_by_ps;  // under g_draw_lock

// A 32-bit BMP of w x h pixels, B, G, R, A in memory, rows top down
void write_bmp(const std::filesystem::path& path, u32 w, u32 h, const std::vector<unsigned char>& px) {
    std::ofstream bmp(path, std::ios::binary);
    u32 size = 54 + u32(px.size());
    unsigned char h54[54] = {'B', 'M'};
    auto put = [&](int at, u32 v) { std::memcpy(h54 + at, &v, 4); };
    put(2, size);
    put(10, 54);
    put(14, 40);
    put(18, w);
    put(22, u32(-std::int32_t(h)));
    h54[26] = 1;
    h54[28] = 32;
    put(34, u32(px.size()));
    bmp.write(reinterpret_cast<const char*>(h54), 54);
    bmp.write(reinterpret_cast<const char*>(px.data()), std::streamsize(px.size()));
}

void save_target(u64 number, const Target& t, u32 width, u32 height, const Memory& mem, const std::string& what) {
    auto dir = std::filesystem::path(M.out_dir) / "draws";
    std::filesystem::create_directories(dir);
    // the index starts afresh with each run's first draw
    std::ofstream(dir / "index.txt", std::ios::binary | (number == 0 ? std::ios::trunc : std::ios::app))
        << std::format("{:03} {}x{} at {:#x}, format {} number type {}, tile mode {}: {}\n", number, t.pitch, t.height,
                       t.base, t.format, t.nfmt, t.tiling, what);
    u32 w = std::min({t.pitch, width, 4096u}), h = std::min({t.height, height, 4096u});  // to the scissor
    std::vector<unsigned char> px(size_t(w) * h * 4);
    bool integer = t.nfmt == 4 || t.nfmt == 5;
    static const int swap4[4][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}, {3, 0, 1, 2}};
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            u64 at = element_address(t.base, t.tiling, t.bytes, t.pitch, t.height, x, y, 0);
            u32 raw[4] = {0, 0, 0, 0}, rgba[4] = {0, 0, 0, 0};
            if (at && mem.ok(at, t.bytes)) unpack(t.format, t.nfmt, reinterpret_cast<const unsigned char*>(at), raw);
            for (u32 c = 0; c < t.count; c++) rgba[t.count >= 3 ? swap4[t.swap][c] : c] = raw[c];
            for (int c = 0; c < 3; c++) {
                float v = integer ? float(rgba[c] & 255) / 255.0f : f(rgba[c]);
                px[(size_t(y) * w + x) * 4 + size_t(2 - c)] = (unsigned char)std::clamp(int(std::lround(v * 255)), 0, 255);
            }
        }
    write_bmp(dir / std::format("{:03}.bmp", number), w, h, px);
}

// ---- --gpu-check: the draw on the PC's GPU too ----

std::map<u64, translate::Program> g_programs;  // decoded once each; under g_draw_lock
u64 g_checked = 0, g_check_same = 0, g_check_gpu_vertices = 0;
std::map<std::string, u64> g_check_skipped;

// A shader's translation, beside its disassembly: shaders/<vs|ps>-<address>.glsl, the first
// time it is translated in a run
void save_glsl(const char* kind, u64 at, const std::string& text) {
    static std::set<std::string> saved;  // under g_draw_lock
    std::string name = std::format("{}-{:x}.glsl", kind, at);
    if (!saved.insert(name).second) return;
    auto dir = std::filesystem::path(M.out_dir) / "shaders";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / name, std::ios::binary) << text;
}

// The vertex shader, for the GPU: translated, its buffers read, the triangles' indices, the
// viewport. False (and why) if it can't be.
bool gpu_vertices(vkdraw::Draw& d, std::string& why, const GpuRegs& r, u64 vs, u32 attributes, const Target& t,
                  const std::vector<u32>& index, const std::vector<std::array<size_t, 3>>& tris_in, bool rects, const Memory& mem) {
    u32 users = std::min((r.sh[0x4b] >> 1) & 0x1f, 16u);
    u32 user[16];
    for (u32 k = 0; k < 16; k++) user[k] = r.sh[0x4c + k];
    translate::Program prog = translate::decode(vs, mem, user, users);
    translate::Values vals = translate::evaluate(prog, user, users, mem);
    if (prog.why.empty() && !vals.why.empty()) prog.why = vals.why;
    translate::VsState st;
    st.user_count = users;
    st.attributes = attributes;
    for (u32 k = 0; k < 32; k++) st.input_cntl[k] = reg(r, 0x191 + k, k);
    for (size_t k = 0; k < vals.buffers.size() && k < 32; k++)
        if ((vals.buffers[k].v[3] >> 23) & 1) st.add_tid |= 1u << k;
    translate::Glsl g = translate::vertex_shader(prog, st);
    if (!g.why.empty()) {
        why = "the vertex shader has " + g.why;
        return false;
    }
    u32 vte = reg(r, 0x206, 0x43f);
    if ((vte >> 8) & 1) {
        why = "positions not divided by w";
        return false;
    }
    // the console's viewport, then Vulkan's for a viewport the size of the target: x' = a x + b w
    float xs = vte & 1 ? regf(r, 0x10f, 1) : 1, xo = vte & 2 ? regf(r, 0x110, 0) : 0;
    float ys = vte & 4 ? regf(r, 0x111, 1) : 1, yo = vte & 8 ? regf(r, 0x112, 0) : 0;
    float zs = vte & 16 ? regf(r, 0x113, 1) : 1, zo = vte & 32 ? regf(r, 0x114, 0) : 0;
    float W = float(t.pitch), H = float(t.height);
    float v[8] = {2 * xs / W, 2 * xo / W - 1, 2 * ys / H, 2 * yo / H - 1, zs, zo, 0, 0};
    std::memcpy(d.viewport, v, sizeof v);
    // each buffer load's elements, as the CPU's interpreter unpacks them, and then what an
    // index past the end reads
    for (const auto& b : vals.buffers) {
        u32 stride = (b.v[1] >> 16) & 0x3fff, records = b.v[2];
        u64 n = stride == 0 ? 1 : records;
        if (n > (1u << 22)) {
            why = "vertex buffers of over 4 million elements";
            return false;
        }
        std::vector<u32> data((n + 1) * 4);
        for (u32 i = 0; i < n; i++) buffer_element(b.v, b.dfmt, b.nfmt, b.offset, i, mem, &data[size_t(i) * 4]);
        if (stride == 0) std::copy_n(data.begin(), 4, data.begin() + 4);
        else buffer_element(b.v, b.dfmt, b.nfmt, b.offset, records, mem, &data[n * 4]);
        d.vs_buffers.push_back(std::move(data));
    }
    u32 offset = reg(r, 0x102, 0);  // VGT_INDX_OFFSET
    for (const auto& tri : tris_in)
        for (size_t k : tri) d.indices.push_back(index[k] + offset);
    if (rects) d.gs = translate::rect_geometry_shader(attributes);
    save_glsl("vs", vs, g.text);
    d.vs = g.text;
    d.vs_values = vals.u;
    d.cull = reg(r, 0x205, 0);
    return true;
}

// The draw, as vkdraw needs it, drawn; false (and why) if it couldn't be. vertex_why: why the
// vertices are the CPU's, if they are.
bool gpu_check(vkdraw::Draw& d, std::string& why, std::string& vertex_why, const GpuRegs& r, u64 vs, u64 ps,
               const Vertices& out, const std::vector<Tri>& tris, const std::vector<u32>& index,
               const std::vector<std::array<size_t, 3>>& tris_in, bool rects, const Target* targets, const int clip[4],
               u32 ps_user, u32 ena, u32 addr, const Memory& mem) {
    const Target& t = targets[0];
    for (u32 k = 1; k < 8; k++)
        if (targets[k].count) {
            why = "draws to more than one render target";
            return false;
        }
    if (!t.count) {
        why = "draws to a render target other than the first";
        return false;
    }
    if (t.format != 10 || (t.nfmt != 0 && t.nfmt != 9) || t.swap > 1) {
        why = std::format("render targets of format {}, number type {}, swap {}", t.format, t.nfmt, t.swap);
        return false;
    }
    if (!t.rows) {
        why = "micro-tiled render targets";
        return false;
    }
    u64 bytes = u64(t.pitch) * t.height * 4;
    if (!mem.ok(t.base, bytes)) {
        why = "a render target outside the GPU's memory";
        return false;
    }
    auto it = g_programs.find(ps);
    if (it == g_programs.end()) it = g_programs.emplace(ps, translate::decode(ps, mem)).first;
    const translate::Program& prog = it->second;
    if (!prog.why.empty()) {
        why = "the shader has " + prog.why;
        return false;
    }
    u32 user[16];
    for (u32 k = 0; k < 16; k++) user[k] = r.sh[0x0c + k];
    u32 users = std::min(ps_user, 16u);
    translate::Values vals = translate::evaluate(prog, user, users, mem);
    if (!vals.why.empty()) {
        why = "the shader has " + vals.why;
        return false;
    }
    translate::PsState st;
    st.user_count = users;
    st.ena = ena;
    st.addr = addr;
    for (u32 k = 0; k < 32; k++) st.input_cntl[k] = reg(r, 0x191 + k, k);
    st.col_format = reg(r, 0x1c5, 0);
    for (auto& im : vals.images) {
        st.image_types.push_back(im.t[3] >> 28);
        st.unnormalized.push_back((im.s[0] >> 15) & 1);
    }
    translate::Glsl gl = translate::pixel_shader(prog, st);
    if (gl.why.empty()) save_glsl("ps", ps, gl.text);
    if (!gl.why.empty()) {
        why = "the shader has " + gl.why;
        return false;
    }
    d.ps = gl.text;
    d.attributes = gl.attributes;
    d.values = vals.u;
    bool on_gpu = gpu_vertices(d, vertex_why, r, vs, gl.attributes, t, index, tris_in, rects, mem);
    if (!on_gpu) {
        d.indices.clear();
        d.vs_buffers.clear();
        d.gs.clear();
        d.vs = translate::pass_vertex_shader(gl.attributes);
    }
    // Or the CPU's vertices, in clip space for a viewport the size of the target; the
    // attributes as SPI_PS_INPUT_CNTL picks them from the vertex shader's parameters
    float W = float(t.pitch), H = float(t.height);
    if (!on_gpu)
    for (const Tri& tr : tris)
        for (int k = 0; k < 3; k++) {
            float w = 1 / tr.invw[k];
            float pos[4] = {(2 * tr.x[k] / W - 1) * w, (2 * tr.y[k] / H - 1) * w, std::clamp(tr.z[k], 0.0f, 1.0f) * w, w};
            d.vertices.insert(d.vertices.end(), pos, pos + 4);
            for (u32 a = 0; a < gl.attributes; a++) {
                u32 cntl = reg(r, 0x191 + a, a), offset = cntl & 0x3f;
                for (u32 c = 0; c < 4; c++) {
                    float v = 0;
                    if (offset & 0x20) {
                        u32 dv = (cntl >> 8) & 3;
                        v = c == 3 ? (dv == 1 || dv == 3 ? 1.0f : 0.0f) : (dv >= 2 ? 1.0f : 0.0f);
                    } else if (offset < out.params) {
                        v = out.param[(tr.v[k] * out.params + offset) * 4 + c];
                    }
                    d.vertices.push_back(v);
                }
            }
        }
    // the textures, each texel as the CPU's interpreter reads it
    for (auto& im : vals.images) {
        Image ti(im.t);
        ti.prepare(mem);
        vkdraw::Texture tx;
        tx.type = ti.type;
        tx.width = ti.width;
        tx.height = ti.type == 8 || ti.type == 12 ? 1 : ti.height;
        tx.layers = ti.array() ? ti.last_array - ti.base_array + 1 : ti.type == 10 ? ti.depth : 1;
        if (ti.nfmt == 4 || ti.nfmt == 5) {
            why = "integer textures";
            return false;
        }
        if (u64(tx.width) * tx.height * tx.layers > (1u << 24)) {
            why = "textures of over 16 million texels";
            return false;
        }
        std::memcpy(tx.sampler, im.s, sizeof tx.sampler);
        tx.rgba.resize(size_t(tx.width) * tx.height * tx.layers * 4);
        float* o = tx.rgba.data();
        for (u32 l = 0; l < tx.layers; l++) {
            u32 slice = ti.array() ? ti.base_array + l : ti.type == 10 ? l : 0;
            for (u32 y = 0; y < tx.height; y++)
                for (u32 x = 0; x < tx.width; x++, o += 4) {
                    u32 v[4];
                    texel_rgba(ti, mem, int(x), int(y), slice, v);
                    std::memcpy(o, v, 16);
                }
        }
        d.textures.push_back(std::move(tx));
    }
    d.width = t.pitch;
    d.height = t.height;
    // VkFormat: R8G8B8A8 unorm 37, srgb 43; B8G8R8A8 unorm 44, srgb 50 (COMP_SWAP alt: B first)
    d.format = t.swap == 0 ? (t.nfmt == 9 ? 43 : 37) : (t.nfmt == 9 ? 50 : 44);
    d.target.assign(reinterpret_cast<const unsigned char*>(t.base), reinterpret_cast<const unsigned char*>(t.base) + bytes);
    d.blend = t.blend;
    d.mask = t.mask;
    for (int c = 0; c < 4; c++) d.blend_color[c] = regf(r, 0x105 + u32(c), 0);
    for (int k = 0; k < 4; k++) d.scissor[k] = clip[k];
    d.before = d.target;
    return vkdraw::run(d, why);
}

// The GPU's picture against the CPU's, now in the target: how many pixels differ by more than
// 1 in 255 in any channel, and by how much at most, and how many the draw changed (on the
// GPU), so that "the same" isn't only "left as they were". Saves the GPU's as NNN-gpu.bmp.
std::string compare(u64 number, const vkdraw::Draw& d, const Target& t, const int clip[4]) {
    u64 n = 0, differ = 0, changed = 0;
    int most = 0;
    for (int y = clip[1]; y < clip[3] && u32(y) < d.height; y++)
        for (int x = clip[0]; x < clip[2] && u32(x) < d.width; x++) {
            size_t at = (size_t(y) * d.width + size_t(x)) * 4;
            const auto* cpu = reinterpret_cast<const unsigned char*>(t.base) + at;
            int m = 0;
            for (int c = 0; c < 4; c++) m = std::max(m, std::abs(int(cpu[c]) - int(d.target[at + size_t(c)])));
            n++;
            differ += m > 1;
            changed += std::memcmp(&d.before[at], &d.target[at], 4) != 0;
            most = std::max(most, m);
        }
    u32 w = std::min({d.width, u32(clip[2]), 4096u}), h = std::min({d.height, u32(clip[3]), 4096u});
    std::vector<unsigned char> px(size_t(w) * h * 4);
    bool rgba = t.swap == 0;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const unsigned char* p = &d.target[(size_t(y) * d.width + x) * 4];
            unsigned char* q = &px[(size_t(y) * w + x) * 4];
            q[0] = rgba ? p[2] : p[0];
            q[1] = p[1];
            q[2] = rgba ? p[0] : p[2];
            q[3] = 0;
        }
    write_bmp(std::filesystem::path(M.out_dir) / "draws" / std::format("{:03}-gpu.bmp", number), w, h, px);
    g_checked++;
    if (!differ) {
        g_check_same++;
        return std::format("on the GPU: the same in all {} pixels ({} of them changed by the draw)", n, changed);
    }
    return std::format("on the GPU: {} of {} pixels differ by more than 1 in 255 (by {} at most; {} changed by the draw)", differ, n,
                       most, changed);
}

// The triangles a primitive type makes of n vertices
std::vector<std::array<size_t, 3>> assemble(u32 type, size_t n, bool& rects, bool& known) {
    std::vector<std::array<size_t, 3>> t;
    rects = false;
    known = true;
    switch (type) {
        case 4: for (size_t i = 0; i + 2 < n; i += 3) t.push_back({i, i + 1, i + 2}); break;
        case 5: for (size_t i = 1; i + 1 < n; i++) t.push_back({0, i, i + 1}); break;
        case 6:
            for (size_t i = 0; i + 2 < n; i++)
                t.push_back(i & 1 ? std::array<size_t, 3>{i + 1, i, i + 2} : std::array<size_t, 3>{i, i + 1, i + 2});
            break;
        case 0x11: rects = true; for (size_t i = 0; i + 2 < n; i += 3) t.push_back({i, i + 1, i + 2}); break;
        case 0x13: for (size_t i = 0; i + 3 < n; i += 4) { t.push_back({i, i + 1, i + 2}); t.push_back({i, i + 2, i + 3}); } break;
        default: known = false;
    }
    return t;
}

}  // namespace

std::string gpu_draw(const GpuRegs& r, u32 count, u64 indices, u32 first) {
    if (!M.draw || !count) return {};
    std::lock_guard draw_lock(g_draw_lock);
    Timer timer(g_draw_ns);
    g_draws++;
    u64 vs = program(r, 0x48), ps = program(r, 0x08);
    if (!vs) return "(no vertex shader)";
    Memory mem;
    mem.snapshot();
    u32 type = r.uc[0x242];
    bool rects, known;
    // the vertices: through the index buffer, or numbered
    std::vector<u32> index(count);
    for (u32 i = 0; i < count; i++) {
        if (!indices) {
            index[i] = i;
        } else {
            u64 at = indices + u64(first + i) * r.index_size;
            u32 v = 0;
            if (mem.ok(at, r.index_size)) std::memcpy(&v, reinterpret_cast<const void*>(at), r.index_size);
            index[i] = v;
        }
    }
    auto tris_in = assemble(type, count, rects, known);
    if (!known) {
        note(std::format("primitive type {:#x}", type));
        return std::format("(primitive type {:#x}: not drawn)", type);
    }
    if (reg(r, 0x200, 0) & 2) note("draws with a depth test (drawn without it)");
    // the vertex shader, 64 vertices a wave; a rectangle's fourth corner is added after
    Vertices out;
    out.params = ((reg(r, 0x1b1, 0) >> 1) & 0x1f) + 1;
    size_t n = count + (rects ? count / 3 : 0);
    out.pos.assign(n * 4, 0);
    out.param.assign(n * out.params * 4, 0);
    u32 vs_user = (r.sh[0x4b] >> 1) & 0x1f;
    u32 instances = std::max(r.instances, 1u);
    if (instances > 1) note("instanced draws (only the first instance drawn)");
    {
        Wave w;
        VsIo io(out);
        for (u32 at = 0; at < count; at += LANES) {
            w.s.fill(0);
            for (u32 k = 0; k < vs_user && k < 16; k++) w.s[k] = r.sh[0x4c + k];
            u32 lanes = std::min<u32>(LANES, count - at);
            for (u32 l = 0; l < lanes; l++) {
                w.v[0][l] = index[at + l] + reg(r, 0x102, 0);  // VGT_INDX_OFFSET
                w.v[1][l] = w.v[2][l] = w.v[3][l] = 0;  // the instance
            }
            w.set_exec(lanes == 64 ? ~0ull : (1ull << lanes) - 1);
            io.first = at;
            run(w, vs, io, mem);
        }
    }
    if (rects)  // the fourth corner: b + c - a
        for (size_t k = 0, extra = count; k + 2 < count; k += 3, extra++) {
            for (int c = 0; c < 4; c++)
                out.pos[extra * 4 + size_t(c)] = out.pos[(k + 1) * 4 + size_t(c)] + out.pos[(k + 2) * 4 + size_t(c)] - out.pos[k * 4 + size_t(c)];
            for (size_t p = 0; p < out.params * 4; p++) {
                auto at = [&](size_t v) { return out.param[v * out.params * 4 + p]; };
                out.param[extra * out.params * 4 + p] = at(k + 1) + at(k + 2) - at(k);
            }
        }
    if (!ps) return std::format("{} vertices (no pixel shader: nothing drawn)", count);
    // the targets, and the viewport and scissor
    Target targets[8];
    bool any = false;
    for (u32 k = 0; k < 8; k++) {
        targets[k] = target(r, k);
        targets[k].prepare(mem);
        any |= targets[k].count != 0;
    }
    u32 mode = (reg(r, 0x202, 0xcc0010) >> 4) & 7;
    if (!any || mode == 0) return std::format("{} vertices (no colour written)", count);
    if (mode != 1) {
        note(std::format("colour modes other than normal ({})", mode));
        return std::format("(colour mode {}: not drawn)", mode);
    }
    u32 vte = reg(r, 0x206, 0x43f);
    float xs = regf(r, 0x10f, 1), xo = regf(r, 0x110, 0), ys = regf(r, 0x111, 1), yo = regf(r, 0x112, 0),
          zs = regf(r, 0x113, 1), zo = regf(r, 0x114, 0);
    int x0 = 0, y0 = 0, x1 = 16384, y1 = 16384;
    auto clip = [&](u32 tl, u32 br) {
        x0 = std::max(x0, int(tl & 0x7fff));
        y0 = std::max(y0, int((tl >> 16) & 0x7fff));
        x1 = std::min(x1, int(br & 0x7fff));
        y1 = std::min(y1, int((br >> 16) & 0x7fff));
    };
    clip(reg(r, 0x0c, 0), reg(r, 0x0d, 0x40004000));  // screen scissor
    clip(reg(r, 0x90, 0), reg(r, 0x91, 0x40004000));  // generic scissor
    if (reg(r, 0x292, 0) & 2) clip(reg(r, 0x94, 0), reg(r, 0x95, 0x40004000));  // viewport 0's
    for (auto& t : targets)
        if (t.count) x1 = std::min(x1, int(t.pitch)), y1 = std::min(y1, int(t.height));
    u32 cull = reg(r, 0x205, 0);
    std::vector<Tri> tris;
    tris.reserve(tris_in.size() * (rects ? 2 : 1));
    u64 behind = 0;
    auto add = [&](size_t a, size_t b, size_t c) {
        Tri t;
        size_t v[3] = {a, b, c};
        for (int k = 0; k < 3; k++) {
            const float* p = &out.pos[v[k] * 4];
            float w = p[3];
            if (!(w > 0)) {
                behind++;
                return;
            }
            bool divide = !((vte >> 8) & 1);
            float nx = divide ? p[0] / w : p[0], ny = divide ? p[1] / w : p[1], nz = divide ? p[2] / w : p[2];
            t.x[k] = (vte & 1 ? nx * xs : nx) + (vte & 2 ? xo : 0);
            t.y[k] = (vte & 4 ? ny * ys : ny) + (vte & 8 ? yo : 0);
            t.z[k] = (vte & 16 ? nz * zs : nz) + (vte & 32 ? zo : 0);
            t.invw[k] = 1 / w;
            t.v[k] = v[k];
        }
        t.area = (t.x[1] - t.x[0]) * (t.y[2] - t.y[0]) - (t.x[2] - t.x[0]) * (t.y[1] - t.y[0]);
        if (t.area == 0 || !std::isfinite(t.area)) return;
        bool ccw = t.area < 0;  // screen y grows downwards
        t.front = (cull >> 2) & 1 ? !ccw : ccw;
        if ((t.front && (cull & 1)) || (!t.front && (cull & 2))) return;
        tris.push_back(t);
    };
    for (size_t k = 0; k < tris_in.size(); k++) {
        auto [a, b, c] = tris_in[k];
        add(a, b, c);
        if (rects) add(b, count + k, c);
    }
    if (behind) note("triangles with a vertex behind the eye (skipped)", behind);
    g_triangles += tris.size();
    if (tris.empty() || x0 >= x1 || y0 >= y1) return std::format("{} vertices, nothing on screen", count);
    // the pixels, in bands of rows, one band to a thread; within a band, triangle by triangle
    u32 ps_user = (r.sh[0x0b] >> 1) & 0x1f;
    u32 ena = reg(r, 0x1b3, 0), addr = reg(r, 0x1b4, 0);
    if (!addr) addr = ena;
    bool save = g_saved < SAVED && g_saved_by_ps[ps] < 2;
    int area[4] = {x0, y0, x1, y1};
    vkdraw::Draw check;
    std::string check_why, vertex_why;
    bool checked = false;
    if (save && M.gpu_check) {
        checked = gpu_check(check, check_why, vertex_why, r, vs, ps, out, tris, index, tris_in, rects, targets, area, ps_user,
                            ena, addr, mem);
        if (!checked) g_check_skipped[check_why]++;
        else if (vertex_why.empty()) g_check_gpu_vertices++;
    }
    Pool& p = pool();
    int bands = (y1 - y0) * (x1 - x0) > 64 * 64 ? p.n : 1;
    int qy0 = y0 & ~1, rows = ((y1 - qy0 + 1) / 2 + bands - 1) / bands * 2;
    std::atomic<u64> pixels{0}, waves{0};
    auto band = [&](int b) {
        if (b >= bands) return;
        int by0 = qy0 + b * rows, by1 = std::min(y1, by0 + rows);
        if (by0 >= by1) return;
        Wave w;
        PsIo io(r, mem, out, targets);
        int lanes = 0;
        auto flush = [&](const Tri& t) {
            if (!lanes) return;
            // the inputs, per SPI_PS_INPUT_ADDR's layout
            static const int sizes[16] = {2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
            float e[3];
            u32 vg = 0;
            for (int bit = 0; bit < 16; bit++) {
                if (!((addr >> bit) & 1)) continue;
                if ((ena >> bit) & 1) {
                    for (int l = 0; l < LANES; l++) {
                        float cx = float(io.px[l]) + 0.5f, cy = float(io.py[l]) + 0.5f;
                        e[0] = ((t.x[2] - t.x[1]) * (cy - t.y[1]) - (t.y[2] - t.y[1]) * (cx - t.x[1])) / t.area;
                        e[1] = ((t.x[0] - t.x[2]) * (cy - t.y[2]) - (t.y[0] - t.y[2]) * (cx - t.x[2])) / t.area;
                        e[2] = 1 - e[0] - e[1];
                        float pw = e[0] * t.invw[0] + e[1] * t.invw[1] + e[2] * t.invw[2];
                        float pi = e[1] * t.invw[1] / pw, pj = e[2] * t.invw[2] / pw;
                        size_t L = size_t(l);
                        switch (bit) {
                            case 0: case 1: case 2: w.v[vg][L] = u(pi); w.v[vg + 1][L] = u(pj); break;
                            case 4: case 5: case 6: w.v[vg][L] = u(e[1]); w.v[vg + 1][L] = u(e[2]); break;
                            case 8: w.v[vg][L] = u(cx); break;
                            case 9: w.v[vg][L] = u(cy); break;
                            case 10: w.v[vg][L] = u(e[0] * t.z[0] + e[1] * t.z[1] + e[2] * t.z[2]); break;
                            case 11: w.v[vg][L] = u(pw); break;
                            case 12: w.v[vg][L] = u(t.front ? 1.0f : -1.0f); break;
                            case 14: w.v[vg][L] = 1; break;
                            case 15: w.v[vg][L] = u32(io.px[l]) | (u32(io.py[l]) << 16); break;
                            default: w.v[vg][L] = 0;
                        }
                    }
                }
                vg += u32(sizes[bit]);
            }
            for (u32 k = 0; k < ps_user && k < 16; k++) w.s[k] = r.sh[0x0c + k];
            w.s[ps_user] = 0;  // the primitive mask
            w.set_exec(io.cover);
            io.tri = &t;
            run(w, ps, io, mem);
            pixels += u64(std::popcount(io.cover));
            waves++;
            lanes = 0;
            io.cover = 0;
        };
        for (const Tri& t : tris) {
            float bx0 = std::min({t.x[0], t.x[1], t.x[2]}), bx1 = std::max({t.x[0], t.x[1], t.x[2]});
            float bty0 = std::min({t.y[0], t.y[1], t.y[2]}), bty1 = std::max({t.y[0], t.y[1], t.y[2]});
            int ix0 = std::max(x0, int(std::floor(bx0))) & ~1, ix1 = std::min(x1, int(std::ceil(bx1)));
            int iy0 = std::max(by0, int(std::floor(bty0)) & ~1), iy1 = std::min(by1, int(std::ceil(bty1)));
            if (iy0 < by0) iy0 = by0;
            // the edges; a pixel on an edge belongs to one of the triangles that share it
            float sign = t.area > 0 ? 1.0f : -1.0f;
            auto owns = [&](int a, int b) {
                float dx = t.x[b] - t.x[a], dy = t.y[b] - t.y[a];
                dx *= sign, dy *= sign;
                return dy > 0 || (dy == 0 && dx < 0);
            };
            bool own[3] = {owns(1, 2), owns(2, 0), owns(0, 1)};
            for (int qy = iy0 & ~1; qy < iy1; qy += 2) {
                for (int qx = ix0; qx < ix1; qx += 2) {
                    u32 bits = 0;
                    for (int k = 0; k < 4; k++) {
                        int x = qx + (k & 1), y = qy + (k >> 1);
                        if (x < x0 || x >= x1 || y < y0 || y >= y1 || y < by0 || y >= by1) continue;
                        float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
                        float e0 = ((t.x[2] - t.x[1]) * (cy - t.y[1]) - (t.y[2] - t.y[1]) * (cx - t.x[1])) * sign;
                        float e1 = ((t.x[0] - t.x[2]) * (cy - t.y[2]) - (t.y[0] - t.y[2]) * (cx - t.x[2])) * sign;
                        float e2 = ((t.x[1] - t.x[0]) * (cy - t.y[0]) - (t.y[1] - t.y[0]) * (cx - t.x[0])) * sign;
                        if ((e0 > 0 || (e0 == 0 && own[0])) && (e1 > 0 || (e1 == 0 && own[1])) && (e2 > 0 || (e2 == 0 && own[2])))
                            bits |= 1u << k;
                    }
                    if (!bits) continue;
                    for (int k = 0; k < 4; k++) {
                        io.px[lanes + k] = qx + (k & 1);
                        io.py[lanes + k] = qy + (k >> 1);
                    }
                    io.cover |= u64(bits) << lanes;
                    lanes += 4;
                    if (lanes == LANES) flush(t);
                }
            }
            if (lanes) {
                for (int l = lanes; l < LANES; l++) io.px[l] = io.px[0], io.py[l] = io.py[0];
                flush(t);
            }
        }
    };
    if (bands == 1) band(0);
    else p.run(band);
    g_pixels += pixels;
    g_ps_waves += waves;
    std::string what = std::format("{} triangles, {} pixels", tris.size(), pixels.load());
    if (save) {
        g_saved_by_ps[ps]++;
        u64 number = g_saved++;
        auto dir = std::filesystem::path(M.out_dir) / "draws";
        std::filesystem::create_directories(dir);
        if (number == 0)  // an earlier run's pictures go
            for (const auto& e : std::filesystem::directory_iterator(dir))
                if (e.path().extension() == ".bmp") std::filesystem::remove(e.path());
        if (checked) {
            what += "; " + compare(number, check, targets[0], area);
            what += vertex_why.empty() ? ", vertices too" : "; vertices from the CPU: " + vertex_why;
        }
        else if (M.gpu_check) what += "; not on the GPU: " + check_why;
        for (auto& t : targets)
            if (t.count) {
                save_target(number, t, u32(x1), u32(y1), mem, std::format("vs {:#x}, ps {:#x}: {}", vs, ps, what));
                break;
            }
    }
    return what;
}

std::string gpu_dispatch(const GpuRegs& r, u32 gx, u32 gy, u32 gz) {
    if (!M.draw) return {};
    std::lock_guard draw_lock(g_draw_lock);
    Timer timer(g_dispatch_ns);
    u64 cs = program(r, 0x20c);
    if (!cs) return "(no compute shader)";
    g_dispatches++;
    Memory mem;
    mem.snapshot();
    u32 rsrc2 = r.sh[0x213];
    u32 user = (rsrc2 >> 1) & 0x1f;
    u32 tx = std::max(r.sh[0x207] & 0xffff, 1u), ty = std::max(r.sh[0x208] & 0xffff, 1u), tz = std::max(r.sh[0x209] & 0xffff, 1u);
    u32 threads = tx * ty * tz, nwaves = (threads + LANES - 1) / LANES;
    u64 groups = u64(gx) * gy * gz;
    std::atomic<u64> next{0};
    struct NoIo : Io {
        void exp(u32, const Vec* const[4], bool, u64) override { note("exports from a compute shader"); }
    };
    auto worker = [&](int) {
        NoIo io;
        std::vector<unsigned char> lds(64 * 1024);
        std::vector<Wave> waves(nwaves);
        for (u64 g; (g = next++) < groups;) {
            u32 id[3] = {u32(g % gx), u32((g / gx) % gy), u32(g / (u64(gx) * gy))};
            std::fill(lds.begin(), lds.end(), 0);
            std::vector<u64> pc(nwaves, cs);
            std::vector<bool> done(nwaves, false);
            for (u32 k = 0; k < nwaves; k++) {
                Wave& w = waves[k];
                w.s.fill(0);
                w.lds = &lds;
                u32 s = 0;
                for (; s < user && s < 16; s++) w.s[s] = r.sh[0x240 + s];
                for (int d = 0; d < 3; d++)
                    if ((rsrc2 >> (7 + d)) & 1) w.s[s++] = id[d];
                if ((rsrc2 >> 10) & 1) w.s[s++] = k;  // the wave's number in its group
                u64 exec = 0;
                for (u32 l = 0; l < LANES; l++) {
                    u32 t = k * LANES + l;
                    if (t >= threads) break;
                    exec |= 1ull << l;
                    w.v[0][l] = t % tx;
                    w.v[1][l] = (t / tx) % ty;
                    w.v[2][l] = t / (tx * ty);
                }
                w.set_exec(exec);
            }
            // the waves in turn, each to its end or the next barrier
            for (u32 left = nwaves; left;) {
                for (u32 k = 0; k < nwaves; k++) {
                    if (done[k]) continue;
                    Result res = run(waves[k], pc[k], io, mem);
                    if (res.stop == Stop::barrier) {
                        pc[k] = res.pc;
                    } else {
                        done[k] = true;
                        left--;
                    }
                }
            }
        }
    };
    if (groups * nwaves > 4) pool().run(worker);
    else worker(0);
    g_groups += groups;
    return std::format("{} groups run", groups);
}

std::vector<std::string> draw_report() {
    if (!M.draw) return {"## Drawing\n", "Not done (`--no-draw`).\n"};
    std::vector<std::string> s = {
        "## Drawing\n",
        std::format("Run on the CPU: {} draws, {} triangles, {} pixels ({} pixel-shader waves); {} dispatches, {} groups.\n",
                    g_draws.load(), g_triangles.load(), g_pixels.load(), g_ps_waves.load(), g_dispatches.load(), g_groups.load()),
        std::format("The draws took {:.1f} s and the dispatches {:.1f} s, of the run's {:.1f}.\n", double(g_draw_ns) / 1e9,
                    double(g_dispatch_ns) / 1e9,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - M.t0).count())};
    if (M.gpu_check) {
        std::lock_guard lock(g_draw_lock);
        std::string where = vkdraw::device();
        s.push_back(std::format("GPU check (`--gpu-check`){}: of the draws saved to draws/, {} were drawn on the GPU as well, "
                                "{} of them the same as the CPU's in every pixel (to 1 in 255), and {} with their vertex "
                                "shaders on the GPU too; see draws/index.txt.\n",
                                where.empty() ? "" : ", " + where, g_checked, g_check_same, g_check_gpu_vertices));
        if (!g_check_skipped.empty()) {
            s.insert(s.end(), {"Not drawn on the GPU:\n", "| Why | Draws |", "|---|---:|"});
            for (auto& [why, count] : g_check_skipped) s.push_back(std::format("| {} | {} |", why, count));
            s.push_back("");
        }
    }
    auto n = notes();
    if (!n.empty()) {
        s.insert(s.end(), {"Not done, or done only in part:\n", "| What | Times |", "|---|---:|"});
        for (auto& [what, count] : n) s.push_back(std::format("| {} | {} |", what, count));
        s.push_back("");
    }
    return s;
}

}  // namespace mensis
