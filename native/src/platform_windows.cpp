// SPDX-License-Identifier: GPL-3.0-or-later
//
// Windows: 64-bit Windows doesn't let a program set fs, and gs points at each thread's own
// information block (TEB). The game's thread-pointer loads are rewritten to read gs:[0x1480 +
// 8 * slot]: the TEB's thread-local slots start at 0x1480, so this reads Windows TLS slot `slot`
// (from TlsAlloc), where each game thread keeps its thread pointer. No stack, flag or other
// register is touched.
//
// A fault is caught by a vectored exception handler, which records it and resumes the thread in
// mensis_restore, back on the loader's stack. But first, it may be a touch of a render target
// kept on the PC's GPU (vkdraw.cpp), whose memory is guarded: then the picture comes back into
// the memory, and the thread goes on.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <cstdio>
#include <format>
#include <thread>
#include <vector>

#include "loader.hpp"
#include "vkdraw.hpp"
#include "vulkan.hpp"

using namespace mensis;

namespace {

DWORD g_slot = TLS_OUT_OF_INDEXES;

LONG CALLBACK on_exception(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters > 1 && r->ExceptionInformation[0] <= 1 &&
        vkdraw::touched(r->ExceptionInformation[1], r->ExceptionInformation[0] == 1))
        return EXCEPTION_CONTINUE_EXECUTION;  // any thread's, the loader's too
    GameThread* t = current_thread();
    if (!t || !t->enter.saved_rsp) return EXCEPTION_CONTINUE_SEARCH;
    std::string what;
    switch (r->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION: {
            u64 kind = r->NumberParameters > 0 ? r->ExceptionInformation[0] : 0;
            u64 at = r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0;
            what = std::format("memory fault ({}) at {:#x}",
                               kind == 0 ? "read" : kind == 1 ? "write" : "execute", at);
            break;
        }
        case EXCEPTION_IN_PAGE_ERROR: what = "memory fault (in-page error)"; break;
        case EXCEPTION_DATATYPE_MISALIGNMENT: what = "misaligned access"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: what = "invalid instruction"; break;
        case EXCEPTION_PRIV_INSTRUCTION: what = "privileged instruction"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: what = "integer divide by zero"; break;
        case EXCEPTION_INT_OVERFLOW: what = "integer overflow"; break;
        case EXCEPTION_BREAKPOINT: what = "breakpoint (int3)"; break;
        case EXCEPTION_STACK_OVERFLOW: what = "stack overflow"; break;
        default: return EXCEPTION_CONTINUE_SEARCH;  // not a fault of the game's (a C++ throw)
    }
    const CONTEXT& c = *e->ContextRecord;
    t->fault = {true, c.Rip, c.Rsp, c.Rbp, c.Rax, c.Rdi, c.Rsi};
    t->fault_reason = what;
    t->faulted = true;
    e->ContextRecord->Rip = reinterpret_cast<DWORD64>(&mensis_restore);
    e->ContextRecord->Rcx = t->enter.saved_rsp;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// ---- the controller ----
//
// The keys held in the window, and an XInput gamepad (an Xbox controller, or another that
// Windows presents as one), the first connected, through xinput1_4.dll if it is there.

std::atomic<bool> g_keys[256];

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn xinput() {
    static XInputGetStateFn f = [] {
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
        return m ? reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState"))) : nullptr;
    }();
    return f;
}

// A stick's axis (-32768 to 32767, up positive) as the PS4's byte (0 to 255, down growing)
std::uint8_t axis(SHORT v, bool flip) {
    int x = std::abs(int(v)) < 4000 ? 0 : int(v);  // a small dead zone: a resting stick drifts
    if (flip) x = -x - 1;
    return std::uint8_t(std::clamp((x + 32768) >> 8, 0, 255));
}

// ---- the window (--window) ----
//
// The display's buffer on screen, scaled to the window, redrawn 30 times a second; the title
// counts the flips and their rate. Drawn through Vulkan (vulkan.cpp), or through GDI if Vulkan
// can't be used.

std::vector<u32> g_pixels;
u32 g_width = 0, g_height = 0;
u64 g_flips = 0, g_flips_then = 0;
ULONGLONG g_then = 0;
double g_rate = 0;
bool g_vulkan = false;

void present_vulkan(HWND w) {
    RECT r;
    GetClientRect(w, &r);
    vk::present(g_pixels.data(), g_width, g_height, u32(r.right), u32(r.bottom));
}

LRESULT CALLBACK window_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER: {
            g_flips = video_picture(g_pixels, g_width, g_height);
            ULONGLONG now = GetTickCount64();
            if (now - g_then >= 1000) {
                g_rate = double(g_flips - g_flips_then) * 1000.0 / double(now - g_then);
                g_flips_then = g_flips;
                g_then = now;
            }
            std::wstring title = g_flips ? std::format(L"Mensis: frame {}, {:.1f} a second", g_flips, g_rate)
                                         : std::wstring(L"Mensis: starting");
            SetWindowTextW(w, title.c_str());
            if (g_vulkan)
                present_vulkan(w);
            else
                InvalidateRect(w, nullptr, FALSE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(w, &ps);
            if (g_vulkan) {
                EndPaint(w, &ps);
                present_vulkan(w);
                return 0;
            }
            RECT r;
            GetClientRect(w, &r);
            if (g_width) {
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = LONG(g_width);
                bi.bmiHeader.biHeight = -LONG(g_height);  // top down
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(dc, HALFTONE);
                StretchDIBits(dc, 0, 0, r.right, r.bottom, 0, 0, int(g_width), int(g_height),
                              g_pixels.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            } else {
                FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
            EndPaint(w, &ps);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_KEYUP:
            if (wp < 256) g_keys[wp] = msg == WM_KEYDOWN;
            return 0;
        case WM_KILLFOCUS:  // keys let go of elsewhere aren't seen here
            for (auto& k : g_keys) k = false;
            return 0;
        case WM_CLOSE:
            finish("the window was closed");
        default:
            return DefWindowProcW(w, msg, wp, lp);
    }
}

}  // namespace

namespace mensis::platform {

bool open_window() {
    std::thread([] {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = window_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
        wc.lpszClassName = L"MensisDisplay";
        RegisterClassW(&wc);
        RECT r{0, 0, 960, 540};  // half the display's 1920x1080
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        HWND w = CreateWindowExW(0, wc.lpszClassName, L"Mensis: starting", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                 nullptr, nullptr, inst, nullptr);
        if (!w) {
            std::fprintf(stderr, "can't open the window (error %lu)\n", GetLastError());
            return;
        }
        std::string error;
        g_vulkan = vk::open(inst, w, error);
        if (!g_vulkan) std::fprintf(stderr, "the window uses GDI: Vulkan couldn't be used: %s\n", error.c_str());
        ShowWindow(w, SW_SHOW);
        g_then = GetTickCount64();
        SetTimer(w, 1, 33, nullptr);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }).detach();
    return true;
}

std::vector<std::string> window_report() { return vk::report(); }

PadInput pad_input() {
    PadInput in;
    struct Key {
        int vk;
        u32 bit;
    };
    static const Key keys[] = {
        {VK_UP, 0x10},    {VK_RIGHT, 0x20}, {VK_DOWN, 0x40},  {VK_LEFT, 0x80},   {VK_RETURN, 0x2000}, {VK_SPACE, 0x2000},
        {VK_BACK, 0x4000}, {'Q', 0x8000},   {'E', 0x1000},    {'Z', 0x400},      {'X', 0x800},        {'C', 0x100},
        {'V', 0x200},     {'B', 0x2},       {'N', 0x4},       {VK_ESCAPE, 0x8},  {'T', 0x100000},
    };
    // Enter and Space are Circle, Backspace Cross: Bloodborne (this release) confirms with Circle
    for (const Key& k : keys)
        if (g_keys[k.vk]) in.buttons |= k.bit;
    // the sticks: W A S D the left, I J K L the right
    if (g_keys['A']) in.lx = 0;
    if (g_keys['D']) in.lx = 255;
    if (g_keys['W']) in.ly = 0;
    if (g_keys['S']) in.ly = 255;
    if (g_keys['J']) in.rx = 0;
    if (g_keys['L']) in.rx = 255;
    if (g_keys['I']) in.ry = 0;
    if (g_keys['K']) in.ry = 255;
    // a gamepad: looked for at most once a second while there is none (asking after a pad that
    // isn't there is slow)
    static std::atomic<int> pad{-1};
    static std::atomic<ULONGLONG> looked{0};
    XInputGetStateFn get = xinput();
    if (!get) return in;
    XINPUT_STATE st{};
    int p = pad;
    if (p < 0 || get(DWORD(p), &st) != ERROR_SUCCESS) {
        pad = -1;
        ULONGLONG now = GetTickCount64();
        if (now - looked < 1000) return in;
        looked = now;
        for (p = 0; p < 4 && get(DWORD(p), &st) != ERROR_SUCCESS; p++) {}
        if (p == 4) return in;
        pad = p;
    }
    const XINPUT_GAMEPAD& g = st.Gamepad;
    static const Key buttons[] = {
        {XINPUT_GAMEPAD_DPAD_UP, 0x10}, {XINPUT_GAMEPAD_DPAD_RIGHT, 0x20}, {XINPUT_GAMEPAD_DPAD_DOWN, 0x40},
        {XINPUT_GAMEPAD_DPAD_LEFT, 0x80}, {XINPUT_GAMEPAD_A, 0x4000}, {XINPUT_GAMEPAD_B, 0x2000},
        {XINPUT_GAMEPAD_X, 0x8000}, {XINPUT_GAMEPAD_Y, 0x1000}, {XINPUT_GAMEPAD_LEFT_SHOULDER, 0x400},
        {XINPUT_GAMEPAD_RIGHT_SHOULDER, 0x800}, {XINPUT_GAMEPAD_LEFT_THUMB, 0x2}, {XINPUT_GAMEPAD_RIGHT_THUMB, 0x4},
        {XINPUT_GAMEPAD_START, 0x8}, {XINPUT_GAMEPAD_BACK, 0x100000},
    };
    for (const Key& b : buttons)
        if (g.wButtons & b.vk) in.buttons |= b.bit;
    in.l2 = std::max(in.l2, g.bLeftTrigger);
    in.r2 = std::max(in.r2, g.bRightTrigger);
    if (g.bLeftTrigger > 30) in.buttons |= 0x100;  // L2 and R2 are buttons too, once pressed a little
    if (g.bRightTrigger > 30) in.buttons |= 0x200;
    if (in.lx == 128) in.lx = axis(g.sThumbLX, false);
    if (in.ly == 128) in.ly = axis(g.sThumbLY, true);
    if (in.rx == 128) in.rx = axis(g.sThumbRX, false);
    if (in.ry == 128) in.ry = axis(g.sThumbRY, true);
    return in;
}

void reserve(u64 addr, u64 size) {
    void* p = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE);
    if (p != reinterpret_cast<void*>(addr)) {
        DWORD error = GetLastError();
        // what is in the way: the first allocation inside the range
        std::string in_way;
        for (u64 at = addr; at < addr + size;) {
            MEMORY_BASIC_INFORMATION mi{};
            if (!VirtualQuery(reinterpret_cast<void*>(at), &mi, sizeof mi)) break;
            if (mi.State != MEM_FREE) {
                in_way = std::format(
                    "; in the way: {:#x} bytes at {:#x} ({}), allocated at {:#x}", mi.RegionSize,
                    u64(mi.BaseAddress),
                    mi.Type == MEM_IMAGE ? "a loaded module"
                    : mi.Type == MEM_MAPPED ? "a mapped file"
                                            : "private memory",
                    u64(mi.AllocationBase));
                break;
            }
            at = u64(mi.BaseAddress) + mi.RegionSize;
        }
        die(std::format("can't reserve {:#x} bytes at {:#x}: error {}{}", size, addr, error,
                        in_way));
    }
}

bool try_reserve(u64 addr, u64 size) {
    return VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE,
                        PAGE_EXECUTE_READWRITE) == reinterpret_cast<void*>(addr);
}

void commit(u64 addr, u64 size) {
    if (!VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_COMMIT, PAGE_EXECUTE_READWRITE))
        die(std::format("can't commit {:#x} bytes at {:#x}: error {}", size, addr,
                        GetLastError()));
}

u32 prepare() {
    g_slot = TlsAlloc();
    if (g_slot == TLS_OUT_OF_INDEXES || g_slot >= 64)
        die("no Windows TLS slot below 64 is free (the TEB holds only the first 64)");
    return 0x1480 + 8 * g_slot;
}

void protect(u64 addr, u64 size, int access) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(addr), size,
                   access == 0 ? PAGE_NOACCESS : access == 1 ? PAGE_READONLY : PAGE_EXECUTE_READWRITE, &old);
}

void install_fault_handler() { AddVectoredExceptionHandler(1, on_exception); }

void enter_thread(u64 tp) { TlsSetValue(g_slot, reinterpret_cast<void*>(tp)); }

// The bytes at rip (Windows has no objdump to hand): look the address up in Ghidra.
std::vector<std::string> disassemble(u64 rip) {
    unsigned char buf[16];
    SIZE_T n = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(rip), buf, sizeof buf,
                           &n) ||
        n == 0)
        return {"  (rip is not in mapped memory)"};
    std::string s = "  bytes:";
    for (SIZE_T i = 0; i < n; i++) s += std::format(" {:02x}", buf[i]);
    return {s};
}

}  // namespace mensis::platform
