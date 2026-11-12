// SPDX-License-Identifier: GPL-3.0-or-later
//
// The stand-ins: the least of the PS4's system the start-up needs. The first ones match
// tools/loader.py's, so over the stretch that loader reaches both runs return the same values;
// the rest take the start-up further. The game's memory is mapped at the addresses it expects,
// so a pointer argument is used directly.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <new>
#include <string>

#include "loader.hpp"

namespace mensis {
namespace {

template <typename T> T* ptr(u64 a) { return reinterpret_cast<T*>(a); }

i64 sign(int r) { return r == 0 ? 0 : (r < 0 ? -1 : 1); }

i64 s_malloc(const u64* a, std::string&) { return i64(alloc(a[0])); }

i64 s_calloc(const u64* a, std::string&) {
    u64 n = a[0] * a[1];
    u64 p = alloc(n);
    std::memset(ptr<void>(p), 0, n);
    return i64(p);
}

i64 s_realloc(const u64* a, std::string&) {
    u64 p = alloc(a[1]);
    if (a[0]) {
        auto it = M.allocs.find(a[0]);
        u64 old = it == M.allocs.end() ? 0 : it->second;
        std::memmove(ptr<void>(p), ptr<void>(a[0]), std::min(old, a[1]));
    }
    return i64(p);
}

// memalign(align, size), aligned_alloc(align, size)
i64 s_memalign(const u64* a, std::string&) { return i64(alloc(a[1], a[0])); }

// posix_memalign(void **out, align, size)
i64 s_posix_memalign(const u64* a, std::string&) {
    *ptr<u64>(a[0]) = alloc(a[2], a[1]);
    return 0;
}

// memcpy and memmove alike: the Python stand-in reads then writes, which is memmove
i64 s_memcpy(const u64* a, std::string&) {
    std::memmove(ptr<void>(a[0]), ptr<void>(a[1]), a[2]);
    return i64(a[0]);
}

i64 s_memset(const u64* a, std::string&) {
    std::memset(ptr<void>(a[0]), int(a[1] & 0xff), a[2]);
    return i64(a[0]);
}

i64 s_memcmp(const u64* a, std::string&) {
    return sign(std::memcmp(ptr<void>(a[0]), ptr<void>(a[1]), a[2]));
}

i64 s_memchr(const u64* a, std::string&) {
    return i64(std::memchr(ptr<void>(a[0]), int(a[1] & 0xff), a[2]));
}

i64 s_strrchr(const u64* a, std::string&) {
    return i64(std::strrchr(ptr<char>(a[0]), int(a[1] & 0xff)));
}

i64 s_strlen(const u64* a, std::string&) { return i64(strnlen(ptr<char>(a[0]), 1 << 16)); }

// The Python stand-ins compare at most 512 characters of each string.
i64 s_strcmp(const u64* a, std::string&) {
    return sign(std::strncmp(ptr<char>(a[0]), ptr<char>(a[1]), 512));
}

i64 s_strncmp(const u64* a, std::string&) {
    return sign(std::strncmp(ptr<char>(a[0]), ptr<char>(a[1]), std::min<u64>(a[2], 512)));
}

i64 s_strcpy(const u64* a, std::string&) {
    std::strcpy(ptr<char>(a[0]), ptr<char>(a[1]));
    return i64(a[0]);
}

// ---- the game's memory ----
//
// Direct memory is the PS4's physical memory, which a game allocates (an offset and a type) and
// then maps; flexible memory is mapped in one step. Here an allocation is a piece of the heap
// and maps 1:1, so its "physical" offset is its address. The loader records both, for the
// calls that ask about them. Released memory is forgotten but not reused.

// sceKernelAllocateDirectMemory's: offset: (length, memory type)
std::mutex g_mem_lock;
std::map<u64, std::pair<u64, int>> g_direct;

// The mappings: start: what is mapped there. The protection bits: 1 CPU read, 2 CPU write,
// 4 CPU execute, 0x10 GPU read, 0x20 GPU write.
struct Mapping {
    u64 end;
    int prot;
    bool direct;
    int type;  // the direct memory's type
    u64 offset;  // the direct memory's offset
};
std::map<u64, Mapping> g_maps;

// The mapping that holds addr, or (find_next) the first one after it; null if none. Under
// g_mem_lock.
const std::pair<const u64, Mapping>* mapping_at(u64 addr, bool find_next = false) {
    auto it = g_maps.upper_bound(addr);
    if (it != g_maps.begin()) {
        auto prev = std::prev(it);
        if (addr < prev->second.end) return &*prev;
    }
    return find_next && it != g_maps.end() ? &*it : nullptr;
}

// sceKernelAllocateDirectMemory(start, end, len, align, type, *physOut): hand out heap
i64 s_alloc_direct(const u64* a, std::string& note) {
    u64 p = alloc(a[2], std::max<u64>(a[3], 0x10000));
    *ptr<u64>(a[5]) = p;
    std::lock_guard lock(g_mem_lock);
    g_direct[p] = {a[2], int(a[4])};
    note = std::format("{:#x}, type {}", p, a[4]);
    return 0;
}

// sceKernelMapDirectMemory(void **addr, len, prot, flags, phys, align): map it 1:1
i64 s_map_direct(const u64* a, std::string&) {
    *ptr<u64>(a[0]) = a[4];
    std::lock_guard lock(g_mem_lock);
    int type = 0;
    auto it = g_direct.upper_bound(a[4]);
    if (it != g_direct.begin() && a[4] < std::prev(it)->first + std::prev(it)->second.first)
        type = std::prev(it)->second.second;
    g_maps[a[4]] = {a[4] + a[1], int(a[2]), true, type, a[4]};
    return 0;
}

// sceKernelMapFlexibleMemory / MapNamedFlexibleMemory(void **addr, len, prot, flags, ...)
i64 s_map_flexible(const u64* a, std::string&) {
    u64 p = alloc(a[1], 0x4000);
    *ptr<u64>(a[0]) = p;
    std::lock_guard lock(g_mem_lock);
    g_maps[p] = {p + a[1], int(a[2]), false, 0, 0};
    return 0;
}

constexpr i64 SCE_EACCES = i64(std::int32_t(0x8002000d)), SCE_EINVAL = i64(std::int32_t(0x80020016));

// sceKernelQueryMemoryProtection(addr, void **start, void **end, u32 *prot): the mapping that
// holds addr
i64 s_query_protection(const u64* a, std::string& note) {
    std::lock_guard lock(g_mem_lock);
    auto* m = mapping_at(a[0]);
    if (!m) {
        note = "not mapped";
        return SCE_EINVAL;
    }
    if (a[1]) *ptr<u64>(a[1]) = m->first;
    if (a[2]) *ptr<u64>(a[2]) = m->second.end;
    if (a[3]) *ptr<u32>(a[3]) = u32(m->second.prot);
    note = std::format("{:#x}-{:#x} prot {:#x}", m->first, m->second.end, m->second.prot);
    return 0;
}

// sceKernelVirtualQuery(addr, flags, SceKernelVirtualQueryInfo *info, size): the mapping that
// holds addr, or with flags bit 0, the next one. The info (0x48 bytes): start, end, offset,
// int protection, int memory type, a byte of flags (1 flexible, 2 direct, 0x10 committed), and
// a 32-byte name.
i64 s_virtual_query(const u64* a, std::string& note) {
    std::lock_guard lock(g_mem_lock);
    auto* m = mapping_at(a[0], a[1] & 1);
    if (!m) {
        note = "not mapped";
        return SCE_EACCES;
    }
    unsigned char info[0x48] = {};
    u64 fields[3] = {m->first, m->second.end, m->second.offset};
    std::memcpy(info, fields, sizeof fields);
    std::memcpy(info + 24, &m->second.prot, 4);
    std::memcpy(info + 28, &m->second.type, 4);
    info[32] = static_cast<unsigned char>((m->second.direct ? 2 : 1) | 0x10);
    std::memcpy(ptr<void>(a[2]), info, std::min<u64>(a[3], sizeof info));
    note = std::format("{:#x}-{:#x} {}", m->first, m->second.end, m->second.direct ? "direct" : "flexible");
    return 0;
}

// sceKernelGetDirectMemoryType(offset, int *type, off_t *start, off_t *end)
i64 s_direct_type(const u64* a, std::string&) {
    std::lock_guard lock(g_mem_lock);
    auto it = g_direct.upper_bound(a[0]);
    if (it == g_direct.begin() || a[0] >= std::prev(it)->first + std::prev(it)->second.first)
        return SCE_EACCES;
    --it;
    if (a[1]) *ptr<int>(a[1]) = it->second.second;
    if (a[2]) *ptr<u64>(a[2]) = it->first;
    if (a[3]) *ptr<u64>(a[3]) = it->first + it->second.first;
    return 0;
}

// sceKernelReleaseDirectMemory(offset, len): forget the allocation and its mappings
i64 s_release_direct(const u64* a, std::string&) {
    std::lock_guard lock(g_mem_lock);
    g_direct.erase(a[0]);
    for (auto it = g_maps.begin(); it != g_maps.end();)
        it = it->second.direct && it->second.offset == a[0] ? g_maps.erase(it) : std::next(it);
    return 0;
}

// sceKernelMunmap(addr, len)
i64 s_munmap(const u64* a, std::string&) {
    std::lock_guard lock(g_mem_lock);
    g_maps.erase(a[0]);
    return 0;
}

// __cxa_guard_acquire(guard): a function's static object, constructed once. 1 = "construct it
// now", while the guard's first byte is 0. Its second byte marks a construction in progress, so
// a second thread arriving meanwhile waits for it rather than constructing again.
std::mutex g_guard_lock;
std::condition_variable g_guard_done;

i64 s_guard_acquire(const u64* a, std::string&) {
    auto* g = ptr<unsigned char>(a[0]);
    std::unique_lock lock(g_guard_lock);
    g_guard_done.wait(lock, [&] { return g[0] != 0 || g[1] == 0; });
    if (g[0]) return 0;
    g[1] = 1;
    return 1;
}

i64 s_guard_release(const u64* a, std::string&) {
    auto* g = ptr<unsigned char>(a[0]);
    std::lock_guard lock(g_guard_lock);
    g[0] = 1;
    g[1] = 0;
    g_guard_done.notify_all();
    return 0;
}

// __cxa_guard_abort(guard): the constructor threw; let the next one try
i64 s_guard_abort(const u64* a, std::string&) {
    auto* g = ptr<unsigned char>(a[0]);
    std::lock_guard lock(g_guard_lock);
    g[1] = 0;
    g_guard_done.notify_all();
    return 0;
}

i64 s_direct_size(const u64*, std::string&) { return i64(5) << 30; }  // the 5 GiB a game is given

i64 s_stack_chk_fail(const u64*, std::string&) {
    stop("__stack_chk_fail: the game's stack check failed");
}

i64 s_zero(const u64*, std::string&) { return 0; }

// ---- printf ----
//
// The game's va_list is System V's: a record of where the next integer and floating-point
// arguments are, in the registers its caller saved or on the stack. Windows' va_list is a plain
// pointer, so the loader walks the game's itself, one conversion at a time, and formats each
// with the host's snprintf.

struct SysvVaList {
    u32 gp_offset;  // into reg_save_area: 0-48 for the six integer registers
    u32 fp_offset;  // 48-176 for xmm0-xmm7
    u64 overflow_arg_area;  // the arguments passed on the stack
    u64 reg_save_area;
};

u64 next_int(SysvVaList* v) {
    u64 x;
    if (v->gp_offset < 48) {
        std::memcpy(&x, ptr<void>(v->reg_save_area + v->gp_offset), 8);
        v->gp_offset += 8;
    } else {
        std::memcpy(&x, ptr<void>(v->overflow_arg_area), 8);
        v->overflow_arg_area += 8;
    }
    return x;
}

double next_double(SysvVaList* v) {
    double d;
    if (v->fp_offset < 176) {
        std::memcpy(&d, ptr<void>(v->reg_save_area + v->fp_offset), 8);
        v->fp_offset += 16;
    } else {
        std::memcpy(&d, ptr<void>(v->overflow_arg_area), 8);
        v->overflow_arg_area += 8;
    }
    return d;
}

std::string narrow16(const char16_t* s) {
    std::string out;
    for (; *s; s++) out += *s < 0x80 ? char(*s) : '?';
    return out;
}

std::string format_sysv(const char* f, SysvVaList* v) {
    std::string out;
    char buf[1024];
    while (*f) {
        if (*f != '%') {
            out += *f++;
            continue;
        }
        const char* start = f++;
        if (*f == '%') {
            out += *f++;
            continue;
        }
        std::string spec = "%";
        while (*f && std::strchr("-+ #0", *f)) spec += *f++;
        if (*f == '*') {
            spec += std::to_string(int(next_int(v)));
            f++;
        } else {
            while (*f >= '0' && *f <= '9') spec += *f++;
        }
        if (*f == '.') {
            spec += *f++;
            if (*f == '*') {
                spec += std::to_string(std::max(0, int(next_int(v))));
                f++;
            } else {
                while (*f >= '0' && *f <= '9') spec += *f++;
            }
        }
        std::string len;
        while (*f && std::strchr("hljztLq", *f)) len += *f++;
        char conv = *f ? *f++ : '\0';
        int bits = len == "hh" ? 8 : len == "h" ? 16 : len.empty() ? 32 : 64;
        switch (conv) {
            case 'd':
            case 'i': {
                i64 x = i64(next_int(v));
                if (bits < 64) x = i64(u64(x) << (64 - bits)) >> (64 - bits);  // sign-extend
                std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), static_cast<long long>(x));
                out += buf;
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o': {
                u64 x = next_int(v);
                if (bits < 64) x &= (u64(1) << bits) - 1;
                std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(),
                              static_cast<unsigned long long>(x));
                out += buf;
                break;
            }
            case 'c':
                std::snprintf(buf, sizeof buf, (spec + "c").c_str(), int(next_int(v) & 0xff));
                out += buf;
                break;
            case 's':
            case 'S': {
                u64 p = next_int(v);
                std::string s = !p ? "(null)"
                                : (len == "l" || conv == 'S') ? narrow16(ptr<char16_t>(p))
                                                              : std::string(ptr<char>(p));
                std::snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
                out += buf;
                break;
            }
            case 'p': out += std::format("0x{:x}", next_int(v)); break;
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
                if (len == "L") {  // long double: 16 bytes on the stack; not seen yet
                    out += "<long double>";
                    break;
                }
                std::snprintf(buf, sizeof buf, (spec + conv).c_str(), next_double(v));
                out += buf;
                break;
            case 'n': next_int(v); break;  // "store the count so far": not done
            default: out.append(start, f);  // not a conversion: copy it as written
        }
    }
    return out;
}

// vprintf(format, va_list): the text goes to the log and to stdout.txt.
// What the game printed: to stdout.txt, and the line to the log.
i64 printed(std::string text, std::string& note) {
    i64 n = i64(text.size());
    {
        std::lock_guard lock(M.log_lock);
        M.stdout_text += text;
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    note = std::format("\"{}\"", text);
    return n;
}

i64 s_vprintf(const u64* a, std::string& note) {
    return printed(format_sysv(ptr<char>(a[0]), ptr<SysvVaList>(a[1])), note);
}

// The variadic ones (printf, sprintf, ...) get their arguments as any call does. A va_list is
// built over them as the callee's own va_start would: the six integer registers and xmm0-xmm7
// in a register save area, the first `fixed` integer arguments already used, and the rest on
// the caller's stack.
std::string format_variadic(int fixed, u64 format) {
    Frame* f = current_frame();
    alignas(16) unsigned char save[176];
    std::memcpy(save, f->args, 48);
    std::memcpy(save + 48, f->xmm0, 16);
    std::memcpy(save + 64, f->xmm1, 16);
    std::memcpy(save + 80, f->xmm2_7, sizeof f->xmm2_7);
    SysvVaList v{u32(8 * fixed), 48, f->stack_args, reinterpret_cast<u64>(save)};
    return format_sysv(ptr<char>(format), &v);
}

// Copies text into the game's buffer of n bytes (n = 0: nothing), 0-terminated; returns the
// whole text's length, as snprintf does.
i64 into(u64 buf, u64 n, const std::string& text) {
    if (n) {
        u64 k = std::min<u64>(text.size(), n - 1);
        std::memcpy(ptr<void>(buf), text.data(), k);
        ptr<char>(buf)[k] = 0;
    }
    return i64(text.size());
}

i64 s_printf(const u64* a, std::string& note) { return printed(format_variadic(1, a[0]), note); }

// fprintf(FILE*, format, ...), vfprintf(FILE*, format, va_list): to a file the game opened,
// or else stdout or stderr
i64 to_stream(u64 file, const std::string& text, std::string& note) {
    if (stream_write(file, text)) return i64(text.size());
    return printed(text, note);
}
i64 s_fprintf(const u64* a, std::string& note) {
    return to_stream(a[0], format_variadic(2, a[1]), note);
}
i64 s_vfprintf(const u64* a, std::string& note) {
    return to_stream(a[0], format_sysv(ptr<char>(a[1]), ptr<SysvVaList>(a[2])), note);
}

// sprintf(buf, format, ...), snprintf(buf, n, format, ...), and their va_list forms
i64 s_sprintf(const u64* a, std::string&) {
    std::string text = format_variadic(2, a[1]);
    return into(a[0], text.size() + 1, text);
}
i64 s_snprintf(const u64* a, std::string&) { return into(a[0], a[1], format_variadic(3, a[2])); }
i64 s_vsprintf(const u64* a, std::string&) {
    std::string text = format_sysv(ptr<char>(a[1]), ptr<SysvVaList>(a[2]));
    return into(a[0], text.size() + 1, text);
}
i64 s_vsnprintf(const u64* a, std::string&) {
    return into(a[0], a[1], format_sysv(ptr<char>(a[2]), ptr<SysvVaList>(a[3])));
}

i64 s_strncpy(const u64* a, std::string&) {
    std::strncpy(ptr<char>(a[0]), ptr<char>(a[1]), a[2]);
    return i64(a[0]);
}

// ldiv(n, d): a 16-byte struct { quot, rem }, returned in rax and rdx
i64 s_ldiv(const u64* a, std::string&) {
    i64 n = i64(a[0]), d = i64(a[1]);
    if (d == 0) stop("ldiv by zero");
    current_frame()->ret2 = u64(n % d);
    return n / d;
}

// _FSin(float x, unsigned quadrant): Dinkumware's sine and cosine in one. The quadrant offset
// picks sin, cos, -sin, -cos; x comes and goes in xmm0.
i64 s_fsin(const u64* a, std::string& note) {
    float x;
    std::memcpy(&x, current_frame()->xmm0, 4);
    float r = (a[0] & 1) ? std::cos(x) : std::sin(x);
    if (a[0] & 2) r = -r;
    std::memcpy(current_frame()->fret, &r, 4);
    note = std::format("{} -> {}", x, r);
    return 0;
}

// ---- wide strings: the PS4's wchar_t is 16 bits (Linux's is 32), so these work on char16_t ----

using wchar = char16_t;

u64 wlen(const wchar* s) {
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

std::string narrow(const wchar* s, u64 limit = 80) {  // for the log
    std::string out;
    for (u64 i = 0; s[i] && i < limit; i++) out += s[i] < 0x80 ? char(s[i]) : '?';
    return out;
}

i64 s_wcslen(const u64* a, std::string&) { return i64(wlen(ptr<wchar>(a[0]))); }

i64 wcompare(const wchar* x, const wchar* y, u64 n) {
    for (u64 i = 0; i < n; i++) {
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
        if (!x[i]) return 0;
    }
    return 0;
}

i64 s_wcscmp(const u64* a, std::string& note) {
    note = std::format("\"{}\" \"{}\"", narrow(ptr<wchar>(a[0])), narrow(ptr<wchar>(a[1])));
    return wcompare(ptr<wchar>(a[0]), ptr<wchar>(a[1]), ~u64(0));
}

i64 s_wcsncmp(const u64* a, std::string& note) {
    note = std::format("\"{}\" \"{}\"", narrow(ptr<wchar>(a[0]), a[2]),
                       narrow(ptr<wchar>(a[1]), a[2]));
    return wcompare(ptr<wchar>(a[0]), ptr<wchar>(a[1]), a[2]);
}

// wcstol(s, wchar **end, base)
i64 s_wcstol(const u64* a, std::string& note) {
    const wchar* s = ptr<wchar>(a[0]);
    const wchar* p = s;
    int base = int(a[2]);
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    i64 v = 0;
    const wchar* start = p;
    for (;; p++) {
        int d = *p >= '0' && *p <= '9'   ? *p - '0'
                : *p >= 'a' && *p <= 'z' ? *p - 'a' + 10
                : *p >= 'A' && *p <= 'Z' ? *p - 'A' + 10
                                         : 99;
        if (d >= base) break;
        v = v * base + d;
    }
    if (p == start) p = s;  // no digits: nothing converted
    if (a[1]) *ptr<const wchar*>(a[1]) = p;
    note = std::format("\"{}\"", narrow(s));
    return neg ? -v : v;
}

i64 s_wmemcpy(const u64* a, std::string&) {  // wmemcpy and wmemmove
    std::memmove(ptr<void>(a[0]), ptr<void>(a[1]), a[2] * sizeof(wchar));
    return i64(a[0]);
}

i64 s_wmemset(const u64* a, std::string&) {
    for (u64 i = 0; i < a[2]; i++) ptr<wchar>(a[0])[i] = wchar(a[1]);
    return i64(a[0]);
}

i64 s_wmemcmp(const u64* a, std::string&) {
    const wchar *x = ptr<wchar>(a[0]), *y = ptr<wchar>(a[1]);
    for (u64 i = 0; i < a[2]; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

// wcschr(s, c): the terminator counts as part of the string
i64 s_wcschr(const u64* a, std::string&) {
    for (const wchar* p = ptr<wchar>(a[0]);; p++) {
        if (*p == wchar(a[1])) return i64(p);
        if (!*p) return 0;
    }
}

i64 s_wmemchr(const u64* a, std::string&) {
    const wchar* p = ptr<wchar>(a[0]);
    for (u64 i = 0; i < a[2]; i++)
        if (p[i] == wchar(a[1])) return i64(p + i);
    return 0;
}

// wcsncpy(dst, src, n): pads with zeros to n
i64 s_wcsncpy(const u64* a, std::string&) {
    wchar *d = ptr<wchar>(a[0]);
    const wchar* s = ptr<wchar>(a[1]);
    u64 i = 0;
    for (; i < a[2] && s[i]; i++) d[i] = s[i];
    for (; i < a[2]; i++) d[i] = 0;
    return i64(a[0]);
}

// ---- the C library's character tables ----
//
// The PS4's libc is Dinkumware's. Its <ctype.h> looks a character up in a table of shorts,
// reached through _Getpctype(); _Getptolower() and _Getptoupper() give the case-mapping tables.
// Each table here covers -128..255, so a signed char or EOF (-1) indexes it safely.

enum : short {
    XD = 0x01,  // hex digit
    UP = 0x02,  // 'A'-'Z'
    SP = 0x04,  // space
    PU = 0x08,  // punctuation
    LO = 0x10,  // 'a'-'z'
    DI = 0x20,  // '0'-'9'
    CN = 0x40,  // \t \n \v \f \r
    BB = 0x80,  // other control characters
};

struct Tables {
    short ctype[384] = {}, lower[384] = {}, upper[384] = {};
    Tables() {
        for (int c = -128; c < 256; c++) {
            short f = 0;
            if (c >= 0 && c < 0x80) {
                if (c >= '0' && c <= '9') f = DI | XD;
                else if (c >= 'a' && c <= 'z') f = LO | (c <= 'f' ? XD : 0);
                else if (c >= 'A' && c <= 'Z') f = UP | (c <= 'F' ? XD : 0);
                else if (c == ' ') f = SP;
                else if (c >= '\t' && c <= '\r') f = CN;
                else if (c < 0x20 || c == 0x7f) f = BB;
                else f = PU;
            }
            ctype[c + 128] = f;
            lower[c + 128] = short(c >= 'A' && c <= 'Z' ? c + 32 : c);
            upper[c + 128] = short(c >= 'a' && c <= 'z' ? c - 32 : c);
        }
    }
};

// copied to a fixed address (LIBC_DATA), so every run hands the game the same pointers
Tables* tables() { return ptr<Tables>(LIBC_DATA); }

i64 s_getpctype(const u64*, std::string&) { return i64(&tables()->ctype[128]); }
i64 s_getptolower(const u64*, std::string&) { return i64(&tables()->lower[128]); }
i64 s_getptoupper(const u64*, std::string&) { return i64(&tables()->upper[128]); }

// gettimeofday(struct timeval *tv, tz): the real clock. (With the game's threads running, a run
// doesn't repeat exactly anyway, and timed waits are measured against this clock.)
i64 s_gettimeofday(const u64* a, std::string&) {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    if (a[0]) {
        ptr<i64>(a[0])[0] = us / 1000000;
        ptr<i64>(a[0])[1] = us % 1000000;
    }
    return 0;
}

// ---- clocks ----

// Seconds and nanoseconds, as FreeBSD's struct timespec: two 64-bit fields.
void write_timespec(u64 at, std::chrono::nanoseconds t) {
    ptr<i64>(at)[0] = t.count() / 1000000000;
    ptr<i64>(at)[1] = t.count() % 1000000000;
}

// clock_gettime(clockid, struct timespec *): FreeBSD's CLOCK_REALTIME (0) is the wall clock;
// the others (CLOCK_MONOTONIC 4 and the rest) count from an arbitrary start
i64 clock_now(const u64* a) {
    if (!a[1]) return 0;
    if (a[0] == 0)
        write_timespec(a[1], std::chrono::system_clock::now().time_since_epoch());
    else
        write_timespec(a[1], std::chrono::steady_clock::now().time_since_epoch());
    return 0;
}
i64 s_clock_gettime(const u64* a, std::string&) { return clock_now(a); }

// sceKernelReadTsc(): the CPU's time-stamp counter. The game doesn't ask its rate
// (sceKernelGetTscFrequency); this one counts at 1.6 GHz, the PS4 CPU's clock.
i64 s_read_tsc(const u64*, std::string&) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - M.t0);
    return ns.count() * 8 / 5;
}

// sceUserServiceGetEvent(SceUserServiceEvent *): a user logging in or out. None happen.
i64 s_user_event(const u64*, std::string&) { return i64(std::int32_t(0x80960007)); }  // NO_EVENT

// sceNetEpollWait(epoll, events, max, timeout in microseconds, or -1): there is no network, so
// nothing ever arrives; wait out the timeout (a second at a time, for ever) and report none
i64 s_epoll_wait(const u64* a, std::string&) {
    std::int32_t timeout = std::int32_t(a[3]);
    std::this_thread::sleep_for(std::chrono::microseconds(timeout < 0 ? 1000000 : timeout));
    return 0;
}

// time(time_t *t)
i64 s_time(const u64* a, std::string&) {
    i64 s = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
    if (a[0]) *ptr<i64>(a[0]) = s;
    return s;
}

// clock(): processor time, in FreeBSD's CLOCKS_PER_SEC (128); here, time since the run began
i64 s_clock(const u64*, std::string&) {
    auto t = std::chrono::steady_clock::now() - M.t0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(t).count() * 128 / 1000;
}

// exp2f(float x): in and out through xmm0
i64 s_exp2f(const u64*, std::string&) {
    float x;
    std::memcpy(&x, current_frame()->xmm0, 4);
    float r = std::exp2(x);
    std::memcpy(current_frame()->fret, &r, 4);
    return 0;
}

// powf(float x, float y): in through xmm0 and xmm1, out through xmm0
i64 s_powf(const u64*, std::string&) {
    float x, y;
    std::memcpy(&x, current_frame()->xmm0, 4);
    std::memcpy(&y, current_frame()->xmm1, 4);
    float r = std::pow(x, y);
    std::memcpy(current_frame()->fret, &r, 4);
    return 0;
}

// _WStof(const wchar_t *s, wchar_t **end, long pw): Dinkumware's wide text to float, scaled by
// 10 to the pw; the PS4's wchar_t is 16 bits. Out through xmm0.
i64 s_wstof(const u64* a, std::string& note) {
    const auto* s = ptr<char16_t>(a[0]);
    std::string text;
    for (u64 i = 0; s[i] && s[i] < 0x80 && i < 256; i++) text += char(s[i]);
    char* end = nullptr;
    float r = std::strtof(text.c_str(), &end);
    if (i64(a[2])) r *= std::pow(10.0f, float(i64(a[2])));
    if (a[1]) *ptr<u64>(a[1]) = a[0] + u64(end - text.c_str()) * 2;
    std::memcpy(current_frame()->fret, &r, 4);
    note = std::format("\"{}\" -> {}", text, r);
    return 0;
}

// ---- system settings ----

// sceSystemServiceParamGetInt(id, int *value): the console's settings
i64 s_system_param(const u64* a, std::string& note) {
    int v = 0;
    switch (a[0]) {
        case 1: v = 1; note = "language: English (US)"; break;
        case 1002: v = 1; note = "enter button: cross"; break;
        default: note = std::format("setting {}: 0", a[0]);
    }
    *ptr<int>(a[1]) = v;
    return 0;
}

// sceAppContentAppParamGetInt(id, int *value): the game's own parameters (param.sfo's
// user-defined values); 0 for each
i64 s_app_param(const u64* a, std::string& note) {
    *ptr<int>(a[1]) = 0;
    note = std::format("parameter {}: 0", a[0]);
    return 0;
}

}  // namespace

std::string format_sysv(const char* format, u64 va) {
    return format_sysv(format, ptr<SysvVaList>(va));
}

const std::unordered_map<std::string, Standin>& standins() {
    static const std::unordered_map<std::string, Standin> table = [] {
        std::unordered_map<std::string, Standin> t = {
            {"malloc", s_malloc},
            {"calloc", s_calloc},
            {"realloc", s_realloc},
            {"memalign", s_memalign},
            {"aligned_alloc", s_memalign},
            {"posix_memalign", s_posix_memalign},
            {"_Znwm", s_malloc},
            {"_Znam", s_malloc},
            {"memcpy", s_memcpy},
            {"memmove", s_memcpy},
            {"memset", s_memset},
            {"memcmp", s_memcmp},
            {"strlen", s_strlen},
            {"strcmp", s_strcmp},
            {"strncmp", s_strncmp},
            {"strcpy", s_strcpy},
            {"sceKernelAllocateDirectMemory", s_alloc_direct},
            {"sceKernelMapDirectMemory", s_map_direct},
            {"sceKernelMapFlexibleMemory", s_map_flexible},
            {"sceKernelMapNamedFlexibleMemory", s_map_flexible},
            {"__stack_chk_fail", s_stack_chk_fail},
            {"__cxa_guard_acquire", s_guard_acquire},
            {"__cxa_guard_release", s_guard_release},
            {"__cxa_guard_abort", s_guard_abort},
            {"sceKernelGetDirectMemorySize", s_direct_size},
            {"vprintf", s_vprintf},
            {"printf", s_printf},
            {"fprintf", s_fprintf},
            {"vfprintf", s_vfprintf},
            {"sprintf", s_sprintf},
            {"snprintf", s_snprintf},
            {"vsprintf", s_vsprintf},
            {"vsnprintf", s_vsnprintf},
            {"strncpy", s_strncpy},
            {"ldiv", s_ldiv},
            {"_FSin", s_fsin},
            {"wcslen", s_wcslen},
            {"wcscmp", s_wcscmp},
            {"wcsncmp", s_wcsncmp},
            {"wcstol", s_wcstol},
            {"wmemcpy", s_wmemcpy},
            {"wmemmove", s_wmemcpy},
            {"wmemset", s_wmemset},
            {"wmemcmp", s_wmemcmp},
            {"wcschr", s_wcschr},
            {"wmemchr", s_wmemchr},
            {"wcsncpy", s_wcsncpy},
            {"memchr", s_memchr},
            {"_Getpctype", s_getpctype},
            {"_Getptolower", s_getptolower},
            {"_Getptoupper", s_getptoupper},
            {"gettimeofday", s_gettimeofday},
            {"clock_gettime", s_clock_gettime},
            {"sceKernelClockGettime", s_clock_gettime},
            {"sceKernelReadTsc", s_read_tsc},
            {"sceUserServiceGetEvent", s_user_event},
            {"sceNetEpollWait", s_epoll_wait},
            {"time", s_time},
            {"clock", s_clock},
            {"exp2f", s_exp2f},
            {"powf", s_powf},
            {"_WStof", s_wstof},
            {"strrchr", s_strrchr},
            {"srand", s_zero},
            {"sceKernelQueryMemoryProtection", s_query_protection},
            {"sceKernelVirtualQuery", s_virtual_query},
            {"sceKernelGetDirectMemoryType", s_direct_type},
            {"sceKernelReleaseDirectMemory", s_release_direct},
            {"sceKernelMunmap", s_munmap},
            {"sceSystemServiceParamGetInt", s_system_param},
            {"sceAppContentAppParamGetInt", s_app_param},
        };
        // Imports for which 0 ("success", "nothing to do") is the right answer: exit handlers
        // never run.
        for (const char* n : {"_init_env", "atexit", "__cxa_atexit"})
            t.emplace(n, s_zero);
        add_thread_standins(t);
        add_file_standins(t);
        add_gnm_standins(t);
        add_libc_standins(t);
        add_audio_standins(t);
        add_video_standins(t);
        return t;
    }();
    return table;
}

void init_libc_data() {
    static_assert(sizeof(Tables) <= PAGE);
    new (ptr<void>(LIBC_DATA)) Tables;
}


}  // namespace mensis
