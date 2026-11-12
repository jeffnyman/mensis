// SPDX-License-Identifier: GPL-3.0-or-later
//
// The graphics driver (libSceGnmDriver): what the game asks of the GPU's driver.
//
// Most of the driver's calls write PM4 packets (the GPU's commands) into a command buffer the
// game is filling. The game reserves the space first and moves on by exactly that much once a
// call succeeds (the default-state calls instead say how much they wrote), so each call here
// fills the whole space: the registers the game's structures hold, then a NOP to the end. What
// the real driver writes besides is not known here; the NOPs keep the buffer well-formed and
// the same length. Submitted buffers go to the command processor (gpu.cpp).

#include <atomic>
#include <cstring>
#include <format>
#include <initializer_list>

#include "loader.hpp"

namespace mensis {
namespace {

// The driver's errors
constexpr i64 GNM_ERROR_VALIDATION_NOT_ENABLED = i64(std::int32_t(0x80d13fff));
constexpr i64 GNM_ERROR_COMPUTEQUEUE_INVALID_PIPE_ID = i64(std::int32_t(0x80d17000));
constexpr i64 GNM_ERROR_COMPUTEQUEUE_INVALID_QUEUE_ID = i64(std::int32_t(0x80d17001));

// A type 3 packet's header: op, and the dwords after the header. Bit 1 marks a compute packet.
u32 pm4(u32 op, u32 body, bool compute = false) {
    return 0xc0000000u | ((body - 1) << 16) | (op << 8) | (compute ? 2u : 0u);
}

constexpr u32 NOP = 0x10, CONTEXT_CONTROL = 0x28, CLEAR_STATE = 0x12;
constexpr u32 SET_CONTEXT_REG = 0x69, SET_SH_REG = 0x76;

// Writes packets into the game's command buffer, dwords at a time.
struct Writer {
    u32* p;
    u32* end;
    Writer(u64 at, u64 dwords) : p(reinterpret_cast<u32*>(at)), end(p + dwords) {}
    void regs(u32 op, u32 reg, std::initializer_list<u32> values, bool compute = false) {
        *p++ = pm4(op, u32(values.size()) + 1, compute);
        *p++ = reg;
        for (u32 v : values) *p++ = v;
    }
    // A NOP over the rest (a lone dword is a type 2 filler).
    void fill() {
        u64 n = u64(end - p);
        if (n == 1) *p = 0x80000000u;
        if (n < 2) return;
        *p = pm4(NOP, u32(n - 1));
        std::memset(p + 1, 0, (n - 1) * 4);
        p = end;
    }
};

// sceGnmGetTheTessellationFactorRingBufferBaseAddress(): where the GPU expects the ring. The
// game then maps 128 KiB of direct memory there; the loader maps direct memory 1:1, so the
// address is a request it doesn't honour, which changes nothing while no GPU reads the ring.
i64 tess_ring_address(const u64*, std::string&) { return i64(0xFF0000000); }

// sceGnmValidateCommandBuffers(...): a retail driver has no validation built in
i64 validate(const u64*, std::string& note) {
    note = "validation not enabled (as on a retail console)";
    return GNM_ERROR_VALIDATION_NOT_ENABLED;
}

// sceGnmMapComputeQueue(pipe, queue, ring base, ring size in dwords, read pointer address): a
// compute queue's id. The PS4 has 7 compute pipes of 8 queues each; ids start at 1, and the
// game treats 0 as failure. The game writes packets into the ring and reads, at the read
// pointer's address, how far the GPU has got.
std::atomic<int> g_next_queue{1};

i64 map_compute_queue(const u64* a, std::string& note) {
    if (a[0] > 6) return GNM_ERROR_COMPUTEQUEUE_INVALID_PIPE_ID;
    if (a[1] > 7) return GNM_ERROR_COMPUTEQUEUE_INVALID_QUEUE_ID;
    int id = g_next_queue++;
    gpu_map_compute(id, a[2], u32(a[3]), a[4]);
    note = std::format("queue {}: pipe {}, queue {}, ring {:#x} ({:#x} dwords), read pointer at {:#x}",
                       id, a[0], a[1], a[2], u32(a[3]), a[4]);
    return id;
}

// sceGnmDingDong(queue id, offset in dwords): the ring is written up to offset; run it
i64 ding_dong(const u64* a, std::string&) {
    gpu_ding_dong(int(a[0]), u32(a[1]));
    return 0;
}

// sceGnmDrawInitDefaultHardwareState200 / DispatchInitDefaultHardwareState(cmd, dwords free):
// the GPU's default state; returns how many dwords it wrote (0 if there wasn't room). The
// real driver's 256 dwords set many registers; here, the context control and a clear to the
// defaults, then filler.
i64 init_default_state(const u64* a, std::string&) {
    constexpr u32 SIZE = 0x100;
    if (u32(a[1]) < SIZE) return 0;
    Writer w(a[0], SIZE);
    *w.p++ = pm4(CONTEXT_CONTROL, 2);
    *w.p++ = 0x80000000u;  // load enable
    *w.p++ = 0x80000000u;  // shadow enable
    *w.p++ = pm4(CLEAR_STATE, 1);
    *w.p++ = 0;
    w.fill();
    return SIZE;
}

i64 init_dispatch_state(const u64* a, std::string&) {
    constexpr u32 SIZE = 0x100;
    if (u32(a[1]) < SIZE) return 0;
    Writer(a[0], SIZE).fill();
    return SIZE;
}

// The shader calls: (cmd, dwords reserved, the stage's registers, ...). The registers come as
// the SDK's structures, consecutive 32-bit fields: the program's address (>> 8, low and high),
// its two resource words, then what the stage's setup needs.

// sceGnmSetVsShader(cmd, dwords, VsStageRegisters*, shader modifier): the modifier replaces
// some of the first resource word's bits.
i64 set_vs_shader(const u64* a, std::string& note) {
    const u32* r = reinterpret_cast<const u32*>(a[2]);
    if (!a[0] || !r || u32(a[1]) < 17) return -1;
    u32 modifier = u32(a[3]);
    u32 rsrc1 = modifier ? (r[2] & 0xfcfffc3fu) | modifier : r[2];
    Writer w(a[0], u32(a[1]));
    w.regs(SET_SH_REG, 0x48, {r[0], r[1]});  // SPI_SHADER_PGM_LO_VS, HI
    w.regs(SET_SH_REG, 0x4a, {rsrc1, r[3]});  // SPI_SHADER_PGM_RSRC1_VS, RSRC2
    w.regs(SET_CONTEXT_REG, 0x1b1, {r[4]});  // SPI_VS_OUT_CONFIG
    w.regs(SET_CONTEXT_REG, 0x1c3, {r[5]});  // SPI_SHADER_POS_FORMAT
    w.regs(SET_CONTEXT_REG, 0x207, {r[6]});  // PA_CL_VS_OUT_CNTL
    w.fill();
    note = std::format("program at {:#x}", u64(r[0]) << 8 | u64(r[1]) << 40);
    return 0;
}

// sceGnmSetPsShader(cmd, dwords, PsStageRegisters*): null means no pixel shader
i64 set_ps_shader(const u64* a, std::string& note) {
    const u32* r = reinterpret_cast<const u32*>(a[2]);
    if (!a[0] || u32(a[1]) < 28) return -1;
    Writer w(a[0], u32(a[1]));
    if (!r) {
        w.regs(SET_SH_REG, 0x08, {0, 0});
        w.fill();
        note = "none";
        return 0;
    }
    w.regs(SET_SH_REG, 0x08, {r[0], r[1]});  // SPI_SHADER_PGM_LO_PS, HI
    w.regs(SET_SH_REG, 0x0a, {r[2], r[3]});  // SPI_SHADER_PGM_RSRC1_PS, RSRC2
    w.regs(SET_CONTEXT_REG, 0x1c4, {r[4], r[5]});  // SPI_SHADER_Z_FORMAT, COL_FORMAT
    w.regs(SET_CONTEXT_REG, 0x1b3, {r[6], r[7]});  // SPI_PS_INPUT_ENA, ADDR
    w.regs(SET_CONTEXT_REG, 0x1b6, {r[8]});  // SPI_PS_IN_CONTROL
    w.regs(SET_CONTEXT_REG, 0x1b8, {r[9]});  // SPI_BARYC_CNTL
    w.regs(SET_CONTEXT_REG, 0x203, {r[10]});  // DB_SHADER_CONTROL
    w.regs(SET_CONTEXT_REG, 0x08f, {r[11]});  // CB_SHADER_MASK
    w.fill();
    note = std::format("program at {:#x}", u64(r[0]) << 8 | u64(r[1]) << 40);
    return 0;
}

// sceGnmSetCsShader(cmd, dwords, CsStageRegisters*): the program, and the threads in a group
i64 set_cs_shader(const u64* a, std::string& note) {
    const u32* r = reinterpret_cast<const u32*>(a[2]);
    if (!a[0] || !r || u32(a[1]) < 13) return -1;
    Writer w(a[0], u32(a[1]));
    w.regs(SET_SH_REG, 0x20c, {r[0], r[1]}, true);  // COMPUTE_PGM_LO, HI
    w.regs(SET_SH_REG, 0x212, {r[2], r[3]}, true);  // COMPUTE_PGM_RSRC1, RSRC2
    w.regs(SET_SH_REG, 0x207, {r[4], r[5], r[6]}, true);  // COMPUTE_NUM_THREAD_X, Y, Z
    w.fill();
    note = std::format("program at {:#x}, {}x{}x{} threads", u64(r[0]) << 8 | u64(r[1]) << 40,
                       r[4] & 0xffff, r[5] & 0xffff, r[6] & 0xffff);
    return 0;
}

// sceGnmInsertPushMarker(cmd, dwords, name) / InsertPopMarker(cmd, dwords): debugging markers,
// which name a stretch of commands for a GPU capture tool. The driver puts them in NOPs; here
// the NOP holds 1 (push) and the name, or 2 (pop), for gpu.cpp's listings.
i64 push_marker(const u64* a, std::string& note) {
    u32 n = u32(a[1]);
    if (!a[0] || n < 2) return -1;
    Writer w(a[0], n);
    w.fill();
    u32* body = reinterpret_cast<u32*>(a[0]) + 1;
    body[0] = 1;
    const char* name = a[2] ? reinterpret_cast<const char*>(a[2]) : "";
    std::strncpy(reinterpret_cast<char*>(body + 1), name, (n - 2) * 4 - 1);
    note = std::format("\"{}\"", name);
    return 0;
}

i64 pop_marker(const u64* a, std::string&) {
    u32 n = u32(a[1]);
    if (!a[0] || n < 2) return -1;
    Writer w(a[0], n);
    w.fill();
    reinterpret_cast<u32*>(a[0])[1] = 2;
    return 0;
}

// sceGnmSubmitCommandBuffers(count, void *dcb[], u32 dcb_bytes[], void *ccb[], u32 ccb_bytes[]):
// the draw command buffers (dcb) and constant command buffers (ccb), to the graphics queue
i64 submit(const u64* a, std::string& note) {
    u32 count = u32(a[0]);
    gpu_submit(count, reinterpret_cast<const u64*>(a[1]), reinterpret_cast<const u32*>(a[2]),
               reinterpret_cast<const u64*>(a[3]), reinterpret_cast<const u32*>(a[4]), {});
    note = std::format("{} buffer(s)", count);
    return 0;
}

i64 nothing(const u64*, std::string&) { return 0; }
i64 yes(const u64*, std::string&) { return 1; }

}  // namespace

void add_gnm_standins(std::unordered_map<std::string, Standin>& t) {
    std::pair<const char*, Standin> list[] = {
        {"sceGnmGetTheTessellationFactorRingBufferBaseAddress", tess_ring_address},
        {"sceGnmValidateCommandBuffers", validate},
        {"sceGnmMapComputeQueue", map_compute_queue},
        {"sceGnmMapComputeQueueWithPriority", map_compute_queue},
        {"sceGnmUnmapComputeQueue", nothing},
        {"sceGnmDingDong", ding_dong},
        {"sceGnmDrawInitDefaultHardwareState200", init_default_state},
        {"sceGnmDispatchInitDefaultHardwareState", init_dispatch_state},
        {"sceGnmSetVsShader", set_vs_shader},
        {"sceGnmSetPsShader", set_ps_shader},
        {"sceGnmSetCsShader", set_cs_shader},
        {"sceGnmInsertPushMarker", push_marker},
        {"sceGnmInsertPopMarker", pop_marker},
        {"sceGnmSubmitCommandBuffers", submit},
        {"sceGnmSubmitDone", nothing},
        {"sceGnmAreSubmitsAllowed", yes},
        {"sceGnmFlushGarlic", nothing},
        {"sceGnmIsUserPaEnabled", nothing},  // a debugging feature; off
        {"sceGnmDriverCaptureInProgress", nothing},
    };
    for (auto [name, fn] : list) t.insert_or_assign(name, fn);
}

}  // namespace mensis
