// SPDX-License-Identifier: GPL-3.0-or-later
//
// The C library's mathematics, strings and number parsing, as the game imports them.
//
// The PS4's C library is Dinkumware's, so besides the standard names there are its internal
// ones (_FLog, _Sin, _Stoul, ...), which its headers' inline functions call. Floating-point
// arguments and results travel in xmm registers, so these read the call's frame.

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

#include "loader.hpp"

namespace mensis {
namespace {

template <typename T> T* ptr(u64 a) { return reinterpret_cast<T*>(a); }

unsigned char* xmm(int i) {
    Frame* f = current_frame();
    return i == 0 ? f->xmm0 : i == 1 ? f->xmm1 : f->xmm2_7[i - 2];
}
float fx(int i) {
    float v;
    std::memcpy(&v, xmm(i), 4);
    return v;
}
double dx(int i) {
    double v;
    std::memcpy(&v, xmm(i), 8);
    return v;
}
void ret(float v) { std::memcpy(current_frame()->fret, &v, 4); }
void ret(double v) { std::memcpy(current_frame()->fret, &v, 8); }

// ---- mathematics ----

#define F1(name, expr) \
    i64 name(const u64*, std::string&) { \
        float x = fx(0); \
        ret(float(expr)); \
        return 0; \
    }
#define F2(name, expr) \
    i64 name(const u64*, std::string&) { \
        float x = fx(0), y = fx(1); \
        ret(float(expr)); \
        return 0; \
    }
#define D1(name, expr) \
    i64 name(const u64*, std::string&) { \
        double x = dx(0); \
        ret(double(expr)); \
        return 0; \
    }
#define D2(name, expr) \
    i64 name(const u64*, std::string&) { \
        double x = dx(0), y = dx(1); \
        ret(double(expr)); \
        return 0; \
    }

F1(m_acosf, std::acos(x))
F1(m_asinf, std::asin(x))
F1(m_atanf, std::atan(x))
F1(m_ceilf, std::ceil(x))
F1(m_expf, std::exp(x))
F1(m_fabsf, std::fabs(x))
F1(m_floorf, std::floor(x))
F1(m_sqrtf, std::sqrt(x))
F1(m_tanf, std::tan(x))
F1(m_tanhf, std::tanh(x))
F2(m_atan2f, std::atan2(x, y))
F2(m_fmodf, std::fmod(x, y))
D1(m_acos, std::acos(x))
D1(m_asin, std::asin(x))
D1(m_atan, std::atan(x))
D1(m_ceil, std::ceil(x))
D1(m_exp, std::exp(x))
D1(m_exp2, std::exp2(x))
D1(m_fabs, std::fabs(x))
D1(m_floor, std::floor(x))
D1(m_sqrt, std::sqrt(x))
D1(m_tan, std::tan(x))
D2(m_atan2, std::atan2(x, y))
D2(m_fmod, std::fmod(x, y))
D2(m_pow, std::pow(x, y))
// Dinkumware's _FSinh(x, y) and _FCosh(x, y) are y * sinh(x) and y * cosh(x)
F2(m_fsinh, y * std::sinh(x))
F2(m_fcosh, y * std::cosh(x))

// _FLog(float x, int base), _Log(double x, int base): base 0 the natural logarithm, 1 base 10,
// -1 base 2
template <typename T> T dlog(T x, int base) {
    return base == 0 ? std::log(x) : base > 0 ? std::log10(x) : std::log2(x);
}
i64 m_flog(const u64* a, std::string&) {
    ret(dlog(fx(0), int(a[0])));
    return 0;
}
i64 m_log(const u64* a, std::string&) {
    ret(dlog(dx(0), int(a[0])));
    return 0;
}

// _Sin(double x, unsigned quadrant): sin, cos, -sin, -cos, as _FSin
i64 m_sin(const u64* a, std::string&) {
    double x = dx(0);
    double r = (a[0] & 1) ? std::cos(x) : std::sin(x);
    ret((a[0] & 2) ? -r : r);
    return 0;
}

// _Dtest(double *x): Dinkumware's classification: 0 zero, -1 finite, -2 subnormal, 1 infinite,
// 2 not a number
i64 m_dtest(const u64* a, std::string&) {
    double x = *ptr<double>(a[0]);
    switch (std::fpclassify(x)) {
        case FP_ZERO: return 0;
        case FP_SUBNORMAL: return -2;
        case FP_INFINITE: return 1;
        case FP_NAN: return 2;
        default: return -1;
    }
}

// frexpf(float, int *exp), frexp(double, int *exp)
i64 m_frexpf(const u64* a, std::string&) {
    ret(std::frexp(fx(0), ptr<int>(a[0])));
    return 0;
}
i64 m_frexp(const u64* a, std::string&) {
    ret(std::frexp(dx(0), ptr<int>(a[0])));
    return 0;
}
// ldexpf(float, int exp), ldexp(double, int exp)
i64 m_ldexpf(const u64* a, std::string&) {
    ret(std::ldexp(fx(0), int(a[0])));
    return 0;
}
i64 m_ldexp(const u64* a, std::string&) {
    ret(std::ldexp(dx(0), int(a[0])));
    return 0;
}
// modff(float, float *whole), modf(double, double *whole)
i64 m_modff(const u64* a, std::string&) {
    ret(std::modf(fx(0), ptr<float>(a[0])));
    return 0;
}
i64 m_modf(const u64* a, std::string&) {
    ret(std::modf(dx(0), ptr<double>(a[0])));
    return 0;
}

// ---- strings ----

i64 s_strcat(const u64* a, std::string&) { return i64(std::strcat(ptr<char>(a[0]), ptr<char>(a[1]))); }
i64 s_strncat(const u64* a, std::string&) {
    return i64(std::strncat(ptr<char>(a[0]), ptr<char>(a[1]), a[2]));
}
i64 s_strchr(const u64* a, std::string&) { return i64(std::strchr(ptr<char>(a[0]), int(a[1] & 0xff))); }
i64 s_strstr(const u64* a, std::string&) { return i64(std::strstr(ptr<char>(a[0]), ptr<char>(a[1]))); }
i64 s_strpbrk(const u64* a, std::string&) { return i64(std::strpbrk(ptr<char>(a[0]), ptr<char>(a[1]))); }
i64 s_strspn(const u64* a, std::string&) { return i64(std::strspn(ptr<char>(a[0]), ptr<char>(a[1]))); }
i64 s_strcspn(const u64* a, std::string&) { return i64(std::strcspn(ptr<char>(a[0]), ptr<char>(a[1]))); }

int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
i64 casecmp(const char* x, const char* y, u64 n) {
    for (u64 i = 0; i < n; i++) {
        int p = lower(static_cast<unsigned char>(x[i])), q = lower(static_cast<unsigned char>(y[i]));
        if (p != q) return p < q ? -1 : 1;
        if (!p) return 0;
    }
    return 0;
}
i64 s_strcasecmp(const u64* a, std::string&) { return casecmp(ptr<char>(a[0]), ptr<char>(a[1]), ~u64(0)); }
i64 s_strncasecmp(const u64* a, std::string&) { return casecmp(ptr<char>(a[0]), ptr<char>(a[1]), a[2]); }

// strtol(s, char **end, base) and its kin: the host's, which agree on the text accepted
i64 s_strtol(const u64* a, std::string&) {
    return i64(std::strtoll(ptr<char>(a[0]), ptr<char*>(a[1]), int(a[2])));
}
i64 s_strtoul(const u64* a, std::string&) {
    return i64(std::strtoull(ptr<char>(a[0]), ptr<char*>(a[1]), int(a[2])));
}
// _Stod(s, char **end, long pw): text to double, scaled by 10 to the pw
i64 s_stod(const u64* a, std::string&) {
    double r = std::strtod(ptr<char>(a[0]), ptr<char*>(a[1]));
    if (i64(a[2])) r *= std::pow(10.0, double(i64(a[2])));
    ret(r);
    return 0;
}

// The PS4's wchar_t is 16 bits.
using wchar = char16_t;
u64 wlen(const wchar* s) {
    u64 n = 0;
    while (s[n]) n++;
    return n;
}
i64 s_wcscpy(const u64* a, std::string&) {
    std::memmove(ptr<void>(a[0]), ptr<void>(a[1]), (wlen(ptr<wchar>(a[1])) + 1) * 2);
    return i64(a[0]);
}
i64 s_wcsrchr(const u64* a, std::string&) {
    const wchar* s = ptr<wchar>(a[0]);
    const wchar* found = nullptr;
    for (;; s++) {
        if (*s == wchar(a[1])) found = s;
        if (!*s) break;
    }
    return i64(reinterpret_cast<u64>(found));
}
i64 s_wcsstr(const u64* a, std::string&) {
    const wchar *s = ptr<wchar>(a[0]), *k = ptr<wchar>(a[1]);
    u64 n = wlen(k);
    for (; *s; s++)
        if (std::memcmp(s, k, n * 2) == 0) return i64(reinterpret_cast<u64>(s));
    return n == 0 ? i64(a[0]) : 0;
}
bool in_set(wchar c, const wchar* set) {
    for (; *set; set++)
        if (*set == c) return true;
    return false;
}
i64 s_wcspbrk(const u64* a, std::string&) {
    for (const wchar* s = ptr<wchar>(a[0]); *s; s++)
        if (in_set(*s, ptr<wchar>(a[1]))) return i64(reinterpret_cast<u64>(s));
    return 0;
}
i64 s_wcsspn(const u64* a, std::string&) {
    const wchar* s = ptr<wchar>(a[0]);
    u64 n = 0;
    while (s[n] && in_set(s[n], ptr<wchar>(a[1]))) n++;
    return i64(n);
}
// Wide text to a number: the ASCII digits, through the narrow conversion
std::string narrow(const wchar* s, u64 limit = 512) {
    std::string out;
    for (u64 i = 0; s[i] && s[i] < 0x80 && i < limit; i++) out += char(s[i]);
    return out;
}
i64 s_wcstoll(const u64* a, std::string&) {
    std::string t = narrow(ptr<wchar>(a[0]));
    char* end = nullptr;
    i64 r = std::strtoll(t.c_str(), &end, int(a[2]));
    if (a[1]) *ptr<u64>(a[1]) = a[0] + u64(end - t.c_str()) * 2;
    return r;
}
i64 s_wstod(const u64* a, std::string&) {
    std::string t = narrow(ptr<wchar>(a[0]));
    char* end = nullptr;
    double r = std::strtod(t.c_str(), &end);
    if (i64(a[2])) r *= std::pow(10.0, double(i64(a[2])));
    if (a[1]) *ptr<u64>(a[1]) = a[0] + u64(end - t.c_str()) * 2;
    ret(r);
    return 0;
}

// ---- the rest ----

i64 s_malloc(const u64* a, std::string&) { return i64(alloc(a[0])); }
i64 nothing(const u64*, std::string&) { return 0; }

// _Assert(const char *message, const char *function): the game's own assertion, reported and
// carried on from, as a release build's would be
i64 s_assert(const u64* a, std::string& note) {
    std::string m = a[0] ? ptr<char>(a[0]) : "", f = a[1] ? ptr<char>(a[1]) : "";
    note = std::format("\"{}\" in {}", m, f);
    std::lock_guard lock(M.log_lock);
    M.stdout_text += std::format("[assert] {} ({})\n", m, f);
    return 0;
}

// _Atomic_fetch_add_4(int *p, int v, order), _Atomic_fetch_sub_4: the old value
i64 s_fetch_add(const u64* a, std::string&) {
    return __atomic_fetch_add(ptr<std::int32_t>(a[0]), std::int32_t(a[1]), __ATOMIC_SEQ_CST);
}
i64 s_fetch_sub(const u64* a, std::string&) {
    return __atomic_fetch_sub(ptr<std::int32_t>(a[0]), std::int32_t(a[1]), __ATOMIC_SEQ_CST);
}

}  // namespace

void add_libc_standins(std::unordered_map<std::string, Standin>& t) {
    std::pair<const char*, Standin> list[] = {
        {"acosf", m_acosf}, {"asinf", m_asinf}, {"atanf", m_atanf}, {"ceilf", m_ceilf},
        {"expf", m_expf}, {"fabsf", m_fabsf}, {"floorf", m_floorf}, {"sqrtf", m_sqrtf},
        {"tanf", m_tanf}, {"tanhf", m_tanhf}, {"atan2f", m_atan2f}, {"fmodf", m_fmodf},
        {"acos", m_acos}, {"asin", m_asin}, {"atan", m_atan}, {"ceil", m_ceil},
        {"exp", m_exp}, {"exp2", m_exp2}, {"fabs", m_fabs}, {"floor", m_floor},
        {"sqrt", m_sqrt}, {"tan", m_tan}, {"atan2", m_atan2}, {"fmod", m_fmod},
        {"pow", m_pow}, {"_FSinh", m_fsinh}, {"_FCosh", m_fcosh}, {"_FLog", m_flog},
        {"_Log", m_log}, {"_Sin", m_sin}, {"_Dtest", m_dtest}, {"frexpf", m_frexpf},
        {"frexp", m_frexp}, {"ldexpf", m_ldexpf}, {"ldexp", m_ldexp}, {"modff", m_modff},
        {"modf", m_modf},

        {"strcat", s_strcat}, {"strncat", s_strncat}, {"strchr", s_strchr},
        {"strstr", s_strstr}, {"strpbrk", s_strpbrk}, {"strspn", s_strspn},
        {"strcspn", s_strcspn}, {"strcasecmp", s_strcasecmp}, {"strncasecmp", s_strncasecmp},
        {"strtol", s_strtol}, {"_Stoll", s_strtol}, {"strtoumax", s_strtoul},
        {"_Stoul", s_strtoul}, {"_Stoull", s_strtoul}, {"_Stod", s_stod},
        {"wcscpy", s_wcscpy}, {"wcsrchr", s_wcsrchr}, {"wcsstr", s_wcsstr},
        {"wcspbrk", s_wcspbrk}, {"wcsspn", s_wcsspn}, {"wcstoll", s_wcstoll},
        {"_WStod", s_wstod},

        {"free", nothing}, {"_ZdlPv", nothing}, {"_ZdaPv", nothing},
        {"_ZnwmRKSt9nothrow_t", s_malloc}, {"_Assert", s_assert},
        {"_Atomic_fetch_add_4", s_fetch_add}, {"_Atomic_fetch_sub_4", s_fetch_sub},
    };
    for (auto [name, fn] : list) t.insert_or_assign(name, fn);
}

}  // namespace mensis
