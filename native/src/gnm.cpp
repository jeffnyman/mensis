// SPDX-License-Identifier: GPL-3.0-or-later
//
// The graphics driver (libSceGnmDriver): what the game asks of the GPU's driver.
//
// There is no GPU behind these yet. They answer the questions a driver answers at set-up (where
// the tessellation ring goes, a queue's id), the way the PS4's driver does, so the game's own
// set-up can complete; what it later submits for the GPU to run is recorded, not run.

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>

#include "loader.hpp"

namespace mensis {
namespace {

// The driver's errors
constexpr i64 GNM_ERROR_VALIDATION_NOT_ENABLED = i64(std::int32_t(0x80d13fff));
constexpr i64 GNM_ERROR_COMPUTEQUEUE_INVALID_PIPE_ID = i64(std::int32_t(0x80d17000));
constexpr i64 GNM_ERROR_COMPUTEQUEUE_INVALID_QUEUE_ID = i64(std::int32_t(0x80d17001));

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
// game treats 0 as failure.
std::atomic<int> g_next_queue{1};

i64 map_compute_queue(const u64* a, std::string& note) {
    if (a[0] > 6) return GNM_ERROR_COMPUTEQUEUE_INVALID_PIPE_ID;
    if (a[1] > 7) return GNM_ERROR_COMPUTEQUEUE_INVALID_QUEUE_ID;
    int id = g_next_queue++;
    note = std::format("queue {}: pipe {}, queue {}, ring {:#x} ({:#x} dwords)", id, a[0], a[1],
                       a[2], u32(a[3]));
    return id;
}

i64 nothing(const u64*, std::string&) { return 0; }

// ---- the video output (libSceVideoOut) ----

// sceVideoOutOpen(user, bus type, index, param): a handle to the display; there is one
i64 video_open(const u64* a, std::string& note) {
    note = std::format("display for user {:#x}, bus {}", a[0], a[1]);
    return 1;
}

// sceVideoOutSetBufferAttribute(attr*, pixel format, tiling mode, aspect ratio, width, height,
// pitch in pixels): fills the attribute (eight 32-bit fields; the last is an option, 0)
i64 video_buffer_attribute(const u64* a, std::string& note) {
    u32 pitch = u32(*reinterpret_cast<const u64*>(current_frame()->stack_args));
    u32 fields[8] = {u32(a[1]), u32(a[2]), u32(a[3]), u32(a[4]), u32(a[5]), pitch, 0, 0};
    std::memcpy(reinterpret_cast<void*>(a[0]), fields, sizeof fields);
    note = std::format("{}x{}, pitch {}, pixel format {:#x}, tiling {}", u32(a[4]), u32(a[5]),
                       pitch, u32(a[1]), u32(a[2]));
    return 0;
}

// sceVideoOutRegisterBuffers(handle, first index, void *const *addresses, count, attr*): the
// frame buffers the game will flip between; returns the group's index
i64 video_register_buffers(const u64* a, std::string& note) {
    std::string where;
    for (u64 i = 0; i < a[3] && i < 16; i++)
        where += std::format(" {:#x}", reinterpret_cast<const u64*>(a[2])[i]);
    note = std::format("{} buffers from index {}:{}", a[3], a[1], where);
    return 0;
}

// sceKernelCreateEqueue(SceKernelEqueue *queue, const char *name): an event queue, through
// which the display reports flips. Nothing is reported yet; the handle is a placeholder.
i64 create_equeue(const u64* a, std::string& note) {
    *reinterpret_cast<u64*>(a[0]) = sync_alloc(16, 16);
    note = std::format("'{}'", a[1] ? reinterpret_cast<const char*>(a[1]) : "");
    return 0;
}

}  // namespace

// sceGnmSubmitCommandBuffers(count, void *dcb[], u32 dcb_bytes[], void *ccb[], u32 ccb_bytes[])
// and sceGnmSubmitAndFlipCommandBuffers (the same, then flip arguments): the draw command
// buffers (dcb) and constant command buffers (ccb) for the GPU. Each is saved whole to
// <out>/gpu/, for reading; returns a description.
std::string save_submission(const u64* a) {
    namespace fs = std::filesystem;
    fs::path dir = fs::path(M.out_dir) / "gpu";
    fs::create_directories(dir);
    u64 count = u32(a[0]);
    std::string parts;
    for (u64 i = 0; i < count && i < 64; i++) {
        for (int k = 0; k < 2; k++) {  // dcb, then ccb
            u64 addrs = a[k ? 3 : 1], sizes = a[k ? 4 : 2];
            if (!addrs || !sizes) continue;
            u64 at = reinterpret_cast<const u64*>(addrs)[i];
            u32 bytes = reinterpret_cast<const u32*>(sizes)[i];
            if (!at || !bytes) continue;
            std::string name = std::format("submit-0-{}-{}.bin", k ? "ccb" : "dcb", i);
            std::ofstream(dir / name, std::ios::binary)
                .write(reinterpret_cast<const char*>(at), bytes);
            parts += std::format("{}{} {:#x} bytes at {:#x}", parts.empty() ? "" : "; ",
                                 k ? "ccb" : "dcb", bytes, at);
        }
    }
    return std::format("{} command buffer(s): {} (saved to gpu/)", count, parts);
}

void add_gnm_standins(std::unordered_map<std::string, Standin>& t) {
    std::pair<const char*, Standin> list[] = {
        {"sceGnmGetTheTessellationFactorRingBufferBaseAddress", tess_ring_address},
        {"sceGnmValidateCommandBuffers", validate},
        {"sceGnmMapComputeQueue", map_compute_queue},
        {"sceGnmMapComputeQueueWithPriority", map_compute_queue},
        {"sceGnmUnmapComputeQueue", nothing},
        {"sceVideoOutOpen", video_open},
        {"sceVideoOutSetFlipRate", nothing},
        {"sceVideoOutAddFlipEvent", nothing},
        {"sceVideoOutSetBufferAttribute", video_buffer_attribute},
        {"sceVideoOutRegisterBuffers", video_register_buffers},
        {"sceKernelCreateEqueue", create_equeue},
    };
    for (auto [name, fn] : list) t.insert_or_assign(name, fn);
}

}  // namespace mensis
