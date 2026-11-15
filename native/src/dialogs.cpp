// SPDX-License-Identifier: GPL-3.0-or-later
//
// The system's dialogs that stand in front of a game: so far the on-screen keyboard
// (libSceImeDialog), which a game opens to have the player type a text (the character's name,
// say). There is no keyboard here to show: the dialog is done as soon as it is opened, the text
// the player would have typed written into the game's buffer, "Hunter" or whatever --name says,
// and its end reported as OK.
//
// SceImeDialogParam, as sceImeDialogInit gets it: the user (int, at 0), the keyboard's type (4),
// the languages (8), the enter key's label (16), the input method (20), a text filter (a
// function, 24), options (32), the text's longest (in characters, 36), the buffer for it (UTF-16,
// at 40), where on screen (48, 52, 56, 60), a placeholder (64) and a title (72), both UTF-16.

#include <cstring>
#include <format>
#include <mutex>

#include "loader.hpp"

namespace mensis {
namespace {

template <typename T> T* ptr(u64 a) { return reinterpret_cast<T*>(a); }

constexpr i64 IME_DIALOG_BUSY = i64(std::int32_t(0x80bc0001)), IME_DIALOG_NOT_RUNNING = i64(std::int32_t(0x80bc0004));
constexpr i64 STATUS_NONE = 0, STATUS_FINISHED = 2;

std::mutex g_lock;
bool g_open = false;
u64 g_opened = 0;

std::string utf16(u64 at, size_t max) {  // a UTF-16 text, for the log (its ASCII; else '?')
    std::string s;
    if (!at) return s;
    for (size_t i = 0; i < max; i++) {
        std::uint16_t c = ptr<std::uint16_t>(at)[i];
        if (!c) break;
        s += c < 128 ? char(c) : '?';
    }
    return s;
}

// sceImeDialogInit(const SceImeDialogParam *, const SceImeDialogParamExtended *)
i64 init(const u64* a, std::string& note) {
    std::lock_guard lock(g_lock);
    if (g_open) return IME_DIALOG_BUSY;
    const unsigned char* p = ptr<unsigned char>(a[0]);
    u32 max, type;
    u64 buffer, title;
    std::memcpy(&type, p + 4, 4);
    std::memcpy(&max, p + 36, 4);
    std::memcpy(&buffer, p + 40, 8);
    std::memcpy(&title, p + 72, 8);
    std::string text = M.name;
    if (buffer && max) {
        size_t n = std::min<size_t>(text.size(), max);
        auto* out = ptr<std::uint16_t>(buffer);
        for (size_t i = 0; i < n; i++) out[i] = static_cast<unsigned char>(text[i]);
        out[n] = 0;
    }
    g_open = true;
    g_opened++;
    note = std::format("type {}, at most {} characters, title \"{}\": typed \"{}\"", type, max, utf16(title, 64), text.substr(0, max));
    return 0;
}

// sceImeDialogGetStatus(): the dialog done at once
i64 status(const u64*, std::string&) {
    std::lock_guard lock(g_lock);
    return g_open ? STATUS_FINISHED : STATUS_NONE;
}

// sceImeDialogGetResult(SceImeDialogResult *): its end, OK (0)
i64 result(const u64* a, std::string&) {
    std::lock_guard lock(g_lock);
    if (!g_open) return IME_DIALOG_NOT_RUNNING;
    if (a[0]) std::memset(ptr<void>(a[0]), 0, 16);
    return 0;
}

// sceImeDialogTerm(), sceImeDialogAbort()
i64 term(const u64*, std::string&) {
    std::lock_guard lock(g_lock);
    g_open = false;
    return 0;
}

}  // namespace

void add_dialog_standins(std::unordered_map<std::string, Standin>& t) {
    std::pair<const char*, Standin> list[] = {
        {"sceImeDialogInit", init},
        {"sceImeDialogGetStatus", status},
        {"sceImeDialogGetResult", result},
        {"sceImeDialogTerm", term},
        {"sceImeDialogAbort", term},
    };
    for (auto [name, fn] : list) t.insert_or_assign(name, fn);
}

}  // namespace mensis
