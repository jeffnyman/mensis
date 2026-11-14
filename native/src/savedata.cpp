// SPDX-License-Identifier: GPL-3.0-or-later
//
// Save data (libSceSaveData). A save is a folder the game mounts, by name, and then reads and
// writes through its ordinary file calls at the mount point it is given (/savedata0/, ...).
// Here each save is a folder of the run's output folder,
// <out>/files/savedata/<user>/<save's name>/, kept from run to run, as a console keeps them;
// files.cpp sends the mount points there. What the game sets about a save (its title, subtitle
// and details, as the system's save list shows them) and its icon go into the save's sce_sys/
// folder as title.txt, subtitle.txt, detail.txt and icon0.png.
//
// sceSaveDataMount(const SceSaveDataMount *, SceSaveDataMountResult *), the older form. The
// request: the user (int, at 0), the title id (a pointer, at 8; none means this game's), the
// save's name (a pointer to 32 chars, at 16), the fingerprint (24), the size in 32 KiB blocks
// (32), and the mode (at 40): 1 read only, 2 read and write, 4 create (an error if it exists),
// 8 no corruption check, 16 copy the game's icon, 32 create if it doesn't exist. The result:
// the mount point (16 chars, at 0), the blocks needed (16), and the status (at 28): 1 if the
// save was just created.

#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>

#include "loader.hpp"

namespace fs = std::filesystem;

namespace mensis {
namespace {

template <typename T> T* ptr(u64 a) { return reinterpret_cast<T*>(a); }

constexpr i64 err(u32 e) { return i64(std::int32_t(e)); }
const i64 PARAMETER = err(0x809f0000), BUSY = err(0x809f0003), NOT_MOUNTED = err(0x809f0004),
          EXISTS = err(0x809f0007), NOT_FOUND = err(0x809f0008), MOUNT_FULL = err(0x809f000c);
constexpr u32 READ_ONLY = 1, CREATE = 4, CREATE_IF_MISSING = 32;
constexpr int SLOTS = 16;  // /savedata0/ to /savedata15/

std::mutex g_lock;
fs::path g_mounted[SLOTS];  // each mount point's folder; empty: free
bool g_read_only[SLOTS] = {};

std::string cstr(const char* p, size_t max) { return std::string(p, strnlen(p, max)); }

// "/savedata3" -> 3; -1 if it isn't a mount point
int slot_of(const std::string& name) {
    std::string n = name;
    if (!n.empty() && n[0] == '/') n = n.substr(1);
    if (n.rfind("savedata", 0) != 0 || n.size() == 8) return -1;
    int s = 0;
    for (size_t i = 8; i < n.size(); i++) {
        if (n[i] < '0' || n[i] > '9') return -1;
        s = s * 10 + (n[i] - '0');
    }
    return s < SLOTS ? s : -1;
}

// a save's name as a folder name: letters, digits, '-' and '_' as they are, anything else not
bool safe(const std::string& name) {
    if (name.empty()) return false;
    for (char c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
    return true;
}

i64 mount(const u64* a, std::string& note) {
    const unsigned char* m = ptr<const unsigned char>(a[0]);
    unsigned char* result = ptr<unsigned char>(a[1]);
    if (!m || !result) return PARAMETER;
    std::int32_t user;
    u64 title_at, dir_at, blocks;
    u32 mode;
    std::memcpy(&user, m, 4);
    std::memcpy(&title_at, m + 8, 8);
    std::memcpy(&dir_at, m + 16, 8);
    std::memcpy(&blocks, m + 32, 8);
    std::memcpy(&mode, m + 40, 4);
    std::string dir = dir_at ? cstr(ptr<const char>(dir_at), 32) : "";
    std::string title = title_at ? cstr(ptr<const char>(title_at), 10) : "";
    note = std::format("user {}, \"{}\"{}, {} blocks, mode {:#x}", user, dir, title.empty() ? "" : " of " + title, blocks, mode);
    if (!safe(dir) || (!title.empty() && !safe(title))) return PARAMETER;
    fs::path folder = fs::path(M.out_dir) / "files" / "savedata" / std::to_string(user) / (title.empty() ? dir : title + "-" + dir);
    std::lock_guard lock(g_lock);
    for (const auto& f : g_mounted)
        if (f == folder) {
            note += ": already mounted";
            return BUSY;
        }
    std::error_code ec;
    bool exists = fs::is_directory(folder, ec), created = false;
    if (mode & CREATE) {
        if (exists) {
            note += ": exists";
            return EXISTS;
        }
        created = true;
    } else if (mode & CREATE_IF_MISSING) {
        created = !exists;
    } else if (!exists) {
        note += ": not found";
        return NOT_FOUND;
    }
    int s = 0;
    while (s < SLOTS && !g_mounted[s].empty()) s++;
    if (s == SLOTS) return MOUNT_FULL;
    if (created) fs::create_directories(folder / "sce_sys", ec);
    g_mounted[s] = folder;
    g_read_only[s] = (mode & 3) == READ_ONLY;
    std::memset(result, 0, 64);
    std::string point = std::format("/savedata{}", s);
    std::memcpy(result, point.c_str(), point.size());
    u32 status = created ? 1 : 0;
    std::memcpy(result + 28, &status, 4);
    note += std::format(": {} at {}", created ? "created" : "found", point);
    return 0;
}

// The folder of a mount point (char[16]); -1 if it isn't mounted
int mounted(u64 point) {
    if (!point) return -1;
    int s = slot_of(cstr(ptr<const char>(point), 16));
    return s >= 0 && !g_mounted[s].empty() ? s : -1;
}

// sceSaveDataUmount(const SceSaveDataMountPoint *)
i64 umount(const u64* a, std::string& note) {
    std::lock_guard lock(g_lock);
    int s = mounted(a[0]);
    if (s < 0) return NOT_MOUNTED;
    note = std::format("/savedata{}", s);
    g_mounted[s].clear();
    return 0;
}

void write_file(const fs::path& p, const void* data, size_t n) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p, std::ios::binary).write(static_cast<const char*>(data), std::streamsize(n));
}

// sceSaveDataSetParam(mount point, type, const void *param, size): 0 all of SceSaveDataParam
// (title, 128 chars; subtitle, 128; detail, 1024; a value of the game's own, 32 bits), 1 the
// title, 2 the subtitle, 3 the detail, 4 the game's value
i64 set_param(const u64* a, std::string& note) {
    std::lock_guard lock(g_lock);
    int s = mounted(a[0]);
    if (s < 0) return NOT_MOUNTED;
    if (!a[2]) return PARAMETER;
    const char* p = ptr<const char>(a[2]);
    fs::path sys = g_mounted[s] / "sce_sys";
    auto text = [&](const char* name, const char* at, size_t max) {
        std::string t = cstr(at, max);
        write_file(sys / name, t.data(), t.size());
        note += (note.empty() ? "" : ", ") + std::format("{} \"{}\"", name, t);
    };
    auto value = [&](const char* at) {
        u32 v;
        std::memcpy(&v, at, 4);
        std::string t = std::to_string(v);
        write_file(sys / "user_param.txt", t.data(), t.size());
        note += (note.empty() ? "" : ", ") + std::format("user_param {}", v);
    };
    switch (u32(a[1])) {
        case 0: text("title.txt", p, 128), text("subtitle.txt", p + 128, 128), text("detail.txt", p + 256, 1024), value(p + 1280); break;
        case 1: text("title.txt", p, 128); break;
        case 2: text("subtitle.txt", p, 128); break;
        case 3: text("detail.txt", p, 1024); break;
        case 4: value(p); break;
        default: note = std::format("type {}", a[1]); return PARAMETER;
    }
    return 0;
}

// sceSaveDataSaveIcon(mount point, const SceSaveDataIcon *): the PNG's address, its buffer's
// size, and its own
i64 save_icon(const u64* a, std::string& note) {
    std::lock_guard lock(g_lock);
    int s = mounted(a[0]);
    if (s < 0) return NOT_MOUNTED;
    if (!a[1]) return PARAMETER;
    u64 buf = ptr<u64>(a[1])[0], size = ptr<u64>(a[1])[2];
    if (!buf || !size || size > (16u << 20)) return PARAMETER;
    write_file(g_mounted[s] / "sce_sys" / "icon0.png", ptr<const void>(buf), size);
    note = std::format("{} bytes", size);
    return 0;
}

i64 nothing(const u64*, std::string&) { return 0; }

}  // namespace

std::string savedata_folder(const std::string& point, bool& writable) {
    std::lock_guard lock(g_lock);
    int s = slot_of(point);
    if (s < 0 || g_mounted[s].empty()) return {};
    writable = !g_read_only[s];
    return g_mounted[s].string();
}

void add_savedata_standins(std::unordered_map<std::string, Standin>& t) {
    std::pair<const char*, Standin> list[] = {
        {"sceSaveDataInitialize", nothing},
        {"sceSaveDataTerminate", nothing},
        {"sceSaveDataMount", mount},
        {"sceSaveDataUmount", umount},
        {"sceSaveDataSetParam", set_param},
        {"sceSaveDataSaveIcon", save_icon},
    };
    for (auto [name, fn] : list) t.insert_or_assign(name, fn);
}

}  // namespace mensis
