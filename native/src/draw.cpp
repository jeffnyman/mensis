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
// Depth and stencil: a pixel's depth (its triangle's z, interpolated, clamped to the viewport's
// range) is tested against the depth buffer, and its stencil value (8 bits, in a buffer of its
// own) against the draw's reference, as Vulkan does it: the pixel shader runs for every pixel
// covered, colour is written for those that passed both, and for those the shader didn't kill
// depth is written (if they passed) and the stencil operation for how they fared is done. The
// depth buffer is 32-bit floats or 16-bit unorm, in rows, as the loader keeps targets.
//
// A triangle reaching past the guard band, or behind the eye, is clipped first, as the hardware
// does. A pixel shader may give its own depth (DB_SHADER_CONTROL's Z_EXPORT_ENABLE, mrtz's x):
// then it is tested once the shader has run. Not done yet: multisampling, mip levels past the
// first.
//
// Where there is Vulkan (the Windows build, unless --cpu), each draw whose pixel shader can be
// translated (translate.cpp) is drawn on the PC's GPU instead (vkdraw.cpp), into its render
// target kept there, which comes back into the game's memory when something touches it. So is
// each dispatch whose compute shader can be translated, and whose stores all go into render
// targets kept there. The CPU runs the rest.
//
// With --gpu-check, each draw saved to draws/ is drawn on the PC's GPU as well, into a copy
// of the render target as it was before; then the CPU draws it, and the two are compared
// pixel by pixel. If the vertex shader can't be translated, the GPU is given the CPU's
// vertices instead.

#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
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
    const char* doing = nullptr;  // what the thread that gave the job is doing (for the count of targets read back)
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
                        vkdraw::t_doing = doing;
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
            doing = vkdraw::t_doing;
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
// Where the draws' time goes, stage by stage: a draw's time goes to one stage at a time (Lap),
// and some stages' parts are timed as well (Timer), for the summary
enum Stage : int { ST_SETUP, ST_PREPARE, ST_GPU, ST_SAVE, ST_CPU_VERTICES, ST_CPU_TRIANGLES, ST_CHECK, ST_CPU_PIXELS, ST_COUNT };
const char* const stage_names[ST_COUNT] = {
    "setting up (the memory's snapshot, targets, depth buffer, HTILE)",
    "preparing for the GPU (translations, buffers, textures, vertices)",
    "the GPU's draws (recording, uploads, waiting)",
    "saving draws to draws/",
    "vertex shaders on the CPU",
    "triangles on the CPU (clipping, culling)",
    "--gpu-check's draws",
    "pixels on the CPU",
};
std::atomic<u64> g_stage_ns[ST_COUNT];
std::atomic<u64> g_texture_hash_ns{0}, g_texture_decode_ns{0}, g_buffer_unpack_ns{0}, g_translate_ns{0};
std::atomic<u64> g_texture_hash_bytes{0}, g_texture_decoded{0};
std::atomic<u64> g_texture_blocks{0}, g_texture_blocks_ns{0};  // textures sent as their compressed blocks
std::atomic<u64> g_texture_copied{0}, g_texture_copy_ns{0};  // and as their texels, as they are in memory
// the textures decoded texel by texel, by their format, tiling and type: how many, how long
std::map<std::string, std::pair<u64, u64>> g_decoded_by;
struct Lap {
    int stage;
    const char* was = vkdraw::t_doing;  // what this thread was doing before
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit Lap(int s) : stage(s) { vkdraw::t_doing = stage_names[s]; }
    void to(int s) {
        auto now = std::chrono::steady_clock::now();
        g_stage_ns[stage] += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t0).count());
        stage = s;
        t0 = now;
        vkdraw::t_doing = stage_names[s];
    }
    ~Lap() {
        to(stage);
        vkdraw::t_doing = was;
    }
};
std::atomic<u64> g_clipped{0};  // triangles clipped to the guard band (on the CPU)
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

u64 g_draw_clears = 0;  // depth and stencil buffers cleared by a draw (clear_by_draw())
u64 g_depth_clears = 0;  // depth buffers cleared (HTILE: expand_htile())

// The depth buffer, from DB_DEPTH_CONTROL (0x200: the test on, writes on, the compare), DB_Z_INFO
// (0x010: the format, 1 16-bit unorm or 3 32-bit float), DB_Z_READ_BASE / DB_Z_WRITE_BASE
// (0x012, 0x014), DB_DEPTH_SIZE (0x016: the pitch and height, in 8s), DB_DEPTH_VIEW (0x002:
// the first slice, of an array), and the viewport's depth range (0x0b4, 0x0b5)
struct DepthBuffer {
    bool test = false, write = false;  // (the console writes depth only with the test on)
    u32 func = 7, format = 0, bpp = 4, pitch = 0, height = 0;
    u64 base = 0;
    float zmin = 0, zmax = 1;
    // The stencil buffer (8 bits a pixel, the depth buffer's size, at sbase), and each face's
    // test and operations: DB_DEPTH_CONTROL (0x200: on, back faces their own, the compares),
    // DB_STENCIL_CONTROL (0x10b: on failing the stencil test, passing both, failing the depth
    // test; front, then back), DB_STENCILREFMASK and _BF (0x10c, 0x10d: the reference, the
    // compare mask, the write mask, the operations' value)
    bool stencil = false;  // the test on
    bool has_stencil = false;  // a stencil buffer there (tested or not)
    u64 sbase = 0;
    struct Face {
        u32 func = 7, fail = 0, zpass = 0, zfail = 0, ref = 0, mask = 255, wmask = 255, opval = 0;
    } front, back;
    bool any() const { return test || stencil; }
    // whether any stencil operation writes anything
    bool stencil_writes() const {
        auto w = [](const Face& f) { return f.wmask && (f.fail || f.zpass || f.zfail); };
        return stencil && (w(front) || w(back));
    }
    bool stencil_pass(int x, int y, bool is_front) const {
        const Face& f = is_front ? front : back;
        u32 v = *reinterpret_cast<const unsigned char*>(sbase + u64(y) * pitch + u64(x));
        return compare_with(f.func, float(f.ref & f.mask), float(v & f.mask));
    }
    // outcome: 0 passed both, 1 failed the depth test, 2 failed the stencil test
    void stencil_op(int x, int y, bool is_front, int outcome) const {
        const Face& f = is_front ? front : back;
        auto* p = reinterpret_cast<unsigned char*>(sbase + u64(y) * pitch + u64(x));
        u32 v = *p, op = outcome == 2 ? f.fail : outcome == 1 ? f.zfail : f.zpass, n = v;
        switch (op) {
            case 0: n = v; break;
            case 1: n = 0; break;
            case 2: n = 255; break;
            case 3: n = f.ref; break;
            case 4: n = f.opval; break;
            case 5: n = std::min(v + f.opval, 255u); break;
            case 6: n = v > f.opval ? v - f.opval : 0; break;
            case 7: n = ~v; break;
            case 8: n = v + f.opval; break;
            case 9: n = v - f.opval; break;
            case 10: n = v & f.opval; break;
            case 11: n = v | f.opval; break;
            case 12: n = v ^ f.opval; break;
            case 13: n = ~(v & f.opval); break;
            case 14: n = ~(v | f.opval); break;
            default: n = ~(v ^ f.opval); break;
        }
        *p = static_cast<unsigned char>(((v & ~f.wmask) | (n & f.wmask)) & 255);
    }
    static bool compare_with(u32 fn, float a, float b) {
        switch (fn) {
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
    u64 at(int x, int y) const { return base + (u64(y) * pitch + u64(x)) * bpp; }
    // Whether a pixel's depth passes, and its value as stored
    bool pass(float z, int x, int y, u32& stored) const {
        u64 a = at(x, y);
        if (format == 3) {
            float old;
            std::memcpy(&old, reinterpret_cast<const void*>(a), 4);
            stored = u(z);
            return compare(z, old);
        }
        std::uint16_t old;
        std::memcpy(&old, reinterpret_cast<const void*>(a), 2);
        stored = u32(std::nearbyint(std::clamp(z, 0.0f, 1.0f) * 65535.0f));
        return compare(float(stored), float(old));
    }
    bool compare(float a, float b) const { return compare_with(func, a, b); }
    void store(int x, int y, u32 v) const { std::memcpy(reinterpret_cast<void*>(at(x, y)), &v, bpp); }
};

DepthBuffer depth_buffer(const GpuRegs& r, const Memory& mem) {
    DepthBuffer d;
    u32 ctl = reg(r, 0x200, 0), info = reg(r, 0x010, 0), format = info & 3;
    bool z = (ctl >> 1) & 1, s = ctl & 1;
    if ((!z && !s) || (format != 1 && format != 3)) return d;
    u64 read = u64(reg(r, 0x012, 0)) << 8, write = u64(reg(r, 0x014, 0)) << 8;
    if (read != write) {
        note("depth buffers read from one place and written to another (drawn without depth)");
        return d;
    }
    u32 size = reg(r, 0x016, 0);
    d.format = format;
    d.bpp = format == 1 ? 2 : 4;
    d.pitch = ((size & 0x7ff) + 1) * 8;
    d.height = (((size >> 11) & 0x7ff) + 1) * 8;
    d.base = read + u64(reg(r, 0x002, 0) & 0x7ff) * d.pitch * d.height * d.bpp;
    if (!mem.ok(d.base, u64(d.pitch) * d.height * d.bpp)) {
        note("depth buffers outside the GPU's memory (drawn without depth)");
        return d;
    }
    d.test = z;
    d.write = z && ((ctl >> 2) & 1);
    d.func = (ctl >> 4) & 7;
    d.zmin = regf(r, 0x0b4, 0);
    d.zmax = regf(r, 0x0b5, 1);
    u64 sread = u64(reg(r, 0x013, 0)) << 8, swrite = u64(reg(r, 0x015, 0)) << 8;
    d.sbase = sread + u64(reg(r, 0x002, 0) & 0x7ff) * d.pitch * d.height;
    d.has_stencil = (reg(r, 0x011, 0) & 1) && sread && sread == swrite && mem.ok(d.sbase, u64(d.pitch) * d.height);
    if (s) {
        if (!d.has_stencil) {
            note("stencil buffers not 8 bits, not read and written at one place, or outside the GPU's memory (drawn without stencil)");
        } else {
            d.stencil = true;
            u32 sc = reg(r, 0x10b, 0), rm = reg(r, 0x10c, 0), rmb = reg(r, 0x10d, 0);
            auto face = [&](u32 func, u32 ops, u32 refmask) {
                return DepthBuffer::Face{func, ops & 15, (ops >> 4) & 15, (ops >> 8) & 15, refmask & 255, (refmask >> 8) & 255, (refmask >> 16) & 255,
                            (refmask >> 24) & 255};
            };
            d.front = face((ctl >> 8) & 7, sc, rm);
            d.back = (ctl >> 7) & 1 ? face((ctl >> 20) & 7, sc >> 12, rmb) : d.front;
        }
    }
    if (!d.test && !d.stencil) return DepthBuffer{};
    return d;
}

u64 hash_memory(const unsigned char* p, u64 n);
u64 memory_stamp(u64 base, u64 n);

// A depth buffer's fast clear. With HTILE on (DB_Z_INFO bit 29), a clear only marks each 8x8
// tile cleared in the HTILE buffer (DB_HTILE_DATA_BASE, 0x005: a 32-bit word a tile, its low 4
// bits, the z mask, 0), and the console reads a cleared tile as DB_DEPTH_CLEAR (0x00b). Here,
// when every tile is marked cleared, the depth buffer is filled with that value (on the GPU,
// if it is kept there), once for each time the HTILE buffer is written: what it held then is
// remembered (its version on the GPU, or a hash of its memory), and until it changes, it has
// been seen to. Some tiles cleared and some not would need the HTILE buffer's own layout: not
// done yet.
std::map<u64, u64> g_htile_seen;  // an HTILE buffer's address: what it held when last seen to

void expand_htile(const GpuRegs& r, const Memory& mem, const DepthBuffer& db) {
    if (!db.any() || !((reg(r, 0x010, 0) >> 29) & 1)) return;
    u64 h = u64(reg(r, 0x005, 0)) << 8;
    u64 tiles = u64(db.pitch / 8) * (db.height / 8);
    if (!h || !mem.ok(h, tiles * 4)) return;
    u64 v = M.gpu ? vkdraw::version(h) : 0;
    u64 stamp = v ? v << 1 : memory_stamp(h, tiles * 4) | 1;
    u64& seen = g_htile_seen[h ^ db.base];
    if (seen == stamp) return;
    seen = stamp;
    float clear = regf(r, 0x00b, 1);
    // both kept on the GPU: seen to there, rather than the HTILE buffer read back
    if (v && vkdraw::clear_depth_by_htile(h, tiles, db.base, db.pitch, db.height, clear)) {
        return;
    }
    const auto* w = reinterpret_cast<const u32*>(h);
    u64 cleared = 0;
    for (u64 i = 0; i < tiles; i++) cleared += (w[i] & 15) == 0;
    if (!cleared) return;
    if (cleared != tiles) {
        note("depth buffers partly cleared (HTILE): not cleared");
        return;
    }
    u64 n = u64(db.pitch) * db.height;
    if (M.gpu && vkdraw::clear_depth(db.base, db.pitch, db.height, clear)) {  // kept on the GPU: cleared there
    } else if (db.format == 3) {
        std::vector<float> v(n, clear);
        std::memcpy(reinterpret_cast<void*>(db.base), v.data(), n * 4);
    } else {
        std::vector<std::uint16_t> v(n, std::uint16_t(std::nearbyint(std::clamp(clear, 0.0f, 1.0f) * 65535.0f)));
        std::memcpy(reinterpret_cast<void*>(db.base), v.data(), n * 2);
    }
    g_depth_clears++;
}

// A clear by a draw: with DB_RENDER_CONTROL's (0x000) DEPTH_CLEAR_ENABLE (bit 0) or
// STENCIL_CLEAR_ENABLE (bit 1) on, the draw writes DB_DEPTH_CLEAR (0x00b), DB_STENCIL_CLEAR
// (0x00a) wherever it covers, rather than its own depth (with HTILE, the console only marks the
// tiles cleared). The draws are rectangles over what is to be cleared; here the scissors say
// how much of the buffer that is (all of it, or a part, one shadow map of several in one depth
// buffer, say), and it is filled with the values, on the GPU if the buffer is kept there, else
// in memory. Returns what was done, or empty if it was no clear.
std::string clear_by_draw(const GpuRegs& r, const Memory& mem) {
    u32 rc = reg(r, 0x000, 0);
    if (!(rc & 3)) return {};
    u32 info = reg(r, 0x010, 0), format = info & 3, size = reg(r, 0x016, 0);
    u64 read = u64(reg(r, 0x012, 0)) << 8, sread = u64(reg(r, 0x013, 0)) << 8;
    if (format != 1 && format != 3) format = 0;
    u32 bpp = format == 1 ? 2 : 4, pitch = ((size & 0x7ff) + 1) * 8, height = (((size >> 11) & 0x7ff) + 1) * 8;
    u32 slice = reg(r, 0x002, 0) & 0x7ff;
    u64 base = read + u64(slice) * pitch * height * bpp, sbase = sread + u64(slice) * pitch * height;
    bool depth = (rc & 1) && format && read && mem.ok(base, u64(pitch) * height * bpp);
    bool stencil = (rc & 2) && (reg(r, 0x011, 0) & 1) && sread && mem.ok(sbase, u64(pitch) * height);
    if (!depth && !stencil) return {};
    int x0 = 0, y0 = 0, x1 = 16384, y1 = 16384;
    auto clip = [&](u32 tl, u32 br) {
        x0 = std::max(x0, int(tl & 0x7fff)), y0 = std::max(y0, int((tl >> 16) & 0x7fff));
        x1 = std::min(x1, int(br & 0x7fff)), y1 = std::min(y1, int((br >> 16) & 0x7fff));
    };
    clip(reg(r, 0x0c, 0), reg(r, 0x0d, 0x40004000));
    clip(reg(r, 0x90, 0), reg(r, 0x91, 0x40004000));
    if (reg(r, 0x292, 0) & 2) clip(reg(r, 0x94, 0), reg(r, 0x95, 0x40004000));
    x1 = std::min(x1, int(pitch)), y1 = std::min(y1, int(height));
    if (x0 >= x1 || y0 >= y1) return "a clear by a draw, of nothing";
    float z = regf(r, 0x00b, 1);
    int s = stencil ? int(reg(r, 0x00a, 0) & 0xff) : -1;
    if (!(M.gpu && vkdraw::clear_depth(base, pitch, height, z, s, depth, u32(x0), u32(y0), u32(x1), u32(y1)))) {
        // in memory, in rows: each row's stretch
        size_t w = size_t(x1 - x0);
        std::vector<float> zf(depth && format == 3 ? w : 0, z);
        std::vector<std::uint16_t> zh(depth && format == 1 ? w : 0, std::uint16_t(std::nearbyint(std::clamp(z, 0.0f, 1.0f) * 65535.0f)));
        for (int y = y0; y < y1; y++) {
            u64 row = u64(y) * pitch + u64(x0);
            if (!zf.empty()) std::memcpy(reinterpret_cast<void*>(base + row * 4), zf.data(), w * 4);
            if (!zh.empty()) std::memcpy(reinterpret_cast<void*>(base + row * 2), zh.data(), w * 2);
            if (stencil) std::memset(reinterpret_cast<void*>(sbase + row), s, w);
        }
    }
    g_draw_clears++;
    return std::format("a clear by a draw: {}{}{} ({}x{} at {}, {}, of {}x{})", depth ? std::format("depth to {}", z) : "",
                       depth && stencil ? ", " : "", stencil ? std::format("stencil to {}", s) : "", x1 - x0, y1 - y0, x0, y0, pitch,
                       height);
}

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
    u64 exported = 0;  // lanes that exported anything (to any target, null included): not killed
    // the depth the shader exported (mrtz's x), if DB_SHADER_CONTROL's Z_EXPORT_ENABLE is on,
    // and which lanes did
    bool z_export = false;
    u32 zexp[LANES] = {};
    u64 zset = 0;
    PsIo(const GpuRegs& r_, const Memory& m, const Vertices& v, const Target* t)
        : r(r_), mem(m), vs(v), targets(t) {
        col_format = reg(r, 0x1c5, 0);
        z_export = reg(r, 0x203, 0) & 1;
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
        exported |= lanes;
        if (target == 8 && z_export && regs[0] && !compr) {
            for (int l = 0; l < LANES; l++)
                if ((lanes >> l) & 1) zexp[l] = (*regs[0])[size_t(l)];
            zset |= lanes;
        }
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
u64 g_checked = 0, g_check_same = 0, g_check_gpu_vertices = 0, g_check_depth_differ = 0;
std::map<std::string, u64> g_check_skipped;
u64 g_gpu_draws = 0, g_gpu_vertices = 0;  // drawn on the GPU
std::map<std::string, u64> g_gpu_skipped;
// the draws whose vertex shaders the CPU ran, by why: how many, their vertices, the seconds
std::map<std::string, std::array<u64, 3>> g_vertex_skipped;
u64 g_gpu_dispatches = 0, g_gpu_groups = 0;  // run on the GPU
std::map<std::string, std::array<u64, 3>> g_gpu_dispatch_skipped;  // by why: the dispatches, their groups, the CPU's time on them
// --gpu-check: dispatches done on the GPU and then the CPU, what they stored compared (so many
// of a shader, so many in all); by the shader's address, how many, and how many the same
constexpr u64 DISPATCH_CHECKS = 400, CHECKS_A_SHADER = 20;
u64 g_dispatch_checks = 0;
std::map<u64, std::pair<u64, u64>> g_dispatch_checked;

// A hash of n bytes of memory, to tell whether a texture changed
u64 hash_memory(const unsigned char* p, u64 n) {
    u64 h[4] = {0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull};
    u64 i = 0;
    for (; i + 32 <= n; i += 32)
        for (int k = 0; k < 4; k++) {
            u64 w;
            std::memcpy(&w, p + i + 8 * k, 8);
            h[k] = (h[k] ^ w) * 0x100000001b3ull;
            h[k] ^= h[k] >> 29;
        }
    for (; i < n; i++) h[0] = (h[0] ^ p[i]) * 0x100000001b3ull;
    return h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7) ^ n;
}

// What n bytes of memory hold, as a hash of them (for the textures and buffers kept from draw
// to draw), hashed again only when they may have changed. In the heap, on Windows, the OS notes
// the pages written: each time some are found written, they are given a new number, and while
// the highest number of a range's pages is what it was when the range was last hashed, the
// range is as it was, and its hash is too. Memory written with what it held already still
// hashes the same.
std::mutex g_pages_lock;
std::vector<u32> g_page_written, g_chunk_written;  // the heap's pages' numbers, and the highest of each 2 MiB
u32 g_page_writes = 0;
bool g_watched = false, g_unwatched = false;  // pages found written at least once; never (not watched)
struct Hashed {
    u64 base, n, written, hash;
};
std::unordered_map<u64, Hashed> g_hashed;  // by range (its start and length, mixed): its pages' number then, and its hash
u64 g_stamps_watched = 0, g_stamps_hashed = 0, g_hashed_bytes = 0;
u64 page_written(u64 base, u64 n);
u64 memory_stamp(u64 base, u64 n) {
    vkdraw::Doing doing("hashing memory (to tell whether it changed)");
    u64 written = page_written(base, n);
    if (written != ~u64(0)) {
        std::lock_guard lock(g_pages_lock);
        u64 range = base ^ (n * 0x9e3779b97f4a7c15ull);
        auto it = g_hashed.find(range);
        if (it != g_hashed.end() && it->second.base == base && it->second.n == n && it->second.written == written) {
            g_stamps_watched++;
            return it->second.hash;
        }
        if (g_hashed.size() > (1u << 20)) g_hashed.clear();
        u64 h = hash_memory(reinterpret_cast<const unsigned char*>(base), n);
        g_hashed[range] = {base, n, written, h};
        g_stamps_hashed++, g_hashed_bytes += n;
        return h;
    }
    g_stamps_hashed++, g_hashed_bytes += n;
    return hash_memory(reinterpret_cast<const unsigned char*>(base), n);
}

// The highest number of the pages of [base, base + n) (in the heap, watched); ~0 if not told
std::atomic<u64> g_os_asked{0}, g_os_ask_ns{0};  // the OS asked which pages were written: how often, the time
std::atomic<u64> g_page_epoch{1};  // one for each draw and dispatch: the OS is asked once in each
std::vector<u64> g_chunk_asked;  // each 2 MiB of the heap: the epoch the OS was last asked about it in
u64 page_written(u64 base, u64 n) {
    if (!g_unwatched && n && base >= M.heap_base && base + n <= M.heap_base + HEAP_RESERVE) {
        static std::vector<u64> pages;
        std::lock_guard lock(g_pages_lock);
        u64 lo = base & ~u64(0xfff), hi = (base + n + 0xfff) & ~u64(0xfff);
        bool told = true;
        if (g_chunk_asked.empty()) g_chunk_asked.assign(HEAP_RESERVE >> 21, 0);
        // the pages written since last asked, of each 2 MiB of the range not asked about yet in
        // this epoch (in one call, those next to each other)
        u64 c0 = (lo - M.heap_base) >> 21, c1 = (hi - M.heap_base + (1u << 21) - 1) >> 21;
        for (u64 c = c0; c < c1 && told;) {
            if (g_chunk_asked[c] == g_page_epoch) {
                c++;
                continue;
            }
            u64 e = c;
            while (e < c1 && g_chunk_asked[e] != g_page_epoch) g_chunk_asked[e++] = g_page_epoch;
            auto t0 = std::chrono::steady_clock::now();
            told = platform::written_pages(M.heap_base + (c << 21), (e - c) << 21, pages);
            g_os_asked++;
            g_os_ask_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            if (told) {
                if (!g_watched) {
                    g_page_written.assign(HEAP_RESERVE >> 12, 0);
                    g_chunk_written.assign(HEAP_RESERVE >> 21, 0);
                    g_watched = true;
                }
                if (!pages.empty()) {
                    g_page_writes++;
                    for (u64 p : pages) {
                        u64 i = (p - M.heap_base) >> 12;
                        g_page_written[i] = g_chunk_written[i >> 9] = g_page_writes;
                    }
                }
            }
            c = e;
        }
        if (told) {
            u32 most = 0;
            for (u64 i = (lo - M.heap_base) >> 12, end = (hi - M.heap_base) >> 12; i < end;)
                if (!(i & 511) && i + 512 <= end) most = std::max(most, g_chunk_written[i >> 9]), i += 512;
                else most = std::max(most, g_page_written[i++]);
            return most;
        }
        if (!g_watched) g_unwatched = true;
    }
    return ~u64(0);
}

// A shader's translation, beside its disassembly: shaders/<vs|ps>-<address>.glsl, the first
// time it is translated in a run
void save_glsl(const char* kind, u64 at, const std::string& text) {
    static std::set<std::pair<char, u64>> saved;  // under g_draw_lock
    if (!saved.insert({kind[0], at}).second) return;
    std::string name = std::format("{}-{:x}.glsl", kind, at);
    auto dir = std::filesystem::path(M.out_dir) / "shaders";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / name, std::ios::binary) << text;
}

// The vertex shader, for the GPU: translated, its buffers read, the triangles' indices, the
// viewport. False (and why) if it can't be.
// Each buffer load's elements, as the CPU's interpreter unpacks them, and then what an index
// past the end reads; false (and why) if there are too many
// Kept, unpacked, to be used again: by the descriptor, the offset and format, and a hash of the
// memory read (up to 2 GiB of them; then the least lately used go, down to 1 GiB)
u64 g_unpacked_bytes = 0, g_unpack_hits = 0, g_unpacks = 0, g_unpack_clears = 0;
struct Unpacked {
    std::shared_ptr<const std::vector<u32>> data;  // shared with the draws that use it, not copied
    u64 used = 0;  // when last found (g_unpack_hits + g_unpacks then)
};
// (the key: the V#, the offset, the format, the memory's hash)
struct UnpackedKey {
    u32 v[4], offset, dfmt, nfmt;
    u64 hash;
    bool operator==(const UnpackedKey&) const = default;
};
struct UnpackedKeyHash {
    size_t operator()(const UnpackedKey& k) const {
        u64 h = k.hash;
        for (u32 x : {k.v[0], k.v[1], k.v[2], k.v[3], k.offset, k.dfmt, k.nfmt}) h = (h ^ x) * 0x100000001b3ull;
        return size_t(h ^ (h >> 29));
    }
};
std::unordered_map<UnpackedKey, Unpacked, UnpackedKeyHash> g_unpacked;
constexpr u64 UNPACKED_BUDGET = u64(2) << 30;

// Over the budget: the least lately used go, down to half of it
void unpacked_let_go() {
    std::vector<std::pair<u64, const UnpackedKey*>> by_age;
    for (auto& [key, e] : g_unpacked) by_age.emplace_back(e.used, &key);
    std::sort(by_age.begin(), by_age.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<UnpackedKey> doomed;
    u64 bytes = g_unpacked_bytes;
    for (auto& [used, key] : by_age) {
        if (bytes <= UNPACKED_BUDGET / 2) break;
        bytes -= g_unpacked[*key].data->size() * 4;
        doomed.push_back(*key);
    }
    for (auto& key : doomed) g_unpacked.erase(key);
    g_unpacked_bytes = bytes;
    g_unpack_clears++;
}
std::atomic<u64> g_buffer_hash_ns{0}, g_buffer_miss_ns{0}, g_buffer_hash_bytes{0};

bool unpack_buffers(const std::vector<translate::Values::Buffer>& buffers, const Memory& mem, std::vector<vkdraw::Buffer>& out,
                    std::string& why) {
    Timer timer(g_buffer_unpack_ns);
    for (const auto& b : buffers) {
        u32 stride = (b.v[1] >> 16) & 0x3fff, records = b.v[2];
        u64 n = stride == 0 ? 1 : records;
        if (n > (1u << 22)) {
            why = "buffers of over 4 million elements";
            return false;
        }
        u64 base = b.v[0] | (u64(b.v[1] & 0xfff) << 32), span = (stride ? u64(stride) * records : records) + b.offset + 16;
        std::optional<UnpackedKey> key;
        if (mem.ok(base, span)) {
            u64 h;
            {
                Timer hashing(g_buffer_hash_ns);
                u64 before = g_hashed_bytes;
                h = memory_stamp(base, span);
                g_buffer_hash_bytes += g_hashed_bytes - before;
            }
            key = UnpackedKey{{b.v[0], b.v[1], b.v[2], b.v[3]}, b.offset, b.dfmt, b.nfmt, h};
            if (auto it = g_unpacked.find(*key); it != g_unpacked.end()) {
                it->second.used = g_unpack_hits + g_unpacks;
                out.push_back(it->second.data);
                g_unpack_hits++;
                continue;
            }
        }
        g_unpacks++;
        Timer missing(g_buffer_miss_ns);
        auto made = std::make_shared<std::vector<u32>>((n + 1) * 4);
        std::vector<u32>& data = *made;
        vkdraw::Doing doing("unpacking a buffer");
        for (u32 i = 0; i < n; i++) buffer_element(b.v, b.dfmt, b.nfmt, b.offset, i, mem, &data[size_t(i) * 4]);
        if (stride == 0) std::copy_n(data.begin(), 4, data.begin() + 4);
        else buffer_element(b.v, b.dfmt, b.nfmt, b.offset, records, mem, &data[n * 4]);
        if (key) {
            g_unpacked_bytes += data.size() * 4;
            g_unpacked[*key] = {made, g_unpack_hits + g_unpacks};
            if (g_unpacked_bytes > UNPACKED_BUDGET) unpacked_let_go();
        }
        out.push_back(std::move(made));
    }
    return true;
}

// The translations kept, by the program and what else they depend on
std::unordered_map<std::string, translate::Glsl> g_glsl;

// The CPU's time preparing a draw for the GPU, by part (each from its mark to the next; a
// part named null isn't counted: another's parts are, below it)
// (and setting a draw up, the same way)
std::map<std::string, u64> g_prepare_ns, g_setup_ns;
struct PrepareParts {
    const char* what;
    std::map<std::string, u64>& into;
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    explicit PrepareParts(const char* w, std::map<std::string, u64>& m = g_prepare_ns) : what(w), into(m) {}
    void to(const char* w) {
        auto now = std::chrono::steady_clock::now();
        if (what) into[what] += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count());
        what = w, t = now;
    }
    ~PrepareParts() { to(nullptr); }
};

bool gpu_vertices(vkdraw::Draw& d, std::string& why, const GpuRegs& r, u64 vs, u32 attributes, const Target& t,
                  const std::vector<u32>& index, const std::vector<std::array<size_t, 3>>& tris_in, bool rects, const Memory& mem) {
    PrepareParts part("the vertex shader: decoded (its fetch shader with it)");
    u32 users = std::min((r.sh[0x4b] >> 1) & 0x1f, 16u);
    u32 user[16];
    for (u32 k = 0; k < 16; k++) user[k] = r.sh[0x4c + k];
    translate::Program prog = translate::decode(vs, mem, user, users);
    part.to("the vertex shader: its scalars worked out");
    translate::Values vals;
    {
        Timer timer(g_translate_ns);
        vals = translate::evaluate(prog, user, users, mem);
    }
    part.to("the vertex shader: its GLSL found (or translated)");
    if (prog.why.empty() && !vals.why.empty()) prog.why = vals.why;
    translate::VsState st;
    st.user_count = users;
    st.attributes = attributes;
    for (u32 k = 0; k < 32; k++) st.input_cntl[k] = reg(r, 0x191 + k, k);
    for (size_t k = 0; k < vals.buffers.size() && k < 64; k++)
        if ((vals.buffers[k].v[3] >> 23) & 1) st.add_tid |= 1ull << k;
    // the program, as decoded (its fetch shader's code included), and the state: translated once
    u64 code_hash = 0x9e3779b97f4a7c15ull;
    for (const auto& in : prog.code) code_hash = (code_hash ^ in.w0 ^ (u64(in.w1) << 32) ^ (u64(in.lit) << 16)) * 0x100000001b3ull;
    std::string gkey = std::format("vs:{:x}:{:x}:{}:{}", vs, code_hash, prog.why, st.key());
    auto git = g_glsl.find(gkey);
    {
        Timer timer(g_translate_ns);
        if (git == g_glsl.end()) git = g_glsl.emplace(gkey, translate::vertex_shader(prog, st)).first;
    }
    if (!git->second.id) git->second.id = std::hash<std::string>{}(git->second.text) | 1;
    const translate::Glsl& g = git->second;
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
    part.to("the vertex shader: its buffers unpacked");
    if (!unpack_buffers(vals.buffers, mem, d.vs_buffers, why)) return false;
    part.to("the vertex shader: indices, GLSL copied, values");
    u32 offset = reg(r, 0x102, 0);  // VGT_INDX_OFFSET
    d.indices.resize(tris_in.size() * 3);
    u32* o = d.indices.data();
    for (const auto& tri : tris_in)
        for (size_t k : tri) *o++ = index[k] + offset;
    if (rects) {  // (made once for each number of attributes)
        static std::map<u32, std::string> made;
        auto it = made.find(attributes);
        if (it == made.end()) it = made.emplace(attributes, translate::rect_geometry_shader(attributes)).first;
        d.gs = it->second;
    }
    save_glsl("vs", vs, g.text);
    d.vs_kept = &g.text;  // (g_glsl's entries stay where they are, for the run)
    d.vs_id = g.id;
    d.vs_values = std::move(vals.u);
    d.cull = reg(r, 0x205, 0);
    return true;
}

// The draw, as vkdraw needs it (but for its target's memory); false (and why) if it can't be.
// vertex_why: why the vertices are the CPU's, if they are.
// A render target's format as Vulkan's (and its bytes a pixel), or 0 if there is none for it:
// 8-bit RGBA, linear or sRGB, in either order (COMP_SWAP alt: B first); 16-bit floats, two
// or four; 11- and 10-bit floats (red in the low bits)
u32 vk_format(u32 format, u32 nfmt, u32 swap, u32& bpp) {
    bpp = 4;
    if (format == 10 && (nfmt == 0 || nfmt == 9) && swap <= 1) return swap == 0 ? (nfmt == 9 ? 43 : 37) : (nfmt == 9 ? 50 : 44);
    if (swap != 0 || nfmt != 7) return 0;
    if (format == 4) return 100;  // R32_SFLOAT
    if (format == 5) return 83;  // R16G16_SFLOAT
    if (format == 6) return 122;  // B10G11R11_UFLOAT_PACK32
    if (format == 12) return bpp = 8, 97;  // R16G16B16A16_SFLOAT
    return 0;
}
// (a texture of 32-bit floats too: a depth buffer, read as one)

// Each image instruction's descriptors, into the translation's state; those that read the
// same texture the same way share it: the textures, one each
std::vector<const translate::Values::Image*> sort_images(const std::vector<translate::Values::Image>& images, translate::PsState& st) {
    std::vector<const translate::Values::Image*> unique;
    for (auto& im : images) {
        u32 type = im.t[3] >> 28;
        st.image_types.push_back(type < 8 ? 9 : type);  // 0 (a null descriptor, all 0s): read as 2D, as the CPU does
        st.unnormalized.push_back((im.s[0] >> 15) & 1);
        u32 nfmt = (im.t[1] >> 26) & 0xf;
        st.integer.push_back(nfmt == 4 || nfmt == 5);
        st.compare.push_back(im.compare ? ((im.s[0] >> 12) & 7) | (((im.s[2] >> 20) & 3) ? 8u : 0u) | (nfmt == 0 ? 16u : 0u) : 0u);
        u32 slot = 0;
        while (slot < unique.size() && (std::memcmp(unique[slot]->t, im.t, sizeof im.t) || std::memcmp(unique[slot]->s, im.s, sizeof im.s)))
            slot++;
        if (slot == unique.size()) unique.push_back(&im);
        st.image_slot.push_back(slot);
    }
    return unique;
}

// The textures a draw's or a dispatch's shader reads, from their descriptors
bool textures_for(const std::vector<const translate::Values::Image*>& unique, const Memory& mem, std::vector<vkdraw::Texture>& out,
                  std::string& why) {
    // the textures: 8-bit RGBA in rows as it is in memory, else each texel as the CPU's
    // interpreter reads it, which is kept on the GPU under a hash of the memory it reads
    for (const translate::Values::Image* one : unique) {
        const auto& im = *one;
        Image ti(im.t);
        ti.prepare(mem);
        vkdraw::Texture tx;
        tx.type = ti.type < 8 ? 9 : ti.type;
        tx.width = ti.width;
        tx.height = ti.type == 8 || ti.type == 12 ? 1 : ti.height;
        // a cube's faces: the slices from base_array to last_array, as the CPU reads them
        bool cube = ti.type == 11;
        tx.layers = ti.array() ? ti.last_array - ti.base_array + 1 : cube ? std::max(ti.last_array, ti.base_array) - ti.base_array + 1
                    : ti.type == 10 ? ti.depth : 1;
        tx.integer = ti.nfmt == 4 || ti.nfmt == 5;
        if (ti.type < 8) {  // not an image (a null descriptor, or one not set): it reads 0, as on the CPU
            tx.type = 9;
            tx.width = tx.height = tx.layers = 1;
            std::memcpy(tx.sampler, im.s, sizeof tx.sampler);
            tx.key = tx.integer ? "zero:integer" : "zero";
            if (!vkdraw::has_texture(tx.key)) tx.rgba.assign(4, 0.0f);
            out.push_back(std::move(tx));
            continue;
        }
        // a format the CPU can't read (an unused descriptor's, say) reads the same everywhere:
        // one texel of it does
        if (!layout(ti.dfmt).count) tx.width = tx.height = tx.layers = 1;
        if (u64(tx.width) * tx.height * tx.layers > (1u << 24)) {
            why = "textures of over 16 million texels";
            return false;
        }
        std::memcpy(tx.sampler, im.s, sizeof tx.sampler);
        bool micro = ti.tiling == 5 || ti.tiling == 9 || ti.tiling == 13 || ti.tiling == 19;
        u32 raw_format = ti.dfmt == 10 && (ti.nfmt == 0 || ti.nfmt == 9) ? (ti.nfmt == 9 ? 43 : 37)  // R8G8B8A8_UNORM, _SRGB
                         : vk_format(ti.dfmt, ti.nfmt, 0, tx.bpp);
        if (ti.dfmt == 10) tx.bpp = 4;
        if (ti.type == 9 && raw_format && !micro && ti.all_ok) {
            tx.format = raw_format;
            tx.pitch = ti.epitch;
            tx.raw = reinterpret_cast<const unsigned char*>(ti.base);
            for (int c = 0; c < 4; c++) tx.swizzle[c] = ti.sel[c];
            // kept on the GPU from draw to draw too, under the T# and a hash of its memory;
            // not memory kept there (a target's, read as a texture), which the GPU copies,
            // and which hashing it here would only bring back
            u64 bytes = u64(tx.pitch) * tx.height * tx.bpp;
            if (!vkdraw::version(ti.base) && !vkdraw::version(ti.base + bytes - 1)) {
                Timer hashing(g_texture_hash_ns);
                u64 before = g_hashed_bytes;
                u64 h = memory_stamp(ti.base, bytes);
                g_texture_hash_bytes += g_hashed_bytes - before;
                tx.key = std::format("raw:{:x}.{:x}.{:x}.{:x}.{:x}.{:x}.{:x}.{:x}:{:x}", im.t[0], im.t[1], im.t[2], im.t[3], im.t[4],
                                     im.t[5], im.t[6], im.t[7], h);
            }
            out.push_back(std::move(tx));
            continue;
        }
        // block-compressed, 2D (BC1-5, BC7, as the CPU decodes them): the blocks, their tiling
        // undone, in rows, for the GPU to decode; kept there like the rest
        u32 bc = !ti.block || ti.type != 9 || !ti.all_ok || !vkdraw::compressed() ? 0
                 : ti.nfmt == 0 ? (ti.dfmt == 35 ? 133 : ti.dfmt == 36 ? 135 : ti.dfmt == 37 ? 137 : ti.dfmt == 38 ? 139
                                   : ti.dfmt == 39 ? 141 : ti.dfmt == 41 ? 145 : 0)  // VK_FORMAT_BCn_..._UNORM_BLOCK
                 : ti.nfmt == 9 ? (ti.dfmt == 35 ? 134 : ti.dfmt == 36 ? 136 : ti.dfmt == 37 ? 138 : ti.dfmt == 41 ? 146 : 0)  // _SRGB_BLOCK
                                : 0;
        if (bc) {
            tx.format = bc;
            tx.block = ti.bytes;
            u32 across = (ti.width + 3) / 4, down = (ti.height + 3) / 4;
            tx.pitch = across * 4;
            for (int c = 0; c < 4; c++) tx.swizzle[c] = ti.sel[c];
            {
                Timer hashing(g_texture_hash_ns);
                u64 before = g_hashed_bytes;
                u64 h = memory_stamp(ti.base, ti.size);
                g_texture_hash_bytes += g_hashed_bytes - before;
                tx.key = std::format("bc:{:x}.{:x}.{:x}.{:x}.{:x}.{:x}.{:x}.{:x}:{:x}", im.t[0], im.t[1], im.t[2], im.t[3], im.t[4],
                                     im.t[5], im.t[6], im.t[7], h);
            }
            if (!vkdraw::has_texture(tx.key)) {
                Timer sending(g_texture_blocks_ns);
                g_texture_blocks++;
                tx.blocks.resize(size_t(across) * down * ti.bytes);
                unsigned char* o = tx.blocks.data();
                for (u32 y = 0; y < down; y++)
                    for (u32 x = 0; x < across; x++, o += ti.bytes)
                        std::memcpy(o, reinterpret_cast<const void*>(element_address(ti.base, ti.tiling, ti.bytes, ti.epitch, ti.eheight, x, y, 0)),
                                    ti.bytes);
            }
            out.push_back(std::move(tx));
            continue;
        }
        // in rows, some of its layers in memory kept on the GPU (a cube's faces drawn there,
        // say): those layers copied from there, by their version; the rest hashed and copied
        // from memory, as below. (So the memory kept isn't read back, to be hashed or copied.)
        if (raw_format && ti.all_ok && !ti.block && tx.bpp == ti.bytes && tx.bpp % 4 == 0 && !micro && ti.type != 10 &&
            layout(ti.dfmt).count) {
            u64 end = ti.base + ti.size;
            std::vector<u64> from(tx.layers), stamp(tx.layers);
            bool any = false;
            {
                Timer hashing(g_texture_hash_ns);
                for (u32 l = 0; l < tx.layers; l++) {
                    u32 slice = ti.array() || cube ? ti.base_array + l : 0;
                    u64 a0 = element_address(ti.base, ti.tiling, ti.bytes, ti.epitch, ti.eheight, 0, 0, slice);
                    u64 bytes = (u64(tx.height - 1) * ti.epitch + tx.width) * tx.bpp;
                    from[l] = a0;
                    if (u64 v = a0 ? vkdraw::steady_version(a0, bytes, tx.bpp) : 0) {
                        stamp[l] = v << 1 | 1, any = true;
                    } else if (a0) {
                        u64 before = g_hashed_bytes;
                        u64 a1 = std::min(end, element_address(ti.base, ti.tiling, ti.bytes, ti.epitch, ti.eheight, 0, 0, slice + 1));
                        stamp[l] = memory_stamp(a0, std::max(a1, a0 + bytes) - a0) << 1;
                        g_texture_hash_bytes += g_hashed_bytes - before;
                    }
                }
            }
            if (any) {
                u64 h = 0x9e3779b97f4a7c15ull;
                for (u64 x : stamp) h = (h ^ x) * 0x100000001b3ull;
                tx.key = std::format("layers:{:x}.{:x}.{:x}.{:x}.{:x}.{:x}.{:x}.{:x}:{:x}", im.t[0], im.t[1], im.t[2], im.t[3], im.t[4],
                                     im.t[5], im.t[6], im.t[7], h);
                if (!vkdraw::has_texture(tx.key)) {
                    Timer copying(g_texture_copy_ns);
                    g_texture_copied++;
                    tx.format = raw_format;
                    tx.pitch = tx.width;
                    tx.kept_pitch = ti.epitch;
                    for (int c = 0; c < 4; c++) tx.swizzle[c] = ti.sel[c];
                    size_t row = size_t(tx.width) * tx.bpp;
                    tx.blocks.resize(row * tx.height * tx.layers);
                    for (u32 l = 0; l < tx.layers; l++) {
                        if (stamp[l] & 1) {
                            tx.kept_layers.push_back({l, from[l]});
                            continue;
                        }
                        unsigned char* o = tx.blocks.data() + row * tx.height * l;
                        u32 slice = ti.array() || cube ? ti.base_array + l : 0;
                        for (u32 y = 0; y < tx.height; y++, o += row)
                            std::memcpy(o, reinterpret_cast<const void*>(element_address(ti.base, ti.tiling, ti.bytes, ti.epitch, ti.eheight, 0, y, slice)),
                                        row);
                    }
                }
                out.push_back(std::move(tx));
                continue;
            }
        }
        if (ti.all_ok) {
            u64 h = 0;
            if (layout(ti.dfmt).count) {
                Timer hashing(g_texture_hash_ns);
                u64 before = g_hashed_bytes;
                h = memory_stamp(ti.base, ti.size);
                g_texture_hash_bytes += g_hashed_bytes - before;
            }
            tx.key = std::format("{:x}.{:x}.{:x}.{:x}.{:x}.{:x}.{:x}.{:x}:{:x}", im.t[0], im.t[1], im.t[2], im.t[3], im.t[4], im.t[5],
                                 im.t[6], im.t[7], h);
            if (vkdraw::has_texture(tx.key)) {
                out.push_back(std::move(tx));
                continue;
            }
        }
        // in a format the GPU reads as it is (any type: 1D, a cube, an array, 3D): the texels
        // copied out as they are, their tiling undone, slice by slice, row by row, for the GPU
        // to read (the T#'s dst_sel as swizzle), and kept there like the rest; else decoded here
        if (raw_format && ti.all_ok && !ti.block && tx.bpp == ti.bytes && layout(ti.dfmt).count) {
            Timer copying(g_texture_copy_ns);
            g_texture_copied++;
            tx.format = raw_format;
            tx.pitch = tx.width;
            for (int c = 0; c < 4; c++) tx.swizzle[c] = ti.sel[c];
            size_t row = size_t(tx.width) * tx.bpp;
            tx.blocks.resize(row * tx.height * tx.layers);
            unsigned char* o = tx.blocks.data();
            for (u32 l = 0; l < tx.layers; l++) {
                u32 slice = ti.array() || cube ? ti.base_array + l : ti.type == 10 ? l : 0;
                for (u32 y = 0; y < tx.height; y++, o += row) {
                    if (!micro) {  // a row's texels, one after another
                        std::memcpy(o, reinterpret_cast<const void*>(element_address(ti.base, ti.tiling, ti.bytes, ti.epitch, ti.eheight, 0, y, slice)),
                                    row);
                        continue;
                    }
                    for (u32 x = 0; x < tx.width; x++)
                        std::memcpy(o + size_t(x) * tx.bpp,
                                    reinterpret_cast<const void*>(element_address(ti.base, ti.tiling, ti.bytes, ti.epitch, ti.eheight, x, y, slice)),
                                    tx.bpp);
                }
            }
            out.push_back(std::move(tx));
            continue;
        }
        auto decode0 = std::chrono::steady_clock::now();
        struct Tally {
            std::pair<u64, u64>& to;
            std::chrono::steady_clock::time_point t0;
            ~Tally() {
                u64 ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
                to.first++, to.second += ns;
                g_texture_decode_ns += ns;
            }
        } tally{g_decoded_by[std::format("format {}, number type {}, tile mode {}, type {}{}", ti.dfmt, ti.nfmt, ti.tiling, ti.type,
                                         ti.all_ok ? "" : " (not all in memory)")],
                decode0};
        g_texture_decoded++;
        tx.rgba.resize(size_t(tx.width) * tx.height * tx.layers * 4);
        float* o = tx.rgba.data();
        for (u32 l = 0; l < tx.layers; l++) {
            u32 slice = ti.array() || cube ? ti.base_array + l : ti.type == 10 ? l : 0;
            for (u32 y = 0; y < tx.height; y++)
                for (u32 x = 0; x < tx.width; x++, o += 4) {
                    u32 v[4];
                    texel_rgba(ti, mem, int(x), int(y), slice, v);
                    std::memcpy(o, v, 16);
                }
        }
        out.push_back(std::move(tx));
    }
    return true;
}

bool prepare_gpu(vkdraw::Draw& d, std::string& why, std::string& vertex_why, const GpuRegs& r, u64 vs, u64 ps,
               const Vertices& out, const std::vector<Tri>& tris, const std::vector<u32>& index,
               const std::vector<std::array<size_t, 3>>& tris_in, bool rects, const Target* targets, const int clip[4],
               u32 ps_user, u32 ena, u32 addr, const Memory& mem, bool gpu_vertices_only = false) {
    PrepareParts part("the targets and depth buffer");
    // the targets written (a channel of them, at least), all the same size
    const Target* first = nullptr;
    for (u32 k = 0; k < 8; k++) {
        const Target& t = targets[k];
        if (!t.count || !t.mask) continue;
        u32 bpp;
        if (!vk_format(t.format, t.nfmt, t.swap, bpp)) {
            why = std::format("render targets of format {}, number type {}, swap {}", t.format, t.nfmt, t.swap);
            return false;
        }
        if (!t.rows) {
            why = "micro-tiled render targets";
            return false;
        }
        if (!mem.ok(t.base, u64(t.pitch) * t.height * bpp)) {
            why = "a render target outside the GPU's memory";
            return false;
        }
        if (first && (t.pitch != first->pitch || t.height != first->height)) {
            why = "render targets of different sizes";
            return false;
        }
        if (!first) first = &t;
    }
    DepthBuffer db = depth_buffer(r, mem);
    Target depth_only;  // a draw of depth alone: its size is the depth buffer's
    if (!first && (db.write || db.stencil_writes())) {
        depth_only.pitch = db.pitch;
        depth_only.height = db.height;
        first = &depth_only;
    }
    if (!first) {
        why = "no render target written";
        return false;
    }
    const Target& t = *first;
    part.to("the pixel shader: found decoded, its scalars worked out");
    // (no pixel shader: depth and stencil alone, every pixel covered passing on, as if exported)
    static const translate::Program no_program;
    auto it = ps ? g_programs.find(ps) : g_programs.end();
    if (ps && it == g_programs.end()) it = g_programs.emplace(ps, translate::decode(ps, mem)).first;
    const translate::Program& prog = ps ? it->second : no_program;
    if (!prog.why.empty()) {
        why = "the shader has " + prog.why;
        return false;
    }
    u32 user[16];
    for (u32 k = 0; k < 16; k++) user[k] = r.sh[0x0c + k];
    u32 users = std::min(ps_user, 16u);
    translate::Values vals;
    if (ps) {
        Timer timer(g_translate_ns);
        vals = translate::evaluate(prog, user, users, mem);
    }
    if (!vals.why.empty()) {
        why = "the shader has " + vals.why;
        return false;
    }
    translate::PsState st;
    st.user_count = users;
    st.ena = ena;
    st.addr = addr;
    part.to("the pixel shader: its state, buffers and images sorted out");
    for (u32 k = 0; k < 32; k++) st.input_cntl[k] = reg(r, 0x191 + k, k);
    st.col_format = reg(r, 0x1c5, 0);
    st.db_shader = reg(r, 0x203, 0);  // DB_SHADER_CONTROL
    st.barycentric = vkdraw::barycentric();
    st.targets = 0;
    for (u32 k = 0; k < 8; k++) {
        u32 bpp;
        if (targets[k].count && targets[k].mask && vk_format(targets[k].format, targets[k].nfmt, targets[k].swap, bpp)) st.targets |= 1u << k;
    }
    // the buffer loads': those of the same descriptor, offset and format share a buffer
    std::vector<translate::Values::Buffer> ps_buffers;
    for (const auto& b : vals.buffers) {
        u32 slot = 0;
        while (slot < ps_buffers.size() && (std::memcmp(ps_buffers[slot].v, b.v, sizeof b.v) || ps_buffers[slot].offset != b.offset ||
                                            ps_buffers[slot].dfmt != b.dfmt || ps_buffers[slot].nfmt != b.nfmt))
            slot++;
        if (slot == ps_buffers.size()) ps_buffers.push_back(b);
        st.buffer_slot.push_back(slot);
    }
    for (size_t k = 0; k < vals.buffers.size() && k < 64; k++)
        if ((vals.buffers[k].v[3] >> 23) & 1) st.add_tid |= 1ull << k;
    std::vector<const translate::Values::Image*> unique = sort_images(vals.images, st);
    part.to("the pixel shader: its GLSL found (or translated), copied");
    static translate::Glsl no_shader;
    if (no_shader.text.empty()) no_shader.text = "#version 450\nvoid main() {}\n", no_shader.id = std::hash<std::string>{}(no_shader.text) | 1;
    std::string gkey = std::format("ps:{:x}:{}", ps, st.key());
    auto git = ps ? g_glsl.find(gkey) : g_glsl.end();
    {
        Timer timer(g_translate_ns);
        if (ps && git == g_glsl.end()) git = g_glsl.emplace(gkey, translate::pixel_shader(prog, st)).first;
    }
    if (ps && !git->second.id) git->second.id = std::hash<std::string>{}(git->second.text) | 1;
    const translate::Glsl& gl = ps ? git->second : no_shader;
    if (ps && gl.why.empty()) save_glsl("ps", ps, gl.text);
    if (!gl.why.empty()) {
        why = "the shader has " + gl.why;
        return false;
    }
    d.ps_kept = &gl.text;
    d.ps_id = gl.id;
    d.attributes = gl.attributes;
    part.to("the pixel shader: its buffers unpacked");
    if (!unpack_buffers(ps_buffers, mem, d.ps_buffers, why)) return false;
    part.to("the targets' and depth's details");
    // each target the shader exports to (one it doesn't is left as it is, as on the CPU)
    d.targets.clear();
    for (u32 k = 0; k < 8; k++) {
        const Target& tk = targets[k];
        vkdraw::Draw::Target dt;
        if (tk.count && tk.mask && ((gl.outputs >> k) & 1)) {
            dt.format = vk_format(tk.format, tk.nfmt, tk.swap, dt.bpp);
            dt.blend = tk.blend;
            dt.mask = tk.mask;
        }
        d.targets.push_back(dt);
    }
    // (a place for every output the shader writes, empty where nothing is written there)
    while (!d.targets.empty() && !d.targets.back().format && !((gl.outputs >> (d.targets.size() - 1)) & 1)) d.targets.pop_back();
    bool none = true;
    for (auto& dt : d.targets) none = none && !dt.format;
    if (none && !db.write && !db.stencil_writes()) {
        why = "no render target exported to";
        return false;
    }
    if (db.any()) {
        d.depth.format = db.format == 3 ? 126 : 124;  // D32_SFLOAT, D16_UNORM
        d.depth.test = db.test;
        if (db.has_stencil) {  // (the stencil buffer is kept with the depth buffer, tested or not)
            if (db.format != 3) {
                why = "stencil with 16-bit depth";
                return false;
            }
            d.depth.format = 130;  // D32_SFLOAT_S8_UINT
            d.depth.stencil = true;
            d.depth.stencil_test = db.stencil;
            d.depth.saddress = db.sbase;
        }
        if (db.stencil) {
            auto face = [&](const DepthBuffer::Face& f, vkdraw::Draw::Depth::Face& v) {
                // the console's operations as Vulkan's: those Vulkan has (with the one value it
                // has for the test and the operations, the reference); else not on the GPU
                auto op = [&](u32 o, u32& out) {
                    u32 w = f.wmask;
                    switch (o) {
                        case 0: out = 0; return true;  // KEEP
                        case 1: out = 1; return true;  // ZERO
                        case 3: out = 2; return true;  // REPLACE
                        case 7: out = 5; return true;  // INVERT
                        case 2: if (((f.ref ^ 255) & w) == 0) return out = 2, true; break;
                        case 4:
                            if ((f.opval & w) == 0) return out = 1, true;
                            if (((f.opval ^ f.ref) & w) == 0) return out = 2, true;
                            break;
                        case 5: if (f.opval == 1) return out = 3, true; break;  // INCREMENT_AND_CLAMP
                        case 6: if (f.opval == 1) return out = 4, true; break;  // DECREMENT_AND_CLAMP
                        case 8: if (f.opval == 1) return out = 6, true; break;  // INCREMENT_AND_WRAP
                        case 9: if (f.opval == 1) return out = 7, true; break;  // DECREMENT_AND_WRAP
                        default: break;
                    }
                    why = std::format("stencil operation {} (value {:#x})", o, f.opval);
                    return false;
                };
                v.func = f.func, v.cmask = f.mask, v.wmask = f.wmask, v.ref = f.ref;
                return op(f.fail, v.fail) && op(f.zpass, v.pass) && op(f.zfail, v.zfail);
            };
            if (!face(db.front, d.depth.front) || !face(db.back, d.depth.back)) return false;
        }
        d.depth.bpp = db.bpp;
        d.depth.width = db.pitch;
        d.depth.height = db.height;
        d.depth.address = db.base;
        d.depth.write = db.write;
        d.depth.func = db.func;
        if (db.zmin != 0 || db.zmax != 1) {
            why = "a viewport's depth range other than 0 to 1";
            return false;
        }
    }
    d.values = vals.u;
    part.to(nullptr);  // (the vertex shader's own parts)
    bool on_gpu = gpu_vertices(d, vertex_why, r, vs, gl.attributes, t, index, tris_in, rects, mem);
    part.to("the CPU's vertices, if the vertex shader was the CPU's");
    if (!on_gpu && gpu_vertices_only) return false;  // why empty: only the vertex shader stopped it
    if (!on_gpu) {
        d.indices.clear();
        d.vs_buffers.clear();
        d.gs.clear();
        d.vs = translate::pass_vertex_shader(gl.attributes);
        d.vs_kept = nullptr;
        d.vs_id = 0;
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
    part.to("the textures");
    if (!textures_for(unique, mem, d.textures, why)) return false;
    part.to("the rest");
    d.width = t.pitch;
    d.height = t.height;
    u32 rows = (t.height + 7) / 8 * 8;  // as the console lays a target out, which a compute shader may write
    d.rows = rows;
    for (u32 k = 0; k < d.targets.size(); k++)
        if (d.targets[k].format && !mem.ok(targets[k].base, u64(t.pitch) * rows * d.targets[k].bpp)) d.rows = t.height;
    for (int c = 0; c < 4; c++) d.blend_color[c] = regf(r, 0x105 + u32(c), 0);
    for (int k = 0; k < 4; k++) d.scissor[k] = clip[k];
    return true;
}

// A pixel's channels (4 at most) as whole numbers in their own encoding, ordered as the values
// are, so that two values a step apart in it differ by 1: 8-bit RGBA, 16-bit floats (two or
// four; their sign bit folded in), 11- and 10-bit floats
int channels(u32 format, const unsigned char* p, int out[4]) {
    if (format == 100) {  // a 32-bit float: steps of it (a whole number's range, as far as it goes)
        std::int32_t v;
        std::memcpy(&v, p, 4);
        out[0] = v < 0 ? -(v & 0x7fffffff) : v;
        return 1;
    }
    if (format == 83 || format == 97) {
        int n = format == 83 ? 2 : 4;
        for (int c = 0; c < n; c++) {
            int h = p[2 * c] | (p[2 * c + 1] << 8);
            out[c] = h & 0x8000 ? -(h & 0x7fff) : h;
        }
        return n;
    }
    if (format == 122) {
        u32 v;
        std::memcpy(&v, p, 4);
        out[0] = int(v & 0x7ff), out[1] = int((v >> 11) & 0x7ff), out[2] = int(v >> 22);
        return 3;
    }
    for (int c = 0; c < 4; c++) out[c] = p[c];
    return 4;
}

// The GPU's pictures against the CPU's, now in the targets: how many pixels differ by more
// than a step (1 in 255 for 8-bit RGBA) in any channel of any target, and by how many steps
// at most, and how many the draw changed (on the GPU), so that "the same" isn't only "left as
// they were". Saves the GPU's first 8-bit target as NNN-gpu.bmp.
std::string compare(u64 number, const vkdraw::Draw& d, const std::vector<std::vector<unsigned char>>& before,
                    const std::vector<std::vector<unsigned char>>& result, const Target* targets, const int clip[4]) {
    u64 n = 0, differ = 0, changed = 0;
    int most = 0, used = 0;
    bool saved = false;
    for (size_t k = 0; k < d.targets.size(); k++) {
        const vkdraw::Draw::Target& dt = d.targets[k];
        if (!dt.format) continue;
        used++;
        u32 bpp = dt.bpp;
        for (int y = clip[1]; y < clip[3] && u32(y) < d.height; y++)
            for (int x = clip[0]; x < clip[2] && u32(x) < d.width; x++) {
                size_t at = (size_t(y) * d.width + size_t(x)) * bpp;
                int a[4], g[4];
                int nc = channels(dt.format, reinterpret_cast<const unsigned char*>(targets[k].base) + at, a);
                channels(dt.format, &result[k][at], g);
                int m = 0;
                for (int c = 0; c < nc; c++) m = std::max(m, std::abs(a[c] - g[c]));
                n++;
                differ += m > 1;
                changed += std::memcmp(&before[k][at], &result[k][at], bpp) != 0;
                most = std::max(most, m);
            }
        if (saved || bpp != 4 || dt.format == 83 || dt.format == 122 || dt.format == 100) continue;
        saved = true;
        u32 w = std::min({d.width, u32(clip[2]), 4096u}), h = std::min({d.height, u32(clip[3]), 4096u});
        std::vector<unsigned char> px(size_t(w) * h * 4);
        bool rgba = dt.format == 37 || dt.format == 43;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) {
                const unsigned char* p = &result[k][(size_t(y) * d.width + x) * 4];
                unsigned char* q = &px[(size_t(y) * w + x) * 4];
                q[0] = rgba ? p[2] : p[0];
                q[1] = p[1];
                q[2] = rgba ? p[0] : p[2];
                q[3] = 0;
            }
        write_bmp(std::filesystem::path(M.out_dir) / "draws" / std::format("{:03}-gpu.bmp", number), w, h, px);
    }
    g_checked++;
    std::string targets_text = used > 1 ? std::format(" of its {} targets", used) : "";
    if (!differ) {
        g_check_same++;
        return std::format("on the GPU: the same in all {} pixels{} ({} of them changed by the draw)", n, targets_text, changed);
    }
    return std::format("on the GPU: {} of {} pixels{} differ by more than a step (by {} at most; {} changed by the draw)", differ, n,
                       targets_text, most, changed);
}

// --gpu-check's depth: the GPU's against the CPU's (in memory), each pixel's depth within a
// 16-bit step (1 in 65535)
std::string compare_depth(const vkdraw::Draw& d, u64 cpu, const std::vector<unsigned char>& before, const std::vector<unsigned char>& gpu,
                          const int clip[4]) {
    u64 n = 0, differ = 0, changed = 0;
    double most = 0;
    u32 bpp = d.depth.bpp;
    for (int y = clip[1]; y < clip[3] && u32(y) < d.depth.height; y++)
        for (int x = clip[0]; x < clip[2] && u32(x) < d.depth.width; x++) {
            size_t at = (size_t(y) * d.depth.width + size_t(x)) * bpp;
            const auto* c = reinterpret_cast<const unsigned char*>(cpu) + at;
            double a, b;
            if (bpp == 2) {
                std::uint16_t ca, gb;
                std::memcpy(&ca, c, 2);
                std::memcpy(&gb, &gpu[at], 2);
                a = ca / 65535.0, b = gb / 65535.0;
            } else {
                float ca, gb;
                std::memcpy(&ca, c, 4);
                std::memcpy(&gb, &gpu[at], 4);
                a = ca, b = gb;
            }
            double m = std::abs(a - b);
            n++;
            differ += m > 1.0 / 65535;
            changed += std::memcmp(&before[at], &gpu[at], bpp) != 0;
            most = std::max(most, m);
        }
    if (!differ) return std::format("; its depth the same in all {} pixels ({} of them changed by the draw)", n, changed);
    g_check_depth_differ++;
    return std::format("; its depth: {} of {} pixels differ by more than 1 in 65535 (by {:.6f} at most; {} changed by the draw)", differ, n,
                       most, changed);
}

// The triangles a primitive type makes of n vertices
std::vector<std::array<size_t, 3>> assemble(u32 type, size_t n, bool& rects, bool& known) {
    std::vector<std::array<size_t, 3>> t;
    t.reserve(type == 5 || type == 6 ? n : type == 0x13 ? n / 2 : n / 3);
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
    Lap lap(ST_SETUP);
    g_draws++;
    g_page_epoch++;
    PrepareParts part("the shaders found, the stages", g_setup_ns);
    u64 vs = program(r, 0x48), ps = program(r, 0x08);
    if (!vs) return "(no vertex shader)";
    // tessellation (a hull shader) or a geometry shader on: the program in the vertex shader's
    // place is then the domain shader, or the geometry shader's copy shader. Not done yet.
    u32 stages = reg(r, 0x2d5, 0);  // VGT_SHADER_STAGES_EN
    if (stages & 0x24) {
        const char* what = stages & 4 ? (stages & 0x20 ? "tessellation and a geometry shader" : "tessellation") : "a geometry shader";
        note(std::format("draws with {}", what));
        return std::format("({}: not drawn)", what);
    }
    part.to("the memory's snapshot");
    Memory mem;
    mem.snapshot();
    part.to("a clear by a draw, seen to");
    if (std::string c = clear_by_draw(r, mem); !c.empty()) return c;
    part.to("the indices read");
    u32 type = r.uc[0x242];
    bool rects, known;
    // the vertices: through the index buffer, or numbered
    std::vector<u32> index(count);
    u64 from = indices + u64(first) * r.index_size;
    if (!indices) {
        for (u32 i = 0; i < count; i++) index[i] = i;
    } else if ((r.index_size == 2 || r.index_size == 4) && mem.ok(from, u64(count) * r.index_size)) {  // all there: read at once
        if (r.index_size == 4) {
            std::memcpy(index.data(), reinterpret_cast<const void*>(from), size_t(count) * 4);
        } else {
            const auto* p = reinterpret_cast<const std::uint16_t*>(from);
            for (u32 i = 0; i < count; i++) index[i] = p[i];
        }
    } else {
        for (u32 i = 0; i < count; i++) {
            u64 at = indices + u64(first + i) * r.index_size;
            u32 v = 0;
            if (mem.ok(at, r.index_size)) std::memcpy(&v, reinterpret_cast<const void*>(at), r.index_size);
            index[i] = v;
        }
    }
    part.to("the triangles assembled");
    auto tris_in = assemble(type, count, rects, known);
    if (!known) {
        note(std::format("primitive type {:#x}", type));
        return std::format("(primitive type {:#x}: not drawn)", type);
    }
    // (no pixel shader: no colour, but depth and stencil as for any other draw)
    // the targets, and the viewport and scissor
    part.to("the targets");
    Target targets[8];
    bool any = false;
    for (u32 k = 0; k < 8; k++) {
        targets[k] = target(r, k);
        targets[k].prepare(mem);
        any |= targets[k].count != 0;
    }
    u32 mode = (reg(r, 0x202, 0xcc0010) >> 4) & 7;
    bool masked = true;  // every channel of every target masked off (a pass for depth alone)
    for (auto& t : targets) masked = masked && (!t.count || !t.mask);
    part.to("the depth buffer");
    DepthBuffer db = depth_buffer(r, mem);
    if ((!any || mode == 0 || masked) && !db.write && !db.stencil_writes())
        return std::format("{} vertices (no colour, depth or stencil written)", count);
    part.to("the depth buffer's fast clear (HTILE)");
    expand_htile(r, mem, db);
    part.to("the rest (scissors, what is saved)");
    if (mode == 0)  // colour off: depth alone
        for (auto& t : targets) t.count = 0;
    if (mode != 1 && mode != 0) {
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
        if (t.count && t.mask && mode == 1) x1 = std::min(x1, int(t.pitch)), y1 = std::min(y1, int(t.height));
    if (db.any()) x1 = std::min(x1, int(db.pitch)), y1 = std::min(y1, int(db.height));
    u32 ps_user = (r.sh[0x0b] >> 1) & 0x1f;
    u32 ena = reg(r, 0x1b3, 0), addr = reg(r, 0x1b4, 0);
    if (!addr) addr = ena;
    bool save = g_saved < SAVED && g_saved_by_ps[ps] < (M.save_ps.empty() ? 2 : 8) && video_flips() >= M.save_from &&
                (M.save_ps.empty() || std::find(M.save_ps.begin(), M.save_ps.end(), ps) != M.save_ps.end());
    int area[4] = {x0, y0, x1, y1};
    vkdraw::Draw check;
    std::string check_why, vertex_why;
    std::vector<std::vector<unsigned char>> before, result;  // each target's
    std::vector<unsigned char> zbefore, sbefore, zresult;  // the depth buffer's, the stencil buffer's; the GPU's depth
    bool checked = false;
    // the draws saved to draws/: their number and a line in the index, once drawn
    auto saved = [&](std::string what, const std::string& extra) {
        if (!save) return what;
        lap.to(ST_SAVE);
        g_saved_by_ps[ps]++;
        u64 number = g_saved++;
        auto dir = std::filesystem::path(M.out_dir) / "draws";
        std::filesystem::create_directories(dir);
        if (number == 0)  // an earlier run's pictures go
            for (const auto& e : std::filesystem::directory_iterator(dir))
                if (e.path().extension() == ".bmp") std::filesystem::remove(e.path());
        if (checked) {
            what += "; " + compare(number, check, before, result, targets, area);
            if (!zresult.empty()) what += compare_depth(check, db.base, zbefore, zresult, area);
            what += vertex_why.empty() ? ", vertices too" : "; vertices from the CPU: " + vertex_why;
        } else if (M.gpu_check) {
            what += "; not on the GPU: " + check_why;
        }
        what += extra;
        for (auto& t : targets)
            if (t.count) {
                save_target(number, t, u32(x1), u32(y1), mem, std::format("vs {:#x}, ps {:#x}: {}", vs, ps, what));
                break;
            }
        return what;
    };
    // Drawn on the GPU, vertices and all, if it can be (and it isn't a draw --gpu-check
    // compares): then the CPU's vertex shader isn't needed. If only the vertex shader can't
    // be, the CPU runs it, and the GPU is given its vertices (below).
    part.to(nullptr);  // (set up)
    bool gpu_failed = false;  // for another reason: not tried again
    std::string vertices_why = !M.gpu ? "" : save && M.gpu_check ? "--gpu-check's draws" : "";
    if (M.gpu && !(save && M.gpu_check) && x0 < x1 && y0 < y1) {
        vkdraw::Draw gd;
        std::string why, vwhy;
        lap.to(ST_PREPARE);
        if (prepare_gpu(gd, why, vwhy, r, vs, ps, Vertices{}, {}, index, tris_in, rects, targets, area, ps_user, ena, addr, mem, true)) {
            for (size_t k = 0; k < gd.targets.size(); k++) gd.targets[k].address = targets[k].base;
            lap.to(ST_GPU);
            if (vkdraw::run(gd, why)) {
                g_gpu_draws++;
                g_gpu_vertices++;
                return saved(std::format("on the GPU: {} triangles{}", gd.indices.size() / 3, rects ? " (rectangles)" : ""), "");
            }
            gpu_failed = true;
        } else if (why.size()) {
            gpu_failed = true;
        } else {
            vertices_why = vwhy.empty() ? "(no reason given)" : vwhy;
        }
        if (gpu_failed) g_gpu_skipped[why]++;
    }
    // the vertex shader, 64 vertices a wave; a rectangle's fourth corner is added after
    lap.to(ST_CPU_VERTICES);
    auto vs0 = std::chrono::steady_clock::now();
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
    if (!vertices_why.empty()) {
        auto& v = g_vertex_skipped[vertices_why];
        v[0]++, v[1] += count;
        v[2] += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - vs0).count());
    }
    lap.to(ST_CPU_TRIANGLES);
    u32 cull = reg(r, 0x205, 0);
    std::vector<Tri> tris;
    tris.reserve(tris_in.size() * (rects ? 2 : 1));
    u64 behind = 0, clipped = 0;
    bool divide = !((vte >> 8) & 1);
    // the guard band: how far past the viewport (in its own widths, as clip space has it) the
    // hardware lets a triangle reach before it clips it
    float gx = std::max(1.0f, regf(r, 0x2fc, 1)), gy = std::max(1.0f, regf(r, 0x2fa, 1));
    auto triangle = [&](size_t a, size_t b, size_t c) {
        Tri t;
        size_t v[3] = {a, b, c};
        for (int k = 0; k < 3; k++) {
            const float* p = &out.pos[v[k] * 4];
            float w = p[3];
            if (!(w > 0)) {
                behind++;
                return;
            }
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
    // A triangle reaching past the guard band (or behind the eye, which the band's planes
    // imply) is clipped in clip space, as the hardware does: to the planes -g w <= x <= g w and
    // the same for y (and w > 0). Depth isn't clipped (the GPU clamps it, as here). The new
    // corners are new vertices, their parameters in proportion; the polygon left is drawn as
    // a fan.
    constexpr float EPS = 1e-6f;
    auto plane = [&](const float* p, int i) {
        switch (i) {
            case 0: return gx * p[3] + p[0];
            case 1: return gx * p[3] - p[0];
            case 2: return gy * p[3] + p[1];
            case 3: return gy * p[3] - p[1];
            default: return p[3] - EPS;
        }
    };
    auto add = [&](size_t a, size_t b, size_t c) {
        size_t v[3] = {a, b, c};
        bool inside = true;
        for (int k = 0; k < 3 && inside; k++)
            for (int i = 0; i < 5; i++)
                if (!(plane(&out.pos[v[k] * 4], i) >= 0)) {
                    inside = false;
                    break;
                }
        if (inside || !divide) return triangle(a, b, c);
        std::vector<size_t> poly{a, b, c}, next;
        for (int i = 0; i < 5 && poly.size() >= 3; i++) {
            next.clear();
            for (size_t k = 0; k < poly.size(); k++) {
                size_t p = poly[k], q = poly[(k + 1) % poly.size()];
                float dp = plane(&out.pos[p * 4], i), dq = plane(&out.pos[q * 4], i);
                if (dp >= 0) next.push_back(p);
                if ((dp >= 0) != (dq >= 0)) {  // the edge crosses the plane: a corner there
                    float s = dp / (dp - dq);
                    size_t n = out.pos.size() / 4, P = out.params * 4;
                    for (int c = 0; c < 4; c++) out.pos.push_back(out.pos[p * 4 + c] + s * (out.pos[q * 4 + c] - out.pos[p * 4 + c]));
                    for (size_t j = 0; j < P; j++) out.param.push_back(out.param[p * P + j] + s * (out.param[q * P + j] - out.param[p * P + j]));
                    next.push_back(n);
                }
            }
            poly.swap(next);
        }
        if (poly.size() < 3) return;  // all of it past the band, or behind the eye: nothing to see
        clipped++;
        for (size_t k = 1; k + 1 < poly.size(); k++) triangle(poly[0], poly[k], poly[k + 1]);
    };
    for (size_t k = 0; k < tris_in.size(); k++) {
        auto [a, b, c] = tris_in[k];
        add(a, b, c);
        if (rects) add(b, count + k, c);
    }
    if (behind) note("triangles with a vertex behind the eye, positions not divided by w (skipped)", behind);
    g_clipped += clipped;
    g_triangles += tris.size();
    if (tris.empty() || x0 >= x1 || y0 >= y1) return std::format("{} vertices, nothing on screen", count);
    if (save && M.gpu_check) {
        lap.to(ST_CHECK);
        checked = prepare_gpu(check, check_why, vertex_why, r, vs, ps, out, tris, index, tris_in, rects, targets, area, ps_user,
                              ena, addr, mem);
        if (checked) {
            before.resize(check.targets.size());
            result.resize(check.targets.size());
            for (size_t k = 0; k < check.targets.size(); k++) {
                vkdraw::Draw::Target& ct = check.targets[k];
                if (!ct.format) continue;
                const auto* p = reinterpret_cast<const unsigned char*>(targets[k].base);
                before[k].assign(p, p + size_t(check.width) * check.height * ct.bpp);
                result[k].resize(before[k].size());
                ct.target = before[k].data();
                ct.result = result[k].data();
            }
            if (check.depth.format) {
                const auto* z = reinterpret_cast<const unsigned char*>(check.depth.address);
                zbefore.assign(z, z + size_t(check.depth.width) * check.depth.height * check.depth.bpp);
                check.depth.target = zbefore.data();
                if (check.depth.write) {
                    zresult.resize(zbefore.size());
                    check.depth.result = zresult.data();
                }
                check.depth.address = 0;
                if (check.depth.stencil) {
                    const auto* s = reinterpret_cast<const unsigned char*>(check.depth.saddress);
                    sbefore.assign(s, s + size_t(check.depth.width) * check.depth.height);
                    check.depth.starget = sbefore.data();
                    check.depth.saddress = 0;
                }
            }
            checked = vkdraw::run(check, check_why);
        }
        if (!checked) g_check_skipped[check_why]++;
        else if (vertex_why.empty()) g_check_gpu_vertices++;
    } else if (M.gpu && !gpu_failed) {
        // drawn on the GPU, into the game's memory, with the CPU's vertices
        vkdraw::Draw gd;
        std::string why, vwhy;
        lap.to(ST_PREPARE);
        if (prepare_gpu(gd, why, vwhy, r, vs, ps, out, tris, index, tris_in, rects, targets, area, ps_user, ena, addr, mem)) {
            for (size_t k = 0; k < gd.targets.size(); k++) gd.targets[k].address = targets[k].base;
            lap.to(ST_GPU);
            if (vkdraw::run(gd, why)) {
                g_gpu_draws++;
                g_gpu_vertices += vwhy.empty();
                size_t n = gd.indices.empty() ? gd.vertices.size() / ((1 + gd.attributes) * 4) : gd.indices.size();
                return saved(std::format("on the GPU: {} triangles{}", n / 3, rects ? " (rectangles)" : ""),
                             vwhy.empty() ? "" : "; vertices from the CPU: " + vwhy);
            }
        }
        g_gpu_skipped[why]++;
    }
    // the pixels, in bands of rows, one band to a thread; within a band, triangle by triangle
    lap.to(ST_CPU_PIXELS);
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
        if (io.z_export && db.test && !masked && mode == 1)
            note("depth exported by a pixel shader that writes colour too (the colour not held back by the depth test, on the CPU)");
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
            // the depth and stencil tests, worked out first (each pixel's depth kept); colour only
            // for those that pass both
            u32 zbits[LANES];
            int outcome[LANES] = {};  // 0 passed, 1 failed the depth test, 2 the stencil test
            u64 covered = io.cover;
            auto z_at = [&](int l) {  // the triangle's depth at the pixel
                float cx = float(io.px[l]) + 0.5f, cy = float(io.py[l]) + 0.5f;
                float a = ((t.x[2] - t.x[1]) * (cy - t.y[1]) - (t.y[2] - t.y[1]) * (cx - t.x[1])) / t.area;
                float b = ((t.x[0] - t.x[2]) * (cy - t.y[2]) - (t.y[0] - t.y[2]) * (cx - t.x[2])) / t.area;
                return a * t.z[0] + b * t.z[1] + (1 - a - b) * t.z[2];
            };
            // the shader's own depth: tested once it has run (colour isn't held back for it)
            bool late = io.z_export && db.test;
            if (db.any()) {
                for (int l = 0; l < LANES; l++) {
                    if (!((covered >> l) & 1)) continue;
                    bool zpass = true;
                    if (db.test && !late) zpass = db.pass(std::clamp(z_at(l), db.zmin, db.zmax), io.px[l], io.py[l], zbits[l]);
                    bool spass = !db.stencil || db.stencil_pass(io.px[l], io.py[l], t.front);
                    outcome[l] = !spass ? 2 : !zpass ? 1 : 0;
                    if (outcome[l]) io.cover &= ~(1ull << l);
                }
                if (!io.cover && !db.stencil) {  // nothing to write, nor a stencil operation to do
                    lanes = 0;
                    io.cover = 0;
                    return;
                }
            }
            for (u32 k = 0; k < ps_user && k < 16; k++) w.s[k] = r.sh[0x0c + k];
            w.s[ps_user] = 0;  // the primitive mask
            w.set_exec(covered);  // every pixel covered runs (whether it is killed decides the stencil's operation)
            io.tri = &t;
            io.exported = 0;
            io.zset = 0;
            if (ps) run(w, ps, io, mem);
            else io.exported = covered;  // no shader: nothing killed, no colour
            if (late)
                for (int l = 0; l < LANES; l++) {
                    if (!((io.exported & covered) >> l & 1) || outcome[l] == 2) continue;
                    float z = (io.zset >> l) & 1 ? f(io.zexp[l]) : z_at(l);
                    outcome[l] = db.pass(std::clamp(z, db.zmin, db.zmax), io.px[l], io.py[l], zbits[l]) ? 0 : 1;
                }
            if (db.any())
                for (int l = 0; l < LANES; l++) {
                    if (!((io.exported & covered) >> l & 1)) continue;
                    if (db.stencil) db.stencil_op(io.px[l], io.py[l], t.front, outcome[l]);
                    if (db.write && outcome[l] == 0) db.store(io.px[l], io.py[l], zbits[l]);
                }
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
    return saved(std::format("{} triangles, {} pixels", tris.size(), pixels.load()), "");
}

// The dispatch on the GPU: false (and why) if it can't be
// (`stored`: where it stores; `before`, for --gpu-check: what the memory held there)
bool gpu_compute(const GpuRegs& r, u64 cs, u32 gx, u32 gy, u32 gz, const Memory& mem, std::string& why,
                 std::vector<std::pair<u64, u64>>& stored, std::vector<std::vector<unsigned char>>* before) {
    auto it = g_programs.find(cs);
    if (it == g_programs.end()) it = g_programs.emplace(cs, translate::decode(cs, mem)).first;
    const translate::Program& prog = it->second;
    if (!prog.why.empty()) {
        why = "the shader has " + prog.why;
        return false;
    }
    u32 rsrc2 = r.sh[0x213], users = std::min((rsrc2 >> 1) & 0x1f, 16u);
    u32 user[16];
    for (u32 k = 0; k < 16; k++) user[k] = r.sh[0x240 + k];
    translate::Values vals;
    {
        vkdraw::Doing doing("evaluating a compute shader's scalar loads");
        vals = translate::evaluate(prog, user, users, mem, true);
    }
    if (!vals.why.empty()) {
        why = "the shader has " + vals.why;
        return false;
    }
    if (vals.stores.empty()) {
        why = "no buffer stores";
        return false;
    }
    vkdraw::Dispatch d;
    translate::CsState st;
    // a buffer's element as an image's texel: its memory, if it is laid out so (each element
    // where the last ends, as many bytes as its format's)
    auto access = [&](const translate::Values::Buffer& b, vkdraw::Dispatch::Access& a) {
        u64 base = b.v[0] | (u64(b.v[1] & 0xfff) << 32);
        u32 stride = (b.v[1] >> 16) & 0x3fff, bpp = translate::element_bytes(b.dfmt, b.nfmt);
        bool swizzled = (b.v[1] >> 31) & 1, add_tid = (b.v[3] >> 23) & 1;
        if (!bpp || stride != bpp || swizzled || add_tid || b.offset % bpp || !b.v[2]) return false;
        a = {base + b.offset, u64(b.v[2]) * bpp, b.v[2], bpp};
        return mem.ok(a.address, a.bytes);
    };
    for (auto& b : vals.stores) {
        vkdraw::Dispatch::Access a;
        if (!access(b, a)) {
            why = std::format("buffer stores of format {}, number type {}, stride {}", b.dfmt, b.nfmt, (b.v[1] >> 16) & 0x3fff);
            return false;
        }
        // memory that will be kept on the GPU, or already is (else it's the CPU's, before any work
        // here). Not part of a page: tried, and that memory, which draws and the CPU's dispatches
        // read too, kept being let go and read back (the-loader.md, item 68)
        if (!vkdraw::kept(a.address, a.bytes, a.bpp) && ((a.address | a.bytes) & 0xfff)) {
            why = "a store into memory not kept on the GPU, not whole pages";
            return false;
        }
        d.stores.push_back(a);
        stored.push_back({a.address, a.bytes});
        translate::CsState::Access ts{b.dfmt, b.nfmt, true, {}};
        ts.width = b.width, ts.height = b.height, ts.pitch = b.pitch;
        st.stores.push_back(ts);
    }
    // the loads: of memory kept on the GPU, from its image; of 32-bit values (one to four an
    // element), the memory as it is, the shader picking them out; else the elements unpacked
    // (in that order, as the shader's arrays are)
    std::vector<std::pair<u32, std::vector<u32>>> raw_slots;  // a load's place in U, and its three values
    std::vector<std::vector<u32>> arrays;
    for (u32 i = 0; i < vals.buffers.size(); i++) {
        const auto& b = vals.buffers[i];
        translate::CsState::Access ta{b.dfmt, b.nfmt, false, false, {b.v[3] & 7, (b.v[3] >> 3) & 7, (b.v[3] >> 6) & 7, (b.v[3] >> 9) & 7}};
        vkdraw::Dispatch::Access a;
        u64 base = b.v[0] | (u64(b.v[1] & 0xfff) << 32);
        u32 stride = (b.v[1] >> 16) & 0x3fff, records = b.v[2];
        bool raw_format = (b.dfmt == 4 || b.dfmt == 11 || b.dfmt == 13 || b.dfmt == 14) && (b.nfmt == 4 || b.nfmt == 5 || b.nfmt == 7);
        u32 dwords = b.dfmt == 4 ? 1 : b.dfmt == 11 ? 2 : b.dfmt == 13 ? 3 : 4;
        u64 span = stride ? u64(stride) * (records ? records - 1 : 0) + b.offset + dwords * 4 : 0;
        bool swizzled = (b.v[1] >> 31) & 1, add_tid = (b.v[3] >> 23) & 1;
        if (access(b, a) && vkdraw::kept(a.address, a.bytes, a.bpp)) {
            ta.image = true;
            d.loads.push_back(a);
            d.load_index.push_back(i);
        } else if (raw_format && stride && stride % 4 == 0 && b.offset % 4 == 0 && records && !swizzled && !add_tid && mem.ok(base, span) &&
                   span <= (u64(256) << 20)) {
            ta.raw = true;
            // kept on the GPU: copied there (else read back to be read here)
            if (vkdraw::version(base) || vkdraw::version(base + span - 1)) {
                arrays.emplace_back();
                d.array_from.resize(arrays.size());
                d.array_from.back() = {base, span};
            } else {
                std::vector<u32> data((span + 3) / 4);
                vkdraw::Doing doing("copying a compute shader's array");
                std::memcpy(data.data(), reinterpret_cast<const void*>(base), span);
                arrays.push_back(std::move(data));
            }
            raw_slots.push_back({i, {b.offset / 4, stride / 4, records}});
        } else {
            std::vector<vkdraw::Buffer> one;
            if (!unpack_buffers({b}, mem, one, why)) return false;
            arrays.push_back(*one[0]);
        }
        st.loads.push_back(ta);
    }
    d.array_from.resize(arrays.size());
    d.arrays = std::move(arrays);
    st.user_count = users;
    st.rsrc2 = rsrc2;
    st.threads[0] = std::max(r.sh[0x207] & 0xffff, 1u);
    st.threads[1] = std::max(r.sh[0x208] & 0xffff, 1u);
    st.threads[2] = std::max(r.sh[0x209] & 0xffff, 1u);
    st.store_slot = u32(vals.u.size());
    st.load_slot = st.store_slot + 3 * u32(st.stores.size());
    vals.u.resize(st.load_slot + 3 * st.loads.size());
    for (auto& [i, v] : raw_slots) std::copy(v.begin(), v.end(), vals.u.begin() + st.load_slot + 3 * i);
    // translated once for the program and all of this
    // the textures it reads, as a draw's; sampled at level 0, so the same filter near and far
    std::vector<const translate::Values::Image*> unique = sort_images(vals.images, st.sampled);
    for (const auto* im : unique)
        if (((im->s[2] >> 20) & 3) != ((im->s[2] >> 22) & 3)) {
            why = "images sampled in a compute shader, filtered one way near and another far";
            return false;
        }
    std::string gkey = std::format("cs:{:x}:{}:{:x}:{}.{}.{}:{}:{}:{}", cs, users, rsrc2, st.threads[0], st.threads[1], st.threads[2],
                                   st.store_slot, st.load_slot, st.sampled.key());
    for (auto& a : st.stores) gkey += std::format(":s{}.{}.{}x{}p{}", a.dfmt, a.nfmt, a.width, a.height, a.pitch);
    for (auto& a : st.loads) gkey += std::format(":l{}.{}.{}{}.{}{}{}{}", a.dfmt, a.nfmt, a.image, a.raw, a.sel[0], a.sel[1], a.sel[2], a.sel[3]);
    auto git = g_glsl.find(gkey);
    if (git == g_glsl.end()) git = g_glsl.emplace(gkey, translate::compute_shader(prog, st)).first;
    const translate::Glsl& gl = git->second;
    if (!gl.why.empty()) {
        why = "the shader has " + gl.why;
        return false;
    }
    save_glsl("cs", cs, gl.text);
    if (!textures_for(unique, mem, d.textures, why)) return false;
    d.cs = gl.text;
    d.values = vals.u;
    d.store_slot = st.store_slot;
    d.load_slot = st.load_slot;
    d.groups[0] = gx, d.groups[1] = gy, d.groups[2] = gz;
    if (before) {  // --gpu-check: the memory as it was, to be put back for the CPU
        vkdraw::Doing doing("--gpu-check: a dispatch's stores, the memory before");
        for (auto& [at, bytes] : stored)
            before->emplace_back(reinterpret_cast<const unsigned char*>(at), reinterpret_cast<const unsigned char*>(at + bytes));
    }
    return vkdraw::dispatch(d, why);
}

std::string gpu_dispatch(const GpuRegs& r, u32 gx, u32 gy, u32 gz) {
    if (!M.draw) return {};
    std::lock_guard draw_lock(g_draw_lock);
    Timer timer(g_dispatch_ns);
    u64 cs = program(r, 0x20c);
    if (!cs) return "(no compute shader)";
    g_dispatches++;
    g_page_epoch++;
    vkdraw::Doing doing("preparing a dispatch");
    Memory mem;
    mem.snapshot();
    u32 rsrc2 = r.sh[0x213];
    u32 user = (rsrc2 >> 1) & 0x1f;
    u32 tx = std::max(r.sh[0x207] & 0xffff, 1u), ty = std::max(r.sh[0x208] & 0xffff, 1u), tz = std::max(r.sh[0x209] & 0xffff, 1u);
    u32 threads = tx * ty * tz, nwaves = (threads + LANES - 1) / LANES;
    u64 groups = u64(gx) * gy * gz;
    std::string cpu_why;
    std::vector<std::pair<u64, u64>> stored;
    std::vector<std::vector<unsigned char>> by_gpu, before;  // --gpu-check: what the GPU stored, to compare
    if (M.gpu) {
        std::string why;
        auto seen = g_dispatch_checked.find(cs);
        bool check = M.gpu_check && g_dispatch_checks < DISPATCH_CHECKS && (seen == g_dispatch_checked.end() || seen->second.first < CHECKS_A_SHADER);
        if (gpu_compute(r, cs, gx, gy, gz, mem, why, stored, check ? &before : nullptr)) {
            g_gpu_dispatches++;
            g_gpu_groups += groups;
            if (!check) return std::format("on the GPU: {} groups", groups);
            g_dispatch_checks++;
            cpu_why = "--gpu-check's: on the GPU, then on the CPU to compare";
            vkdraw::Doing doing("--gpu-check: a dispatch's stores, as the GPU did them");
            // all of it first, then the memory as it was, for the CPU (two stores may be into
            // the same memory)
            for (auto [at, bytes] : stored)
                by_gpu.emplace_back(reinterpret_cast<const unsigned char*>(at), reinterpret_cast<const unsigned char*>(at + bytes));
            for (size_t i = 0; i < stored.size(); i++) std::memcpy(reinterpret_cast<void*>(stored[i].first), before[i].data(), stored[i].second);
        } else {
            cpu_why = std::move(why);
        }
    }
    // (named by why it is the CPU's, for the read-backs it brings: each name kept for the run)
    static std::set<std::string> on_cpu_names;
    vkdraw::Doing on_cpu(cpu_why.empty() ? "a dispatch on the CPU"
                                         : on_cpu_names.insert(std::format("a dispatch on the CPU ({})", cpu_why)).first->c_str());
    struct Tally {  // (the CPU's time on it, by why the GPU didn't do it)
        const std::string& why;
        u64 groups;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Tally() {
            if (!M.gpu) return;
            auto& v = g_gpu_dispatch_skipped[why];
            v[0]++, v[1] += groups;
            v[2] += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        }
    } tally{cpu_why, groups};
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
    if (!by_gpu.empty()) {  // (the CPU's stores stand)
        vkdraw::Doing doing("--gpu-check: a dispatch's stores, as the CPU did them");
        bool same = true;
        for (size_t i = 0; i < stored.size(); i++)
            same = same && std::memcmp(by_gpu[i].data(), reinterpret_cast<const void*>(stored[i].first), stored[i].second) == 0;
        auto& c = g_dispatch_checked[cs];
        c.first++, c.second += same;
        return std::format("on the GPU and the CPU: {} groups, {}", groups, same ? "the same" : "differing");
    }
    return std::format("{} groups run", groups);
}

std::vector<std::string> draw_report() {
    if (!M.draw) return {"## Drawing\n", "Not done (`--no-draw`).\n"};
    std::vector<std::string> s = {
        "## Drawing\n",
        std::format("{} draws and {} dispatches. On the CPU: {} triangles ({} clipped to the guard band first), {} pixels ({} "
                    "pixel-shader waves), and the dispatches' {} groups.\n",
                    g_draws.load(), g_dispatches.load(), g_triangles.load(), g_clipped.load(), g_pixels.load(), g_ps_waves.load(),
                    g_groups.load()),
        std::format("Depth buffers cleared the fast way (HTILE): {} times; depth and stencil buffers cleared by a draw: {} times.\n",
                    g_depth_clears, g_draw_clears),
        std::format("The draws took {:.1f} s and the dispatches {:.1f} s, of the run's {:.1f}.\n", double(g_draw_ns) / 1e9,
                    double(g_dispatch_ns) / 1e9,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - M.t0).count())};
    {
        std::string t = "Where the draws' time went:\n\n| Stage | Seconds |\n|---|---:|\n";
        for (int k = 0; k < ST_COUNT; k++)
            if (g_stage_ns[k]) t += std::format("| {} | {:.1f} |\n", stage_names[k], double(g_stage_ns[k]) / 1e9);
        t += std::format("\nOf which: translating and evaluating shaders {:.1f} s; unpacking buffers {:.1f} s; telling "
                         "whether textures' memory changed {:.1f} s ({} MiB hashed); sending {} as their compressed blocks {:.1f} s; "
                         "{} as their texels as they are (their tiling undone) {:.1f} s; decoding {} texel by texel {:.1f} s.\n",
                         double(g_translate_ns) / 1e9, double(g_buffer_unpack_ns) / 1e9, double(g_texture_hash_ns) / 1e9,
                         g_texture_hash_bytes >> 20, u64(g_texture_blocks), double(g_texture_blocks_ns) / 1e9,
                         u64(g_texture_copied), double(g_texture_copy_ns) / 1e9, u64(g_texture_decoded), double(g_texture_decode_ns) / 1e9);
        if (!g_prepare_ns.empty()) {
            std::vector<std::pair<u64, std::string>> by;
            for (auto& [what, ns] : g_prepare_ns) by.push_back({ns, what});
            std::sort(by.rbegin(), by.rend());
            t += "\nPreparing the draws for the GPU, by part:\n\n| Part | Seconds |\n|---|---:|\n";
            for (auto& [ns, what] : by) t += std::format("| {} | {:.2f} |\n", what, double(ns) / 1e9);
        }
        if (!g_setup_ns.empty()) {
            std::vector<std::pair<u64, std::string>> by;
            for (auto& [what, ns] : g_setup_ns) by.push_back({ns, what});
            std::sort(by.rbegin(), by.rend());
            t += "\nSetting the draws up, by part:\n\n| Part | Seconds |\n|---|---:|\n";
            for (auto& [ns, what] : by) t += std::format("| {} | {:.2f} |\n", what, double(ns) / 1e9);
        }
        if (!g_decoded_by.empty()) {
            std::vector<std::pair<std::pair<u64, u64>, std::string>> by;
            for (auto& [what, v] : g_decoded_by) by.push_back({v, what});
            std::sort(by.begin(), by.end(), [](auto& a, auto& b) { return a.first.second > b.first.second; });
            t += "\nTextures decoded texel by texel:\n\n| Format | Times | Seconds |\n|---|---:|---:|\n";
            for (size_t i = 0; i < by.size() && i < 20; i++)
                t += std::format("| {} | {} | {:.1f} |\n", by[i].second, by[i].first.first, double(by[i].first.second) / 1e9);
            t += "\n";
        }
        t += std::format("Buffers: {} unpacked, {} found kept ({} times the least lately used let go); telling whether their "
                         "memory changed {:.1f} s ({} MiB hashed), unpacking them {:.1f} s.\n",
                         g_unpacks, g_unpack_hits, g_unpack_clears, double(g_buffer_hash_ns) / 1e9, g_buffer_hash_bytes >> 20,
                         double(g_buffer_miss_ns) / 1e9);
        t += std::format("The OS asked which pages were written: {} times, {:.1f} s.\n", u64(g_os_asked), double(g_os_ask_ns) / 1e9);
        t += std::format("Memory told changed or not: {} times from the pages the OS saw written (not hashed again), {} times by hashing it.\n",
                         g_stamps_watched, g_stamps_hashed);
        s.push_back(t);
    }
    if (M.gpu) {
        std::lock_guard lock(g_draw_lock);
        std::string where = vkdraw::device();
        s.push_back(std::format("On the GPU{}: {} draws, {} of them with their vertex shaders there too; the CPU drew "
                                "the rest.\n",
                                where.empty() ? "" : ", " + where, g_gpu_draws, g_gpu_vertices));
        if (std::string rep = vkdraw::report(); !rep.empty()) s.push_back(rep + ".\n");
        if (!g_gpu_skipped.empty()) {
            s.insert(s.end(), {"Drawn on the CPU instead:\n", "| Why | Draws |", "|---|---:|"});
            for (auto& [why, count] : g_gpu_skipped) s.push_back(std::format("| {} | {} |", why, count));
            s.push_back("");
        }
        if (!g_vertex_skipped.empty()) {
            s.insert(s.end(), {"Drawn on the GPU, the vertex shader run on the CPU:\n", "| Why | Draws | Vertices | Seconds |",
                               "|---|---:|---:|---:|"});
            for (auto& [why, v] : g_vertex_skipped)
                s.push_back(std::format("| {} | {} | {} | {:.1f} |", why, v[0], v[1], double(v[2]) / 1e9));
            s.push_back("");
        }
        s.push_back(std::format("Dispatches on the GPU: {}, of {} groups; the CPU ran the rest.\n", g_gpu_dispatches, g_gpu_groups));
        if (!g_gpu_dispatch_skipped.empty()) {
            s.insert(s.end(), {"Dispatches run on the CPU instead:\n", "| Why | Dispatches | Groups | Seconds |", "|---|---:|---:|---:|"});
            for (auto& [why, v] : g_gpu_dispatch_skipped)
                s.push_back(std::format("| {} | {} | {} | {:.1f} |", why, v[0], v[1], double(v[2]) / 1e9));
            s.push_back("");
        }
    }
    if (M.gpu_check) {
        std::lock_guard lock(g_draw_lock);
        std::string where = vkdraw::device();
        s.push_back(std::format("GPU check (`--gpu-check`){}: of the draws saved to draws/, {} were drawn on the GPU as well, "
                                "{} of them the same as the CPU's in every pixel (to 1 in 255), and {} with their vertex "
                                "shaders on the GPU too; of those that write depth, {} with depth differing; see draws/index.txt.\n",
                                where.empty() ? "" : ", " + where, g_checked, g_check_same, g_check_gpu_vertices, g_check_depth_differ));
        if (!g_check_skipped.empty()) {
            s.insert(s.end(), {"Not drawn on the GPU:\n", "| Why | Draws |", "|---|---:|"});
            for (auto& [why, count] : g_check_skipped) s.push_back(std::format("| {} | {} |", why, count));
            s.push_back("");
        }
        if (!g_dispatch_checked.empty()) {
            u64 n = 0, same = 0;
            for (auto& [cs, c] : g_dispatch_checked) n += c.first, same += c.second;
            s.push_back(std::format("Dispatches done on the GPU and then on the CPU (at most {} of a shader, {} in all): {}, {} of "
                                    "them storing the same in every byte.\n",
                                    CHECKS_A_SHADER, DISPATCH_CHECKS, n, same));
            s.insert(s.end(), {"| Compute shader | Dispatches | The same |", "|---|---:|---:|"});
            for (auto& [cs, c] : g_dispatch_checked) s.push_back(std::format("| {:#x} | {} | {} |", cs, c.first, c.second));
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
