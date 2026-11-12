// SPDX-License-Identifier: GPL-3.0-or-later
//
// The command processor: what the GPU's front end does with what the game submits, short of
// drawing. The game writes PM4 packets (AMD's command format) into command buffers and hands
// them to the driver: whole buffers for the graphics queue (sceGnmSubmitCommandBuffers), or
// packets in a ring for a compute queue (sceGnmDingDong says how far the ring is written).
//
// Each queue here has a thread of its own that walks the packets in order. Draws and dispatches
// are counted, not run: there are no shaders yet. What a packet writes to memory is written,
// and a wait for memory waits, because the game and the GPU keep each other in step through
// memory: a fence value written at the end of some work, which the CPU (or another queue) waits
// to see. The first few submissions of each queue are also written out, decoded, to
// <out>/gpu/.

#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>

#include "loader.hpp"

namespace mensis {
namespace {

constexpr int LISTED = 6;  // submissions decoded to <out>/gpu/, per queue

const char* op_name(u32 op) {
    switch (op) {
        case 0x10: return "NOP";
        case 0x11: return "SET_BASE";
        case 0x12: return "CLEAR_STATE";
        case 0x13: return "INDEX_BUFFER_SIZE";
        case 0x15: return "DISPATCH_DIRECT";
        case 0x16: return "DISPATCH_INDIRECT";
        case 0x1d: return "ATOMIC_GDS";
        case 0x1e: return "ATOMIC_MEM";
        case 0x1f: return "OCCLUSION_QUERY";
        case 0x20: return "SET_PREDICATION";
        case 0x21: return "REG_RMW";
        case 0x22: return "COND_EXEC";
        case 0x23: return "PRED_EXEC";
        case 0x24: return "DRAW_INDIRECT";
        case 0x25: return "DRAW_INDEX_INDIRECT";
        case 0x26: return "INDEX_BASE";
        case 0x27: return "DRAW_INDEX_2";
        case 0x28: return "CONTEXT_CONTROL";
        case 0x2a: return "INDEX_TYPE";
        case 0x2c: return "DRAW_INDIRECT_MULTI";
        case 0x2d: return "DRAW_INDEX_AUTO";
        case 0x2f: return "NUM_INSTANCES";
        case 0x30: return "DRAW_INDEX_MULTI_AUTO";
        case 0x33: return "INDIRECT_BUFFER_CONST";
        case 0x34: return "STRMOUT_BUFFER_UPDATE";
        case 0x35: return "DRAW_INDEX_OFFSET_2";
        case 0x36: return "DRAW_PREAMBLE";
        case 0x37: return "WRITE_DATA";
        case 0x38: return "DRAW_INDEX_INDIRECT_MULTI";
        case 0x39: return "MEM_SEMAPHORE";
        case 0x3b: return "COPY_DW";
        case 0x3c: return "WAIT_REG_MEM";
        case 0x3f: return "INDIRECT_BUFFER";
        case 0x40: return "COPY_DATA";
        case 0x42: return "PFP_SYNC_ME";
        case 0x43: return "SURFACE_SYNC";
        case 0x45: return "COND_WRITE";
        case 0x46: return "EVENT_WRITE";
        case 0x47: return "EVENT_WRITE_EOP";
        case 0x48: return "EVENT_WRITE_EOS";
        case 0x49: return "RELEASE_MEM";
        case 0x4a: return "PREAMBLE_CNTL";
        case 0x50: return "DMA_DATA";
        case 0x58: return "ACQUIRE_MEM";
        case 0x59: return "REWIND";
        case 0x5f: return "LOAD_SH_REG";
        case 0x60: return "LOAD_CONFIG_REG";
        case 0x61: return "LOAD_CONTEXT_REG";
        case 0x68: return "SET_CONFIG_REG";
        case 0x69: return "SET_CONTEXT_REG";
        case 0x73: return "SET_CONTEXT_REG_INDIRECT";
        case 0x76: return "SET_SH_REG";
        case 0x77: return "SET_SH_REG_OFFSET";
        case 0x78: return "SET_QUEUE_REG";
        case 0x79: return "SET_UCONFIG_REG";
        case 0x7d: return "SCRATCH_RAM_WRITE";
        case 0x7e: return "SCRATCH_RAM_READ";
        case 0x80: return "LOAD_CONST_RAM";
        case 0x81: return "WRITE_CONST_RAM";
        case 0x83: return "DUMP_CONST_RAM";
        case 0x84: return "INCREMENT_CE_COUNTER";
        case 0x85: return "INCREMENT_DE_COUNTER";
        case 0x86: return "WAIT_ON_CE_COUNTER";
        case 0x88: return "WAIT_ON_DE_COUNTER_DIFF";
        case 0x8b: return "SWITCH_BUFFER";
        default: return nullptr;
    }
}

bool is_draw(u32 op) {
    return op == 0x24 || op == 0x25 || op == 0x27 || op == 0x2c || op == 0x2d || op == 0x30 ||
           op == 0x35 || op == 0x38;
}

// The names of the registers the driver's shader calls set (gnm.cpp), by their offset in each
// register space; the shaders' user data (their resource pointers) is a range.
std::string reg_name(u32 op, u32 reg) {
    static const std::map<std::pair<u32, u32>, const char*> names = {
        {{0x76, 0x008}, "SPI_SHADER_PGM_LO_PS"},   {{0x76, 0x009}, "SPI_SHADER_PGM_HI_PS"},
        {{0x76, 0x00a}, "SPI_SHADER_PGM_RSRC1_PS"}, {{0x76, 0x00b}, "SPI_SHADER_PGM_RSRC2_PS"},
        {{0x76, 0x048}, "SPI_SHADER_PGM_LO_VS"},   {{0x76, 0x049}, "SPI_SHADER_PGM_HI_VS"},
        {{0x76, 0x04a}, "SPI_SHADER_PGM_RSRC1_VS"}, {{0x76, 0x04b}, "SPI_SHADER_PGM_RSRC2_VS"},
        {{0x76, 0x207}, "COMPUTE_NUM_THREAD_X"},   {{0x76, 0x208}, "COMPUTE_NUM_THREAD_Y"},
        {{0x76, 0x209}, "COMPUTE_NUM_THREAD_Z"},   {{0x76, 0x20c}, "COMPUTE_PGM_LO"},
        {{0x76, 0x20d}, "COMPUTE_PGM_HI"},         {{0x76, 0x212}, "COMPUTE_PGM_RSRC1"},
        {{0x76, 0x213}, "COMPUTE_PGM_RSRC2"},      {{0x69, 0x08f}, "CB_SHADER_MASK"},
        {{0x69, 0x1b1}, "SPI_VS_OUT_CONFIG"},      {{0x69, 0x1b3}, "SPI_PS_INPUT_ENA"},
        {{0x69, 0x1b4}, "SPI_PS_INPUT_ADDR"},      {{0x69, 0x1b6}, "SPI_PS_IN_CONTROL"},
        {{0x69, 0x1b8}, "SPI_BARYC_CNTL"},         {{0x69, 0x1c3}, "SPI_SHADER_POS_FORMAT"},
        {{0x69, 0x1c4}, "SPI_SHADER_Z_FORMAT"},    {{0x69, 0x1c5}, "SPI_SHADER_COL_FORMAT"},
        {{0x69, 0x203}, "DB_SHADER_CONTROL"},      {{0x69, 0x207}, "PA_CL_VS_OUT_CNTL"},
    };
    if (auto it = names.find({op, reg}); it != names.end()) return it->second;
    if (op == 0x76) {
        if (reg >= 0x00c && reg < 0x01c) return std::format("SPI_SHADER_USER_DATA_PS_{}", reg - 0x00c);
        if (reg >= 0x04c && reg < 0x05c) return std::format("SPI_SHADER_USER_DATA_VS_{}", reg - 0x04c);
        if (reg >= 0x240 && reg < 0x250) return std::format("COMPUTE_USER_DATA_{}", reg - 0x240);
    }
    const char* space = op == 0x76 ? "SH" : op == 0x69 ? "CONTEXT" : op == 0x68 ? "CONFIG" : "UCONFIG";
    return std::format("{}+{:#x}", space, reg);
}

// The GPU sees the same addresses as the CPU (direct memory is mapped 1:1), but only memory the
// game was given: a packet that points anywhere else is skipped, not followed.
bool in_heap(u64 addr, u64 bytes) {
    std::lock_guard lock(M.heap_lock);
    return addr >= M.heap_base && addr + bytes >= addr && addr + bytes <= M.heap_top;
}

u64 gpu_clock() {  // the GPU's 100 MHz counter, as EVENT_WRITE_EOP's "write the time" sends it
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - M.t0).count() / 10);
}

struct Job {
    std::vector<std::pair<u64, u32>> buffers;  // address, dwords: the draw buffers, run in order
    std::vector<std::pair<u64, u32>> constants;  // the constant buffers: listed, not run
    std::function<void()> then;  // after the job: a flip
    u32 ring_end = 0;  // compute: the read pointer to report once done
};

struct Queue {
    std::string name;  // "graphics", "compute-1"
    bool compute = false;
    u64 ring = 0, rptr_addr = 0;
    u32 ring_dwords = 0, rptr = 0;

    std::mutex m;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::thread worker;

    // under g_stats_lock
    u64 submissions = 0, packets = 0, draws = 0, dispatches = 0;
    std::string waiting;  // the wait it's in now, if any
};

std::mutex g_stats_lock;
std::map<std::string, u64> g_ops;  // packets of each kind, all queues
std::map<int, std::unique_ptr<Queue>> g_queues;  // 0 is the graphics queue; compute ids from 1
std::mutex g_queues_lock;

struct Walk {
    Queue& q;
    std::ofstream* out;  // the listing, if this job is listed
    void line(int depth, const std::string& s) {
        if (out) *out << std::string(size_t(depth) * 4, ' ') << s << "\n";
    }

    // The packets at addr, dwords long; run (memory writes and waits) unless list_only.
    void run(u64 addr, u32 dwords, int depth, bool list_only) {
        if (!in_heap(addr, u64(dwords) * 4)) {
            line(depth, std::format("({:#x}, {} dwords: not in the game's memory; skipped)", addr, dwords));
            return;
        }
        const u32* p = reinterpret_cast<const u32*>(addr);
        u32 i = 0;
        while (i < dwords) {
            u32 h = p[i];
            u32 type = h >> 30;
            if (type == 2) {  // a one-dword filler
                i++;
                continue;
            }
            u32 body = ((h >> 16) & 0x3fff) + 1;
            if (type == 0) {
                line(depth, std::format("{:5} type 0: {} register(s) from {:#x}", i, body, h & 0xffff));
                i += 1 + body;
                continue;
            }
            if (type != 3 || i + 1 + body > dwords) {
                line(depth, std::format("{:5} {:08x}: not a packet that fits; the rest is skipped", i, h));
                break;
            }
            u32 op = (h >> 8) & 0xff;
            const u32* b = p + i + 1;
            const char* name = op_name(op);
            std::string label = name ? name : std::format("op {:#04x}", op);
            {
                std::lock_guard lock(g_stats_lock);
                g_ops[label]++;
                q.packets++;
                if (is_draw(op)) q.draws++;
                if (op == 0x15 || op == 0x16) q.dispatches++;
            }
            std::string what = list_only ? describe(op, b, body) : execute(op, b, body, depth, list_only);
            line(depth, std::format("{:5} {}{}", i, label, what.empty() ? "" : " " + what));
            if ((op == 0x3f || op == 0x33) && body >= 3) {  // an indirect buffer: its packets, in place
                u64 at = (b[0] & ~3u) | (u64(b[1] & 0xffff) << 32);
                if (depth < 4) run(at, b[2] & 0xfffff, depth + 1, list_only || op == 0x33);
            }
            i += 1 + body;
        }
    }

    static u64 addr_of(u32 lo, u32 hi) { return (lo & ~3u) | (u64(hi & 0xffff) << 32); }

    std::string describe(u32 op, const u32* b, u32 n) {
        switch (op) {
            case 0x68: case 0x69: case 0x76: case 0x79: {
                std::string s = reg_name(op, b[0] & 0xffff) + " =";
                for (u32 k = 1; k < n; k++) s += std::format(" {:#x}", b[k]);
                return s;
            }
            case 0x10:
                // a marker (gnm.cpp): 1 push or 2 pop, then the name
                if (n >= 2 && (b[0] == 1 || b[0] == 2)) {
                    const char* text = reinterpret_cast<const char*>(b + 1);
                    return b[0] == 1 ? std::format("marker push \"{}\"", std::string(text, strnlen(text, (n - 1) * 4)))
                                     : "marker pop";
                }
                return std::format("({} dwords)", n);
            case 0x3f: case 0x33:
                return n >= 3 ? std::format("{:#x}, {} dwords", addr_of(b[0], b[1]), b[2] & 0xfffff) : "";
            case 0x3c:
                return n >= 5 ? std::format("until [{:#x}] & {:#x} {} {:#x}", addr_of(b[1], b[2]), b[4],
                                            compare_name(b[0] & 7), b[3])
                              : "";
            case 0x15:
                return n >= 3 ? std::format("{} x {} x {} groups", b[0], b[1], b[2]) : "";
            case 0x2d:
                return n >= 1 ? std::format("{} indices", b[0]) : "";
            case 0x27:
                return n >= 4 ? std::format("{} indices from {:#x}", b[3], b[1] | (u64(b[2]) << 32)) : "";
            default:
                return "";
        }
    }

    static const char* compare_name(u32 f) {
        static const char* names[] = {"(always)", "<", "<=", "==", "!=", ">=", ">", "(?)"};
        return names[f & 7];
    }

    static bool compare(u32 f, u32 v, u32 ref) {
        switch (f & 7) {
            case 0: return true;
            case 1: return v < ref;
            case 2: return v <= ref;
            case 3: return v == ref;
            case 4: return v != ref;
            case 5: return v >= ref;
            case 6: return v > ref;
            default: return true;
        }
    }

    bool write32(u64 at, u32 v) {
        if (!in_heap(at, 4)) return false;
        reinterpret_cast<std::atomic<u32>*>(at)->store(v, std::memory_order_release);
        return true;
    }
    bool write64(u64 at, u64 v) {
        if (!in_heap(at, 8)) return false;
        reinterpret_cast<std::atomic<u64>*>(at)->store(v, std::memory_order_release);
        return true;
    }

    // An end-of-pipe or release write: data_sel 1 = 32 bits, 2 = 64, 3 = the GPU's clock. The
    // graphics queue's interrupt (int_sel) is the end-of-pipe event the driver reports to
    // sceGnmAddEqEvent's queues; a compute queue's have events of their own, not yet reported.
    std::string end_of_pipe(u64 at, u32 data_sel, u32 int_sel, u32 lo, u32 hi) {
        std::string s;
        bool ok = true;
        switch (data_sel) {
            case 1: ok = write32(at, lo); s = std::format("[{:#x}] = {:#x}", at, lo); break;
            case 2: ok = write64(at, lo | (u64(hi) << 32)); s = std::format("[{:#x}] = {:#x}", at, lo | (u64(hi) << 32)); break;
            case 3: ok = write64(at, gpu_clock()); s = std::format("[{:#x}] = the time", at); break;
            default: s = "(no write)";
        }
        if (!ok) s += " (not in the game's memory; skipped)";
        if (int_sel && !q.compute) {
            trigger_event(EVENT_GRAPHICS, GRAPHICS_EOP, 0);
            s += ", interrupt";
        } else if (int_sel) {
            s += ", interrupt (a compute queue's; not reported)";
        }
        return s;
    }

    std::string execute(u32 op, const u32* b, u32 n, int depth, bool list_only) {
        (void)depth;
        (void)list_only;
        switch (op) {
            case 0x37: {  // WRITE_DATA: control, address, data...
                if (n < 4) return "";
                u32 dst = (b[0] >> 8) & 0xf;
                if (dst != 2 && dst != 5) return std::format("to register {:#x}", b[1]);
                u64 at = b[1] | (u64(b[2]) << 32);
                bool one = (b[0] >> 16) & 1;
                bool ok = true;
                for (u32 k = 3; k < n; k++) ok = write32(at + (one ? 0 : (k - 3) * 4), b[k]) && ok;
                return std::format("[{:#x}] = {:#x}{}{}", at, b[3], n > 4 ? std::format(" (+{} more)", n - 4) : "",
                                   ok ? "" : " (not in the game's memory; skipped)");
            }
            case 0x47:  // EVENT_WRITE_EOP: event, address lo, address hi | int_sel | data_sel, data
                if (n < 5) return "";
                return end_of_pipe(addr_of(b[1], b[2]), b[2] >> 29, (b[2] >> 24) & 3, b[3], b[4]);
            case 0x49:  // RELEASE_MEM: event, int_sel | data_sel, address, data
                if (n < 6) return "";
                return end_of_pipe((b[2] & ~3u) | (u64(b[3]) << 32), b[1] >> 29, (b[1] >> 24) & 3, b[4], b[5]);
            case 0x48: {  // EVENT_WRITE_EOS: event, address lo, address hi | command, data
                if (n < 4) return "";
                u64 at = addr_of(b[1], b[2]);
                if ((b[2] >> 29) != 2) return "(GDS store; not done)";
                return std::format("[{:#x}] = {:#x}{}", at, b[3], write32(at, b[3]) ? "" : " (skipped)");
            }
            case 0x50: {  // DMA_DATA: control, source (or data), destination, byte count
                if (n < 6) return "";
                u32 src_sel = (b[0] >> 29) & 3, dst_sel = (b[0] >> 20) & 3;
                u64 src = b[1] | (u64(b[2]) << 32), dst = b[3] | (u64(b[4]) << 32);
                u32 bytes = b[5] & 0x1fffff;
                if ((dst_sel != 0 && dst_sel != 3) || src_sel == 1 || !in_heap(dst, bytes))
                    return std::format("{} bytes to {:#x} (not done)", bytes, dst);
                if (src_sel == 2) {
                    for (u32 k = 0; k + 4 <= bytes; k += 4) std::memcpy(reinterpret_cast<void*>(dst + k), &b[1], 4);
                    return std::format("fill {} bytes at {:#x} with {:#x}", bytes, dst, b[1]);
                }
                if (!in_heap(src, bytes)) return std::format("{} bytes from {:#x} (not done)", bytes, src);
                std::memmove(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), bytes);
                return std::format("copy {} bytes {:#x} -> {:#x}", bytes, src, dst);
            }
            case 0x3c: {  // WAIT_REG_MEM: wait until memory compares true
                if (n < 5) return "";
                std::string s = describe(op, b, n);
                if (!((b[0] >> 4) & 1)) return s + " (a register; not waited for)";
                u64 at = addr_of(b[1], b[2]);
                if (!in_heap(at, 4)) return s + " (not in the game's memory; not waited for)";
                auto* v = reinterpret_cast<const std::atomic<u32>*>(at);
                if (compare(b[0], v->load(std::memory_order_acquire) & b[4], b[3])) return s;
                {
                    std::lock_guard lock(g_stats_lock);
                    q.waiting = "waiting " + s;
                }
                auto start = std::chrono::steady_clock::now();
                while (!compare(b[0], v->load(std::memory_order_acquire) & b[4], b[3]))
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                {
                    std::lock_guard lock(g_stats_lock);
                    q.waiting.clear();
                }
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                return std::format("{} (waited {:.2f} ms)", s, ms);
            }
            default:
                return describe(op, b, n);
        }
    }
};

void work(Queue* q) {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(q->m);
            q->cv.wait(lock, [&] { return !q->jobs.empty(); });
            job = std::move(q->jobs.front());
        }
        u64 number;
        {
            std::lock_guard lock(g_stats_lock);
            number = ++q->submissions;
        }
        std::ofstream file;
        if (number <= LISTED) {
            auto dir = std::filesystem::path(M.out_dir) / "gpu";
            std::filesystem::create_directories(dir);
            file.open(dir / std::format("{}-{}.txt", q->name, number), std::ios::binary);
        }
        Walk w{*q, file.is_open() ? &file : nullptr};
        for (auto [at, dw] : job.constants) {
            w.line(0, std::format("constant buffer {:#x}, {} dwords (listed, not run):", at, dw));
            w.run(at, dw, 1, true);
        }
        for (auto [at, dw] : job.buffers) {
            w.line(0, std::format("{} {:#x}, {} dwords:", q->compute ? "ring" : "draw buffer", at, dw));
            w.run(at, dw, 1, false);
        }
        if (q->compute && q->rptr_addr && in_heap(q->rptr_addr, 4))
            reinterpret_cast<std::atomic<u32>*>(q->rptr_addr)->store(job.ring_end, std::memory_order_release);
        if (job.then) job.then();
        std::lock_guard lock(q->m);
        q->jobs.pop_front();
        q->cv.notify_all();
    }
}

Queue* queue(int id) {
    std::lock_guard lock(g_queues_lock);
    auto& q = g_queues[id];
    if (!q) {
        q = std::make_unique<Queue>();
        q->name = id ? std::format("compute-{}", id) : "graphics";
        q->compute = id != 0;
        q->worker = std::thread(work, q.get());
        q->worker.detach();
    }
    return q.get();
}

void push(Queue* q, Job job) {
    std::lock_guard lock(q->m);
    q->jobs.push_back(std::move(job));
    q->cv.notify_all();
}

}  // namespace

void gpu_submit(u32 count, const u64* dcbs, const u32* dcb_bytes, const u64* ccbs,
                const u32* ccb_bytes, std::function<void()> then) {
    Job job;
    for (u32 i = 0; i < count; i++) {
        if (dcbs && dcb_bytes && dcbs[i] && dcb_bytes[i]) job.buffers.push_back({dcbs[i], dcb_bytes[i] / 4});
        if (ccbs && ccb_bytes && ccbs[i] && ccb_bytes[i]) job.constants.push_back({ccbs[i], ccb_bytes[i] / 4});
    }
    job.then = std::move(then);
    push(queue(0), std::move(job));
}

void gpu_map_compute(int id, u64 ring, u32 dwords, u64 rptr_addr) {
    Queue* q = queue(id);
    std::lock_guard lock(q->m);
    q->ring = ring;
    q->ring_dwords = dwords;
    q->rptr_addr = rptr_addr;
    q->rptr = 0;
}

void gpu_ding_dong(int id, u32 offset) {
    Queue* q;
    {
        std::lock_guard lock(g_queues_lock);
        auto it = g_queues.find(id);
        if (it == g_queues.end()) return;
        q = it->second.get();
    }
    Job job;
    {
        std::lock_guard lock(q->m);
        if (!q->ring_dwords) return;
        offset %= q->ring_dwords + 1;
        if (offset < q->rptr) {  // wrapped: the ring's end, then its start
            job.buffers.push_back({q->ring + u64(q->rptr) * 4, q->ring_dwords - q->rptr});
            if (offset) job.buffers.push_back({q->ring, offset});
        } else if (offset > q->rptr) {
            job.buffers.push_back({q->ring + u64(q->rptr) * 4, offset - q->rptr});
        }
        q->rptr = offset;
        job.ring_end = offset;
    }
    push(q, std::move(job));
}

std::vector<std::string> gpu_report() {
    std::vector<std::string> s;
    std::lock_guard lock(g_queues_lock);
    if (g_queues.empty()) return s;
    std::lock_guard stats(g_stats_lock);
    s.insert(s.end(), {"## GPU\n", "| Queue | Submissions | Packets | Draws | Dispatches | Now |",
                       "|---|---:|---:|---:|---:|---|"});
    for (auto& [id, q] : g_queues)
        s.push_back(std::format("| {} | {} | {} | {} | {} | {} |", q->name, q->submissions, q->packets,
                                q->draws, q->dispatches, q->waiting.empty() ? "" : "`" + q->waiting + "`"));
    s.insert(s.end(), {"", "| Packet | Count |", "|---|---:|"});
    std::vector<std::pair<u64, std::string>> by;
    for (auto& [n, c] : g_ops) by.push_back({c, n});
    std::sort(by.rbegin(), by.rend());
    for (auto& [c, n] : by) s.push_back(std::format("| {} | {} |", n, c));
    s.push_back("");
    return s;
}

}  // namespace mensis
