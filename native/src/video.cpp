// SPDX-License-Identifier: GPL-3.0-or-later
//
// The display (libSceVideoOut) and the kernel's event queues, through which the display and the
// GPU tell the game what has happened.
//
// A flip makes one of the registered frame buffers the one on screen. The display here shows
// nothing yet, but keeps the timing: one flip at each vertical blank, 60 a second (or the rate
// sceVideoOutSetFlipRate asked for), each reported as an event to the queues that asked for flip
// events, and in sceVideoOutGetFlipStatus.

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>

#include "loader.hpp"

namespace mensis {
namespace {

constexpr i64 SCE_KERNEL_ERROR_ETIMEDOUT = i64(std::int32_t(0x8002003c));
constexpr i64 SCE_KERNEL_ERROR_EBADF = i64(std::int32_t(0x80020009));

// ---- event queues ----

// What the game registered, and whether it has happened since the game last looked.
struct Event {
    int filter;
    u64 ident;
    u64 udata;
    bool fired = false;
    i64 data = 0;
};

struct Equeue {
    std::string name;
    std::vector<Event> events;
};

std::mutex g_eq_lock;
std::condition_variable g_eq_cv;
std::map<u64, std::unique_ptr<Equeue>> g_equeues;  // by handle

Equeue* equeue(u64 h) {
    auto it = g_equeues.find(h);
    return it == g_equeues.end() ? nullptr : it->second.get();
}

void add_event(u64 h, int filter, u64 ident, u64 udata) {
    std::lock_guard lock(g_eq_lock);
    if (Equeue* q = equeue(h)) q->events.push_back({filter, ident, udata});
}

// sceKernelCreateEqueue(SceKernelEqueue *queue, const char *name)
i64 create_equeue(const u64* a, std::string& note) {
    u64 h = sync_alloc(16, 16);
    {
        std::lock_guard lock(g_eq_lock);
        auto q = std::make_unique<Equeue>();
        q->name = a[1] ? reinterpret_cast<const char*>(a[1]) : "";
        note = std::format("'{}'", q->name);
        g_equeues[h] = std::move(q);
    }
    *reinterpret_cast<u64*>(a[0]) = h;
    return 0;
}

i64 delete_equeue(const u64* a, std::string&) {
    std::lock_guard lock(g_eq_lock);
    return g_equeues.erase(a[0]) ? 0 : SCE_KERNEL_ERROR_EBADF;
}

// sceKernelWaitEqueue(queue, SceKernelEvent *events, int max, int *count, u32 *timeout in
// microseconds, or null to wait for ever). An event is FreeBSD's kevent, 32 bytes: ident, a
// 16-bit filter, 16-bit flags, 32-bit fflags, data, udata. Each is reported once (EV_CLEAR).
i64 wait_equeue(const u64* a, std::string& note) {
    struct Kevent {
        u64 ident;
        std::int16_t filter;
        std::uint16_t flags;
        u32 fflags;
        i64 data;
        u64 udata;
    };
    static_assert(sizeof(Kevent) == 32);
    auto* out = reinterpret_cast<Kevent*>(a[1]);
    int max = int(a[2]);
    std::unique_lock lock(g_eq_lock);
    auto ready = [&] {
        Equeue* q = equeue(a[0]);
        if (!q) return true;
        for (auto& e : q->events)
            if (e.fired) return true;
        return false;
    };
    if (a[4]) {
        auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(*reinterpret_cast<const u32*>(a[4]));
        if (!g_eq_cv.wait_until(lock, until, ready)) {
            if (a[3]) *reinterpret_cast<int*>(a[3]) = 0;
            return SCE_KERNEL_ERROR_ETIMEDOUT;
        }
    } else {
        g_eq_cv.wait(lock, ready);
    }
    Equeue* q = equeue(a[0]);
    if (!q) return SCE_KERNEL_ERROR_EBADF;
    int n = 0;
    for (auto& e : q->events) {
        if (!e.fired || n >= max) continue;
        out[n++] = {e.ident, std::int16_t(e.filter), 0x20 /* EV_CLEAR */, 0, e.data, e.udata};
        e.fired = false;
        note += std::format("{}{}/{:#x}", note.empty() ? "" : ", ", e.filter, e.ident);
    }
    if (a[3]) *reinterpret_cast<int*>(a[3]) = n;
    return 0;
}

// sceGnmAddEqEvent(queue, event id, udata): the GPU's events (0x40: end of pipe)
i64 add_eq_event(const u64* a, std::string&) {
    add_event(a[0], EVENT_GRAPHICS, a[1], a[2]);
    return 0;
}

i64 delete_eq_event(const u64* a, std::string&) {
    std::lock_guard lock(g_eq_lock);
    if (Equeue* q = equeue(a[0]))
        std::erase_if(q->events, [&](const Event& e) { return e.filter == EVENT_GRAPHICS && e.ident == a[1]; });
    return 0;
}

// sceGnmGetEqEventType(const SceKernelEvent*): the event id it was registered with
i64 eq_event_type(const u64* a, std::string&) { return i64(*reinterpret_cast<const u64*>(a[0])); }

// ---- the display ----

struct Flip {
    int index;  // the buffer to show; -1 shows none
    i64 arg;  // the game's own value, handed back in the flip's event and status
};

struct Display {
    std::mutex m;
    int rate = 0;  // sceVideoOutSetFlipRate: 0 = 60 a second, 1 = 30, 2 = 20
    std::vector<u64> buffers;
    // The buffers' attribute (sceVideoOutRegisterBuffers'): all share one
    u32 format = 0, tiling = 0, width = 0, height = 0, pitch = 0;
    std::deque<Flip> pending;  // asked for, waiting for a vertical blank
    int on_gpu = 0;  // flips that wait for their command buffers first
    u64 flips = 0;
    i64 last_arg = 0;
    int current = -1;
    u64 last_flip_time = 0;
    bool started = false;
} g_display;

u64 now_us() {
    return u64(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - M.t0).count());
}

// One flip at each vertical blank.
void scan_out() {
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        int rate;
        {
            std::lock_guard lock(g_display.m);
            rate = g_display.rate;
        }
        next += std::chrono::microseconds(1000000 * (rate + 1) / 60);
        std::this_thread::sleep_until(next);
        Flip f;
        u64 count;
        {
            std::lock_guard lock(g_display.m);
            if (g_display.pending.empty()) continue;
            f = g_display.pending.front();
            g_display.pending.pop_front();
            count = ++g_display.flips;
            g_display.last_arg = f.arg;
            g_display.current = f.index;
            g_display.last_flip_time = now_us();
        }
        trigger_event(EVENT_VIDEO_OUT, VIDEO_OUT_FLIP, f.arg);
        if (M.max_flips && count >= M.max_flips) finish(std::format("{} flips shown", count));
    }
}

void request_flip(int index, i64 arg) {
    std::lock_guard lock(g_display.m);
    g_display.pending.push_back({index, arg});
}

// sceVideoOutOpen(user, bus type, index, param): a handle to the display; there is one
i64 video_open(const u64* a, std::string& note) {
    {
        std::lock_guard lock(g_display.m);
        if (!g_display.started) {
            g_display.started = true;
            std::thread(scan_out).detach();
        }
    }
    note = std::format("display for user {:#x}, bus {}", a[0], a[1]);
    return 1;
}

// sceVideoOutSetFlipRate(handle, rate)
i64 video_flip_rate(const u64* a, std::string&) {
    std::lock_guard lock(g_display.m);
    g_display.rate = int(std::min<u64>(a[1], 2));
    return 0;
}

// sceVideoOutAddFlipEvent(queue, handle, udata)
i64 video_add_flip_event(const u64* a, std::string&) {
    add_event(a[0], EVENT_VIDEO_OUT, VIDEO_OUT_FLIP, a[2]);
    return 0;
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
    std::lock_guard lock(g_display.m);
    for (u64 i = 0; i < a[3] && i < 16; i++) {
        u64 at = reinterpret_cast<const u64*>(a[2])[i];
        if (g_display.buffers.size() <= a[1] + i) g_display.buffers.resize(a[1] + i + 1);
        g_display.buffers[a[1] + i] = at;
        where += std::format(" {:#x}", at);
    }
    if (a[4]) {  // the attribute: pixel format, tiling mode, aspect ratio, width, height, pitch
        const u32* attr = reinterpret_cast<const u32*>(a[4]);
        g_display.format = attr[0];
        g_display.tiling = attr[1];
        g_display.width = attr[3];
        g_display.height = attr[4];
        g_display.pitch = attr[5];
    }
    note = std::format("{} buffers from index {}:{}", a[3], a[1], where);
    return 0;
}

// sceVideoOutSubmitFlip(handle, buffer index, flip mode, flip arg)
i64 video_submit_flip(const u64* a, std::string&) {
    request_flip(int(a[1]), i64(a[3]));
    return 0;
}

// sceGnmSubmitAndFlipCommandBuffers(count, dcb[], dcb_bytes[], ccb[], ccb_bytes[], video
// handle, buffer index, flip mode, flip arg): the buffers, then the flip once the GPU is done
// with them
i64 submit_and_flip(const u64* a, std::string& note) {
    const u64* more = reinterpret_cast<const u64*>(current_frame()->stack_args);
    int index = int(more[0]);
    i64 arg = i64(more[2]);
    {
        std::lock_guard lock(g_display.m);
        g_display.on_gpu++;
    }
    gpu_submit(u32(a[0]), reinterpret_cast<const u64*>(a[1]), reinterpret_cast<const u32*>(a[2]),
               reinterpret_cast<const u64*>(a[3]), reinterpret_cast<const u32*>(a[4]), [index, arg] {
                   {
                       std::lock_guard lock(g_display.m);
                       g_display.on_gpu--;
                   }
                   request_flip(index, arg);
               });
    note = std::format("{} buffer(s), then buffer {} (arg {:#x})", u32(a[0]), index, arg);
    return 0;
}

// sceVideoOutGetFlipStatus(handle, SceVideoOutFlipStatus*): flips so far, when the last was,
// its arg, how many are waiting, and the buffer on screen (0x40 bytes)
i64 video_flip_status(const u64* a, std::string&) {
    struct Status {
        u64 count, process_time, tsc;
        i64 flip_arg;
        u64 submit_tsc, reserved0;
        std::int32_t gc_queue_num, flip_pending_num, current_buffer;
        u32 reserved1;
    };
    static_assert(sizeof(Status) == 0x40);
    std::lock_guard lock(g_display.m);
    Status s{g_display.flips, g_display.last_flip_time, g_display.last_flip_time, g_display.last_arg, 0, 0,
             g_display.on_gpu, std::int32_t(g_display.pending.size()) + g_display.on_gpu, g_display.current, 0};
    std::memcpy(reinterpret_cast<void*>(a[1]), &s, sizeof s);
    return 0;
}

i64 nothing(const u64*, std::string&) { return 0; }

}  // namespace

// The event, to every queue that registered it.
void trigger_event(int filter, u64 ident, i64 data) {
    std::lock_guard lock(g_eq_lock);
    for (auto& [h, q] : g_equeues)
        for (auto& e : q->events)
            if (e.filter == filter && e.ident == ident) {
                e.fired = true;
                e.data = data;
            }
    g_eq_cv.notify_all();
}

// The buffer on screen, as a 32-bit BMP, with its bytes as they are in memory: the PS4's
// A8R8G8B8 is B, G, R, A in memory, as a BMP's pixels are. A tiled buffer (tiling mode 0) is
// one the console's GPU would arrange in tiles; the loader's GPU (draw.cpp) keeps such buffers
// in rows, so it is written as it is. Returns how many of its pixels aren't 0. Under
// g_display.m.
u64 save_frame(const std::string& path) {
    const Display& d = g_display;
    u64 at = d.buffers[size_t(d.current)];
    struct {
        char magic[2] = {'B', 'M'};
        u32 file_size, reserved = 0, offset = 54;
        u32 header = 40;
        std::int32_t width, height;  // negative: the rows top down
        std::uint16_t planes = 1, bits = 32;
        u32 compression = 0, image_size, x_ppm = 2835, y_ppm = 2835, colours = 0, important = 0;
    } h;
    static_assert(sizeof(h) == 56);  // 54, and the padding after the magic
    u64 row = u64(d.width) * 4;
    h.image_size = u32(row * d.height);
    h.file_size = 54 + h.image_size;
    h.width = std::int32_t(d.width);
    h.height = -std::int32_t(d.height);
    std::ofstream f(path, std::ios::binary);
    f.write(h.magic, 2);
    f.write(reinterpret_cast<const char*>(&h.file_size), 52);
    u64 lit = 0;
    for (u32 y = 0; y < d.height; y++) {
        const u32* p = reinterpret_cast<const u32*>(at + u64(y) * d.pitch * 4);
        for (u32 x = 0; x < d.width; x++) lit += p[x] != 0;
        f.write(reinterpret_cast<const char*>(p), std::streamsize(row));
    }
    return lit;
}

// For the window: a copy of the buffer on screen (empty before the first flip) and its size;
// returns the flips so far.
u64 video_picture(std::vector<u32>& pixels, u32& width, u32& height) {
    std::lock_guard lock(g_display.m);
    const Display& d = g_display;
    pixels.clear();
    width = height = 0;
    if (d.current >= 0 && size_t(d.current) < d.buffers.size() && d.width && d.height && d.pitch >= d.width) {
        width = d.width;
        height = d.height;
        pixels.resize(size_t(width) * height);
        for (u32 y = 0; y < height; y++)
            std::memcpy(pixels.data() + size_t(y) * width,
                        reinterpret_cast<const void*>(d.buffers[size_t(d.current)] + u64(y) * d.pitch * 4),
                        size_t(width) * 4);
    }
    return d.flips;
}

std::vector<std::string> video_report() {
    std::lock_guard lock(g_display.m);
    if (!g_display.started) return {};
    std::string s = std::format("Display: {} flips shown, {} waiting, {} waiting for the GPU; buffer {} on screen",
                                g_display.flips, g_display.pending.size(), g_display.on_gpu, g_display.current);
    const Display& d = g_display;
    if (d.current >= 0 && size_t(d.current) < d.buffers.size() && d.width && d.height && d.pitch >= d.width) {
        std::filesystem::create_directories(M.out_dir);
        u64 lit = save_frame(M.out_dir + "/frame.bmp");
        s += std::format(" (saved to frame.bmp: {}x{}, {}; {} of its {} pixels not 0)", d.width, d.height,
                         d.tiling == 0 ? "tiled: kept in rows by the loader's GPU" : "linear", lit,
                         u64(d.width) * d.height);
    }
    return {s + ".\n"};
}

void add_video_standins(std::unordered_map<std::string, Standin>& t) {
    std::pair<const char*, Standin> list[] = {
        {"sceKernelCreateEqueue", create_equeue},
        {"sceKernelDeleteEqueue", delete_equeue},
        {"sceKernelWaitEqueue", wait_equeue},
        {"sceGnmAddEqEvent", add_eq_event},
        {"sceGnmDeleteEqEvent", delete_eq_event},
        {"sceGnmGetEqEventType", eq_event_type},
        {"sceVideoOutOpen", video_open},
        {"sceVideoOutClose", nothing},
        {"sceVideoOutSetFlipRate", video_flip_rate},
        {"sceVideoOutAddFlipEvent", video_add_flip_event},
        {"sceVideoOutSetBufferAttribute", video_buffer_attribute},
        {"sceVideoOutRegisterBuffers", video_register_buffers},
        {"sceVideoOutSubmitFlip", video_submit_flip},
        {"sceVideoOutGetFlipStatus", video_flip_status},
        {"sceGnmSubmitAndFlipCommandBuffers", submit_and_flip},
    };
    for (auto [name, fn] : list) t.insert_or_assign(name, fn);
}

}  // namespace mensis
