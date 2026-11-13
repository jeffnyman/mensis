// SPDX-License-Identifier: GPL-3.0-or-later
//
// The player: one user, logged in from the start (libSceUserService), and their controller
// (libScePad). What the controller reports comes from the platform (on Windows, the keys held
// in the window and an XInput gamepad, platform_windows.cpp), and from --press, which holds
// buttons down from given frames, so that a run can go past a menu the same way each time.
//
// ScePadData, as scePadReadState fills it (120 bytes): the buttons (32 bits), the sticks (left
// x and y, right x and y: a byte each, 128 the middle, y growing downwards), the triggers (L2,
// R2), the controller's orientation (a quaternion, at 12), its acceleration and angular
// velocity (3 floats each), the touch pad's data, then whether it is connected (at 76), a
// timestamp in microseconds (at 80), and how many times it has been connected (at 104).

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>

#include "loader.hpp"

namespace mensis {
namespace {

template <typename T> T* ptr(u64 a) { return reinterpret_cast<T*>(a); }

constexpr std::int32_t USER = 1;  // the user's id
constexpr i64 HANDLE = 1;  // the controller's
constexpr u64 HOLD = 6;  // flips a --press holds its buttons down for (0.2 s at 30 a second)
constexpr i64 PAD_INVALID_ARG = i64(std::int32_t(0x80920001)), PAD_INVALID_HANDLE = i64(std::int32_t(0x80920003));

struct Button {
    const char* name;
    u32 bit;
};
constexpr Button BUTTONS[] = {
    {"l3", 0x2},        {"r3", 0x4},      {"options", 0x8},  {"up", 0x10},       {"right", 0x20},  {"down", 0x40},
    {"left", 0x80},     {"l2", 0x100},    {"r2", 0x200},     {"l1", 0x400},      {"r1", 0x800},    {"triangle", 0x1000},
    {"circle", 0x2000}, {"cross", 0x4000}, {"square", 0x8000}, {"touchpad", 0x100000},
};

std::string names(u32 bits) {
    std::string s;
    for (const Button& b : BUTTONS)
        if (bits & b.bit) s += (s.empty() ? "" : "+") + std::string(b.name);
    return s;
}

std::mutex g_lock;
u64 g_reads = 0;
std::vector<std::string> g_presses;  // the first few times buttons went down, for the summary
u32 g_held = 0;

PadInput current() {
    PadInput in = platform::pad_input();
    u64 flip = video_flips();
    for (auto [at, bits] : M.presses)
        if (flip >= at && flip < at + HOLD) in.buttons |= bits;
    if (in.buttons & 0x100) in.l2 = 255;  // L2 and R2 pressed as buttons pull their triggers too
    if (in.buttons & 0x200) in.r2 = 255;
    return in;
}

// sceUserServiceGetInitialUser(int *user)
i64 initial_user(const u64* a, std::string&) {
    *ptr<std::int32_t>(a[0]) = USER;
    return 0;
}

// sceUserServiceGetLoginUserIdList(int list[4]): the user, then none (-1)
i64 login_users(const u64* a, std::string&) {
    auto* list = ptr<std::int32_t>(a[0]);
    list[0] = USER;
    list[1] = list[2] = list[3] = -1;
    return 0;
}

// sceUserServiceGetUserName(user, char *name, size)
i64 user_name(const u64* a, std::string&) {
    const char name[] = "Player";
    if (!a[2]) return 0;
    size_t n = std::min<size_t>(sizeof name - 1, a[2] - 1);
    std::memcpy(ptr<char>(a[1]), name, n);
    ptr<char>(a[1])[n] = 0;
    return 0;
}

// scePadOpen(user, type, index, param): the user's controller; any other user has none
i64 pad_open(const u64* a, std::string& note) {
    if (std::int32_t(a[0]) != USER) {
        note = "not the user";
        return PAD_INVALID_ARG;
    }
    return HANDLE;
}

// scePadReadState(handle, ScePadData *)
i64 read_state(const u64* a, std::string& note) {
    if (i64(std::int32_t(a[0])) != HANDLE) return PAD_INVALID_HANDLE;
    PadInput in = current();
    unsigned char d[120] = {};
    std::memcpy(d, &in.buttons, 4);
    d[4] = in.lx, d[5] = in.ly, d[6] = in.rx, d[7] = in.ry, d[8] = in.l2, d[9] = in.r2;
    float one = 1;
    std::memcpy(d + 24, &one, 4);  // the orientation: (0, 0, 0, 1), level
    d[76] = 1;  // connected
    u64 us = u64(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - M.t0).count());
    std::memcpy(d + 80, &us, 8);
    d[104] = 1;
    std::memcpy(ptr<void>(a[1]), d, sizeof d);
    if (in.buttons) note = names(in.buttons);
    std::lock_guard lock(g_lock);
    g_reads++;
    u32 down = in.buttons & ~g_held;
    if (down && g_presses.size() < 20) g_presses.push_back(std::format("{} (frame {})", names(down), video_flips()));
    g_held = in.buttons;
    return 0;
}

// scePadGetControllerInformation(handle, ScePadControllerInformation *): a DualShock 4, plugged
// in: the touch pad's density and size, the sticks' dead zones, the connection (local), how many
// times it has been connected, connected, and its class (a standard controller)
i64 controller_info(const u64* a, std::string&) {
    if (i64(std::int32_t(a[0])) != HANDLE) return PAD_INVALID_HANDLE;
    unsigned char d[28] = {};
    float density = 44.86f;
    std::uint16_t size[2] = {1920, 943};
    std::memcpy(d, &density, 4);
    std::memcpy(d + 4, size, 4);
    d[8] = d[9] = 2;
    d[11] = 1;
    d[12] = 1;
    std::memcpy(ptr<void>(a[1]), d, sizeof d);
    return 0;
}

i64 nothing(const u64*, std::string&) { return 0; }

}  // namespace

u32 pad_button(const std::string& name) {
    for (const Button& b : BUTTONS)
        if (name == b.name) return b.bit;
    return 0;
}

std::vector<std::string> pad_report() {
    std::lock_guard lock(g_lock);
    if (!g_reads) return {};
    std::string s = std::format("Controller: read {} times (scePadReadState); ", g_reads);
    if (g_presses.empty()) return {s + "no buttons pressed.\n"};
    s += "buttons pressed: ";
    for (size_t i = 0; i < g_presses.size(); i++) s += (i ? ", " : "") + g_presses[i];
    return {s + ".\n"};
}

void add_pad_standins(std::unordered_map<std::string, Standin>& t) {
    std::pair<const char*, Standin> list[] = {
        {"sceUserServiceInitialize", nothing},
        {"sceUserServiceTerminate", nothing},
        {"sceUserServiceGetInitialUser", initial_user},
        {"sceUserServiceGetLoginUserIdList", login_users},
        {"sceUserServiceGetUserName", user_name},
        {"scePadInit", nothing},
        {"scePadOpen", pad_open},
        {"scePadClose", nothing},
        {"scePadReadState", read_state},
        {"scePadGetControllerInformation", controller_info},
        {"scePadSetVibration", nothing},
        {"scePadResetOrientation", nothing},
        {"scePadSetAngularVelocityDeadbandState", nothing},
        {"scePadSetTiltCorrectionState", nothing},
    };
    for (auto [name, fn] : list) t.insert_or_assign(name, fn);
}

}  // namespace mensis
