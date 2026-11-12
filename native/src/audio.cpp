// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audio output (libSceAudioOut). Nothing is played yet: a port takes the game's samples and
// discards them, but at the pace a real port would. On the PS4, sceAudioOutOutput blocks until
// the port is ready for the next buffer, and the sound library's audio thread relies on that to
// keep time; returning at once would have it spin.

#include <chrono>
#include <format>
#include <map>
#include <mutex>
#include <thread>

#include "loader.hpp"

namespace mensis {
namespace {

struct Port {
    u32 samples = 256;  // per buffer
    u32 rate = 48000;  // samples a second
    std::chrono::steady_clock::time_point next{};
};

std::mutex g_ports_lock;
std::map<int, Port> g_ports;
int g_next_port = 1;

// sceAudioOutOpen(user, type, index, samples per buffer, rate, format): a port handle
i64 open_port(const u64* a, std::string& note) {
    std::lock_guard lock(g_ports_lock);
    int h = g_next_port++;
    Port p;
    p.samples = u32(a[3]) ? u32(a[3]) : 256;
    p.rate = u32(a[4]) ? u32(a[4]) : 48000;
    g_ports[h] = p;
    note = std::format("port {}: type {}, {} samples at {} Hz, format {:#x}", h, a[1], p.samples,
                       p.rate, u32(a[5]));
    return h;
}

// sceAudioOutOutput(handle, samples): wait until this port's next buffer is due
i64 output(const u64* a, std::string&) {
    std::chrono::steady_clock::time_point due;
    {
        std::lock_guard lock(g_ports_lock);
        auto it = g_ports.find(int(a[0]));
        if (it == g_ports.end()) return 0;
        Port& p = it->second;
        auto now = std::chrono::steady_clock::now();
        auto period = std::chrono::microseconds(u64(p.samples) * 1000000 / p.rate);
        if (p.next < now) p.next = now;  // first buffer, or the game fell behind
        p.next += period;
        due = p.next - period;
    }
    std::this_thread::sleep_until(due);
    return 0;
}

i64 nothing(const u64*, std::string&) { return 0; }

}  // namespace

void add_audio_standins(std::unordered_map<std::string, Standin>& t) {
    std::pair<const char*, Standin> list[] = {
        {"sceAudioOutInit", nothing},
        {"sceAudioOutOpen", open_port},
        {"sceAudioOutClose", nothing},
        {"sceAudioOutOutput", output},
        {"sceAudioOutSetVolume", nothing},
    };
    for (auto [name, fn] : list) t.insert_or_assign(name, fn);
}

}  // namespace mensis
