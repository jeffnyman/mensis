// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's pixel and vertex shaders, translated from GCN into GLSL for a PC's GPU.
//
// A GCN wave runs 64 lanes in step; a GPU through Vulkan runs each pixel as an invocation of
// its own. So each invocation plays one lane: the VGPRs become its own variables, and the
// SGPRs too, holding the same value in every invocation. A lane mask (exec, vcc, and any
// SGPR pair a comparison writes) is kept as the lane's own bit only, all ones or all zeros,
// in both halves of the pair: the bitwise operations the masks go through (s_and_b64,
// s_andn2_b64, s_and_saveexec_b64, ...) then give each lane its own bit of the result, as
// they would on the console. A vector instruction writes its result only where exec is set,
// as on the console; a texture is always read, and the result kept or dropped after, as
// reading one where only some of a quad's pixels go is undefined in GLSL.
//
// What a shader reads from memory through its scalar loads (constants, descriptors) is the
// same for every pixel of a draw. It is found before the draw (evaluate): the scalar
// instructions are followed with the draw's user SGPRs, and each load's dwords are read
// from the game's memory and handed to the shader in a buffer (U). Each image instruction's
// descriptors are found the same way, and become a texture and sampler bound for it.
//
// A vertex shader calls a fetch shader (s_swappc_b64), which reads the vertex's data through
// buffer descriptors and returns (s_setpc_b64); the fetch shader's instructions are put in
// the vertex shader's place, where it calls. Its loads (buffer_load_format,
// tbuffer_load_format, by the vertex's index) read a buffer's elements as the CPU unpacked
// them before the draw, one uvec4 each, with the value an index past the end reads after
// them.
//
// A compute shader runs a thread an invocation, in groups as the dispatch's. Its buffer stores
// so far: by index, of 8-bit RGBA, into a render target kept on the GPU, as a storage image
// viewed as uints, each element packed exactly as the CPU's interpreter packs it.
//
// Not yet: branches, buffer loads other than by index, other stores, LDS instructions, depth
// export, gathers, depth-compare sampling, offsets, and the rarer ALU ops. A shader with any
// of them isn't translated, and says what stopped it.

#include "translate.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <set>

namespace mensis::translate {

namespace {

enum Enc : u32 { SOP2, SOPK, SOP1, SOPC, SOPP, SMRD, VOP1, VOP2, VOPC, VOP3, VINTRP, DS, MUBUF, MTBUF, MIMG, EXP };

const char* sample_suffix[8] = {"", "_cl", "_d", "_d_cl", "_l", "_b", "_b_cl", "_lz"};

std::string hex(u32 v) { return std::format("0x{:08x}u", v); }

// An SGPR (or special register) as a GLSL variable; empty if there is none for it
std::string sreg(u32 code) {
    if (code < 104) return std::format("s{}", code);
    switch (code) {
        case 106: return "vcc_lo";
        case 107: return "vcc_hi";
        case 124: return "m0";
        case 126: return "exec_lo";
        case 127: return "exec_hi";
        default: return {};
    }
}

u32 inline_float(u32 code) {
    static const u32 v[] = {0x3f000000, 0xbf000000, 0x3f800000, 0xbf800000,
                            0x40000000, 0xc0000000, 0x40800000, 0xc0800000};
    return v[code - 240];
}

}  // namespace

// ---- decoding ----

namespace {

// Decodes from pc into p until s_endpgm (or, for a fetch shader, its s_setpc_b64)
void decode_into(Program& p, u64 pc, const shade::Memory& mem, const u32* user, u32 user_count, bool fetch) {
    for (int n = 0; n < 4096; n++) {
        if (!mem.ok(pc, 4)) {
            p.why = "code outside the game's memory";
            return;
        }
        Inst in;
        in.at = pc;
        u32 w0 = *reinterpret_cast<const u32*>(pc);
        in.w0 = w0;
        u32 size = 4;
        bool lit = false;
        if (!(w0 >> 31)) {
            u32 top7 = w0 >> 25;
            if (top7 == 0x3f) in.enc = VOP1;
            else if (top7 == 0x3e) in.enc = VOPC;
            else {
                in.enc = VOP2;
                u32 op = (w0 >> 25) & 0x3f;
                if (op == 32 || op == 33) lit = true;  // v_madmk_f32, v_madak_f32
            }
            if ((w0 & 0x1ff) == 255) lit = true;
        } else if ((w0 >> 30) == 2) {
            u32 top9 = w0 >> 23;
            if (top9 == 0x17f) {
                in.enc = SOPP;
            } else if (top9 == 0x17d) {
                in.enc = SOP1;
                lit = (w0 & 0xff) == 255;
            } else if (top9 == 0x17e) {
                in.enc = SOPC;
                lit = (w0 & 0xff) == 255 || ((w0 >> 8) & 0xff) == 255;
            } else if ((w0 >> 28) == 0xb) {
                in.enc = SOPK;
                lit = ((w0 >> 23) & 0x1f) == 21;
            } else {
                in.enc = SOP2;
                lit = (w0 & 0xff) == 255 || ((w0 >> 8) & 0xff) == 255;
            }
        } else {
            u32 enc = w0 >> 26;
            switch (enc) {
                case 0x30: case 0x31:
                    in.enc = SMRD;
                    lit = !((w0 >> 8) & 1) && (w0 & 0xff) == 255;
                    break;
                case 0x32: in.enc = VINTRP; break;
                case 0x34: in.enc = VOP3; size = 8; break;
                case 0x36: in.enc = DS; size = 8; break;
                case 0x38: in.enc = MUBUF; size = 8; break;
                case 0x3a: in.enc = MTBUF; size = 8; break;
                case 0x3c: in.enc = MIMG; size = 8; break;
                case 0x3e: in.enc = EXP; size = 8; break;
                default:
                    p.why = std::format("instruction format {:#x}", enc);
                    return;
            }
        }
        if (size == 8) {
            if (!mem.ok(pc + 4, 4)) {
                p.why = "code outside the game's memory";
                return;
            }
            in.w1 = *reinterpret_cast<const u32*>(pc + 4);
            if (in.enc == VOP3) {  // a literal can't follow VOP3 on this generation
                lit = false;
            }
        }
        if (lit) {
            if (!mem.ok(pc + size, 4)) {
                p.why = "code outside the game's memory";
                return;
            }
            in.lit = *reinterpret_cast<const u32*>(pc + size);
            size += 4;
        }
        pc += size;
        if (in.enc == SOP1) {
            u32 op = (w0 >> 8) & 0xff, src = w0 & 0xff;
            if (op == 32) {  // s_setpc_b64: a fetch shader's return
                if (fetch) return;
                p.why = "s_setpc_b64";
                return;
            }
            if (op == 33) {  // s_swappc_b64: a call to the fetch shader, whose address is user data
                if (fetch || src + 1 >= user_count) {
                    p.why = "a call to an address the shader works out";
                    return;
                }
                p.code.push_back(in);  // kept for its destination (the return address)
                decode_into(p, user[src] | (u64(user[src + 1]) << 32), mem, user, user_count, true);
                if (!p.why.empty()) return;
                continue;
            }
        }
        p.code.push_back(in);
        if (in.enc == SOPP) {
            u32 op = (w0 >> 16) & 0x7f;
            if (op == 1) {  // s_endpgm
                if (fetch) p.why = "a fetch shader that ends";
                return;
            }
            if ((op >= 2 && op <= 9) || op == 10) {
                p.why = op == 10 ? "s_barrier" : "branches";
                return;
            }
        }
        if (in.enc == DS) p.why = "LDS instructions";
        if (!p.why.empty()) return;
    }
    p.why = "no s_endpgm in 4096 instructions";
}

}  // namespace

Program decode(u64 pc, const shade::Memory& mem, const u32* user, u32 user_count) {
    Program p;
    decode_into(p, pc, mem, user, user_count, false);
    return p;
}

// ---- the draw's values ----

Values evaluate(const Program& p, const u32* user, u32 user_count, const shade::Memory& mem, bool compute) {
    Values v;
    u32 s[128] = {};
    bool known[128] = {};
    for (u32 k = 0; k < 16; k++) {
        v.u.push_back(k < user_count ? user[k] : 0);
        if (k < user_count) s[k] = user[k], known[k] = true;
    }
    if (user_count < 104 && !compute) known[user_count] = true;  // the primitive mask, 0 here
    auto forget = [&](u32 code, u32 n) {
        for (u32 k = 0; k < n; k++)
            if (code + k < 128) known[code + k] = false;
    };
    auto get = [&](u32 code, u32 lit, bool& ok) -> u32 {
        if (code < 128) {
            ok = ok && known[code];
            return s[code];
        }
        if (code <= 192) return code - 128;
        if (code <= 208) return u32(192 - int(code));
        if (code >= 240 && code <= 247) return inline_float(code);
        if (code == 255) return lit;
        ok = false;
        return 0;
    };
    for (const Inst& in : p.code) {
        u32 w0 = in.w0;
        switch (in.enc) {
            case SOP1: {
                u32 op = (w0 >> 8) & 0xff, dst = (w0 >> 16) & 0x7f, src = w0 & 0xff;
                if (op == 3 || op == 4) {  // s_mov_b32, s_mov_b64
                    for (u32 k = 0; k < (op == 4 ? 2u : 1u); k++) {
                        bool ok = true;
                        u32 x = get(src + (src < 128 ? k : 0), in.lit, ok);
                        if (op == 4 && src >= 128 && k == 1) x = (src >= 193 && src <= 208) ? ~0u : 0;
                        if (dst + k < 128) s[dst + k] = x, known[dst + k] = ok;
                    }
                } else {
                    forget(dst, 2);
                    if (op >= 36 && op <= 43) forget(126, 2);
                }
                break;
            }
            case SOPK: {
                u32 op = (w0 >> 23) & 0x1f, dst = (w0 >> 16) & 0x7f;
                if (op == 0) {
                    if (dst < 128) s[dst] = u32(std::int32_t(std::int16_t(w0 & 0xffff))), known[dst] = true;
                } else if (op != 3 && !(op >= 4 && op <= 14)) {
                    forget(dst, 1);
                }
                break;
            }
            case SOP2: forget((w0 >> 16) & 0x7f, 2); break;
            case SMRD: {
                u32 op = (w0 >> 22) & 0x1f, dst = (w0 >> 15) & 0x7f, base = ((w0 >> 9) & 0x3f) * 2;
                bool imm = (w0 >> 8) & 1;
                u32 off = w0 & 0xff;
                if (op > 12 || (op & 7) > 4) {
                    v.why = std::format("scalar memory op {}", op);
                    return v;
                }
                u32 count = 1u << (op & 7);
                bool ok = true;
                u64 offset = imm ? u64(off) * 4 : get(off, in.lit, ok);
                u64 at;
                if (op >= 8) {  // through a buffer descriptor
                    u32 desc[4];
                    for (u32 k = 0; k < 4; k++) desc[k] = get(base + k, 0, ok);
                    at = (desc[0] | (u64(desc[1] & 0xfff) << 32)) + offset;
                } else {
                    at = ((s[base] | (u64(s[base + 1]) << 32)) & ~3ull) + offset;
                    ok = ok && known[base] && known[base + 1];
                }
                if (!ok) {
                    v.why = "a scalar load from an address the shader works out";
                    return v;
                }
                for (u32 k = 0; k < count; k++) {
                    u32 x = 0;
                    if (mem.ok(at + 4 * k, 4)) std::memcpy(&x, reinterpret_cast<const void*>(at + 4 * k), 4);
                    v.u.push_back(x);
                    if (dst + k < 128) s[dst + k] = x, known[dst + k] = true;
                }
                break;
            }
            case VOPC: forget(106, 2); break;
            case VOP2: {
                u32 op = (w0 >> 25) & 0x3f;
                if (op >= 0x25 && op <= 0x2a) forget(106, 2);
                if (op == 1) forget((w0 >> 17) & 0xff, 1);  // v_readlane_b32
                break;
            }
            case VOP3: {
                u32 op = (w0 >> 17) & 0x1ff;
                if (op < 0x100 || op == 0x101 || (op >= 0x125 && op <= 0x12a) || op == 0x16d || op == 0x182)
                    forget(op >= 0x125 && op != 0x182 ? (w0 >> 8) & 0x7f : w0 & 0xff, 2);
                break;
            }
            case VOP1:
                if (((w0 >> 9) & 0xff) == 2) forget((w0 >> 17) & 0xff, 1);  // v_readfirstlane_b32
                break;
            case MIMG: {
                u32 srsrc = ((in.w1 >> 16) & 0x1f) * 4, ssamp = ((in.w1 >> 21) & 0x1f) * 4;
                u32 op = (w0 >> 18) & 0x7f;
                bool sampled = op >= 32;
                Values::Image im;
                bool ok = true;
                for (u32 k = 0; k < 8; k++) im.t[k] = get(srsrc + k, 0, ok);
                if (sampled)
                    for (u32 k = 0; k < 4; k++) im.s[k] = get(ssamp + k, 0, ok);
                if (!ok) {
                    v.why = "an image descriptor the shader works out";
                    return v;
                }
                v.images.push_back(im);
                break;
            }
            case MUBUF: case MTBUF: {
                bool typed = in.enc == MTBUF;
                u32 op = typed ? (w0 >> 16) & 7 : (w0 >> 18) & 0x7f;
                if (op > 3 && (typed || op > 7)) {
                    v.why = std::format("{} op {}", typed ? "MTBUF" : "MUBUF", op);
                    return v;
                }
                Values::Buffer b;
                bool ok = true;
                u32 srsrc = ((in.w1 >> 16) & 0x1f) * 4;
                for (u32 k = 0; k < 4; k++) b.v[k] = get(srsrc + k, 0, ok);
                b.offset = (w0 & 0xfff) + get(in.w1 >> 24, 0, ok);
                b.dfmt = typed ? (w0 >> 19) & 0xf : (b.v[3] >> 15) & 15;
                b.nfmt = typed ? (w0 >> 23) & 7 : (b.v[3] >> 12) & 7;
                if (!ok) {
                    v.why = "a buffer descriptor the shader works out";
                    return v;
                }
                (op > 3 ? v.stores : v.buffers).push_back(b);
                break;
            }
            default: break;
        }
    }
    return v;
}

std::string PsState::key() const {
    std::string k = std::format("{}:{:x}:{:x}:{:x}", user_count, ena, addr, col_format);
    for (u32 c : input_cntl) k += std::format(":{:x}", c);
    for (size_t i = 0; i < image_types.size(); i++) k += std::format(":t{}{}", image_types[i], unnormalized[i] ? "u" : "");
    return k;
}

// ---- GLSL ----

namespace {

struct Writer {
    const PsState& st;
    bool vertex = false;  // a vertex shader (else a pixel shader)
    const CsState* cs = nullptr;  // a compute shader's
    u32 store = 0;  // the next buffer store's
    std::string body, why;
    u32 slot = 16;  // the next scalar load's place in U
    u32 image = 0;  // the next image instruction's binding
    u32 buffer = 0;  // the next buffer load's
    u32 attrs = 0;  // how many attributes are read
    std::string interp[32];  // each attribute's interpolation qualifier
    bool outputs[8] = {};
    std::set<u32> vgprs;

    explicit Writer(const PsState& s) : st(s) {}

    void fail(const std::string& w) {
        if (why.empty()) why = w;
    }
    void line(const std::string& s) { body += "    " + s + "\n"; }

    // A 32-bit source operand, as a uint expression
    std::string src(u32 code, u32 lit) {
        if (code >= 256) {
            vgprs.insert(code - 256);
            return std::format("v{}", code - 256);
        }
        if (code <= 192 && code >= 128) return std::format("{}u", code - 128);
        if (code >= 193 && code <= 208) return hex(u32(192 - int(code)));
        if (code >= 240 && code <= 247) return hex(inline_float(code));
        switch (code) {
            case 251: return "(vcc_lo == 0u ? 1u : 0u)";
            case 252: return "(exec_lo == 0u ? 1u : 0u)";
            case 253: return "(scc ? 1u : 0u)";
            case 255: return hex(lit);
            default: break;
        }
        std::string r = sreg(code);
        if (r.empty()) fail(std::format("operand {}", code));
        return r.empty() ? "0u" : r;
    }
    // The high half of a 64-bit scalar operand
    std::string src_hi(u32 code, u32 lit) {
        if (code < 128) return src(code + 1, lit);
        if (code >= 193 && code <= 208) return "0xffffffffu";
        return "0u";
    }
    std::string fsrc(u32 code, u32 lit, bool abs, bool neg) {
        std::string x = "uintBitsToFloat(" + src(code, lit) + ")";
        if (abs) x = "abs(" + x + ")";
        if (neg) x = "(-" + x + ")";
        return x;
    }
    std::string sdst(u32 code) {
        std::string r = sreg(code);
        if (r.empty()) fail(std::format("destination {}", code));
        return r.empty() ? "dummy" : r;
    }
    std::string vdst(u32 v) {
        vgprs.insert(v);
        return std::format("v{}", v);
    }
    // A vector result, written where exec is set
    void vwrite(u32 v, const std::string& expr) {
        line(std::format("{{ uint r = {}; if (exec_lo != 0u) {} = r; }}", expr, vdst(v)));
    }
    // A lane mask, from a condition, into an SGPR pair (inactive lanes' bits are 0)
    void mask_write(u32 code, const std::string& cond) {
        std::string lo = sdst(code), hi = sdst(code + 1);
        line(std::format("{{ uint m = (exec_lo != 0u && ({})) ? 0xffffffffu : 0u; {} = m; {} = m; }}", cond, lo, hi));
    }

    void scalar(const Inst& in);
    void vector(const Inst& in);
    void interp_op(const Inst& in);
    void image_op(const Inst& in);
    void buffer_op(const Inst& in);
    void export_op(const Inst& in);
};

void Writer::scalar(const Inst& in) {
    u32 w0 = in.w0;
    switch (in.enc) {
        case SOPP: {
            u32 op = (w0 >> 16) & 0x7f;
            if (op == 1 || op == 0 || op == 12 || (op >= 13 && op <= 17) || op == 19 || op == 20 || op == 21) return;
            fail(std::format("SOPP op {}", op));
            return;
        }
        case SOPK: {
            u32 op = (w0 >> 23) & 0x1f, dst = (w0 >> 16) & 0x7f;
            u32 imm = u32(std::int32_t(std::int16_t(w0 & 0xffff)));
            if (op == 0) line(std::format("{} = {};", sdst(dst), hex(imm)));
            else if (op == 15) line(std::format("{{ int a = int({0}), b = int({1}); int r = a + b; {0} = uint(r); scc = (a >= 0) == (b >= 0) && (r >= 0) != (a >= 0); }}", sdst(dst), hex(imm)));
            else if (op == 16) line(std::format("{0} = uint(int({0}) * int({1}));", sdst(dst), hex(imm)));
            else fail(std::format("SOPK op {}", op));
            return;
        }
        case SOP1: {
            u32 op = (w0 >> 8) & 0xff, dst = (w0 >> 16) & 0x7f, s0 = w0 & 0xff;
            std::string a = src(s0, in.lit);
            switch (op) {
                case 3: line(std::format("{} = {};", sdst(dst), a)); return;
                case 33: return;  // s_swappc_b64: the fetch shader follows, put in its place
                case 4: line(std::format("{{ uint lo = {}, hi = {}; {} = lo; {} = hi; }}", a, src_hi(s0, in.lit), sdst(dst), sdst(dst + 1))); return;
                case 7: line(std::format("{} = ~{}; scc = {} != 0u;", sdst(dst), a, sdst(dst))); return;
                case 8: line(std::format("{{ uint lo = ~{}, hi = ~{}; {} = lo; {} = hi; scc = (lo | hi) != 0u; }}", a, src_hi(s0, in.lit), sdst(dst), sdst(dst + 1))); return;
                case 10:
                    // whole quad mode: a lane is on if any lane of its 2x2 quad is. Each
                    // invocation's quad has at least one pixel covered, so from exec it is all.
                    if (s0 != 126) fail("s_wqm_b64 of something other than exec");
                    line(std::format("{} = 0xffffffffu; {} = 0xffffffffu; scc = true;", sdst(dst), sdst(dst + 1)));
                    return;
                default: break;
            }
            if (op >= 36 && op <= 43) {  // s_<op>_saveexec_b64: the old exec out, exec = s0 <op> exec
                static const char* f[] = {"a & e", "a | e", "a ^ e", "a & ~e", "a | ~e", "~(a & e)", "~(a | e)", "~(a ^ e)"};
                line(std::format("{{ uint a = {}, e = exec_lo; {} = exec_lo; {} = exec_hi; exec_lo = {}; exec_hi = exec_lo; scc = exec_lo != 0u; }}",
                                 a, sdst(dst), sdst(dst + 1), f[op - 36]));
                return;
            }
            fail(std::format("SOP1 op {}", op));
            return;
        }
        case SOP2: {
            u32 op = (w0 >> 23) & 0x7f, dst = (w0 >> 16) & 0x7f, s0 = w0 & 0xff, s1 = (w0 >> 8) & 0xff;
            std::string a = src(s0, in.lit), b = src(s1, in.lit), d = sdst(dst);
            static const char* bit[] = {"a & b", "a | b", "a ^ b", "a & ~b", "a | ~b", "~(a & b)", "~(a | b)", "~(a ^ b)"};
            if (op >= 14 && op <= 29) {
                const char* f = bit[(op - 14) / 2];
                if (op & 1)
                    line(std::format("{{ uint a = {}, b = {}; uint lo = {}; a = {}; b = {}; uint hi = {}; {} = lo; {} = hi; scc = (lo | hi) != 0u; }}",
                                     a, b, f, src_hi(s0, in.lit), src_hi(s1, in.lit), f, d, sdst(dst + 1)));
                else
                    line(std::format("{{ uint a = {}, b = {}; {} = {}; scc = {} != 0u; }}", a, b, d, f, d));
                return;
            }
            switch (op) {
                case 0: line(std::format("{{ uint a = {}, b = {}; {} = a + b; scc = a + b < a; }}", a, b, d)); return;
                case 1: line(std::format("{{ uint a = {}, b = {}; {} = a - b; scc = b > a; }}", a, b, d)); return;
                case 2: case 3:
                    line(std::format("{{ int a = int({}), b = int({}); int r = a {} b; {} = uint(r); scc = {}; }}", a, b, op == 2 ? "+" : "-", d,
                                     op == 2 ? "(a >= 0) == (b >= 0) && (r >= 0) != (a >= 0)" : "(a >= 0) != (b >= 0) && (r >= 0) != (a >= 0)"));
                    return;
                case 6: line(std::format("{{ int a = int({}), b = int({}); scc = a < b; {} = uint(min(a, b)); }}", a, b, d)); return;
                case 7: line(std::format("{{ uint a = {}, b = {}; scc = a < b; {} = min(a, b); }}", a, b, d)); return;
                case 8: line(std::format("{{ int a = int({}), b = int({}); scc = a > b; {} = uint(max(a, b)); }}", a, b, d)); return;
                case 9: line(std::format("{{ uint a = {}, b = {}; scc = a > b; {} = max(a, b); }}", a, b, d)); return;
                case 10: line(std::format("{} = scc ? {} : {};", d, a, b)); return;
                case 11: line(std::format("{{ uint lo = scc ? {} : {}, hi = scc ? {} : {}; {} = lo; {} = hi; }}", a, b, src_hi(s0, in.lit), src_hi(s1, in.lit), d, sdst(dst + 1))); return;
                case 30: line(std::format("{{ uint r = {} << ({} & 31u); {} = r; scc = r != 0u; }}", a, b, d)); return;
                case 32: line(std::format("{{ uint r = {} >> ({} & 31u); {} = r; scc = r != 0u; }}", a, b, d)); return;
                case 34: line(std::format("{{ uint r = uint(int({}) >> int({} & 31u)); {} = r; scc = r != 0u; }}", a, b, d)); return;
                case 36: line(std::format("{} = ((1u << ({} & 31u)) - 1u) << ({} & 31u);", d, a, b)); return;
                case 38: line(std::format("{} = uint(int({}) * int({}));", d, a, b)); return;
                case 39: line(std::format("{{ uint a = {}, b = {}; uint o = b & 31u, w = (b >> 16) & 127u; uint r = w == 0u ? 0u : w >= 32u ? a >> o : (a >> o) & ((1u << w) - 1u); {} = r; scc = r != 0u; }}", a, b, d)); return;
                default: fail(std::format("SOP2 op {}", op)); return;
            }
        }
        case SOPC: {
            u32 op = (w0 >> 16) & 0x7f;
            std::string a = src(w0 & 0xff, in.lit), b = src((w0 >> 8) & 0xff, in.lit);
            static const char* cmp[] = {"==", "!=", ">", ">=", "<", "<="};
            if (op <= 5) line(std::format("scc = int({}) {} int({});", a, cmp[op], b));
            else if (op <= 11) line(std::format("scc = {} {} {};", a, cmp[op - 6], b));
            else if (op == 12 || op == 13) line(std::format("scc = (({} >> ({} & 31u)) & 1u) == {}u;", a, b, op - 12));
            else fail(std::format("SOPC op {}", op));
            return;
        }
        case SMRD: {
            u32 op = (w0 >> 22) & 0x1f, dst = (w0 >> 15) & 0x7f;
            u32 count = 1u << (op & 7);
            for (u32 k = 0; k < count; k++) line(std::format("{} = U[{}];", sdst(dst + k), slot + k));
            slot += count;
            return;
        }
        default: break;
    }
}

// A 32-bit vector op's result (VOP3 numbering), from its sources as floats (fa, fb, fc) or
// uints (a, b, c) and what its destination held (d); empty if not done here
std::string alu(u32 op, bool& is_float) {
    is_float = true;
    switch (op) {
        case 0x103: return "fa + fb";
        case 0x104: return "fa - fb";
        case 0x105: return "fb - fa";
        case 0x106: return "((fa == 0.0 || fb == 0.0) ? 0.0 : fa * fb) + uintBitsToFloat(d)";
        case 0x107: return "(fa == 0.0 || fb == 0.0) ? 0.0 : fa * fb";
        case 0x108: return "fa * fb";
        case 0x10f: return "min(fa, fb)";
        case 0x110: return "max(fa, fb)";
        case 0x11f: return "fa * fb + uintBitsToFloat(d)";
        case 0x12b: return "ldexp(fa, int(b))";
        case 0x140: return "((fa == 0.0 || fb == 0.0) ? 0.0 : fa * fb) + fc";
        case 0x141: return "fa * fb + fc";
        case 0x14b: case 0x16f: return "fma(fa, fb, fc)";
        case 0x151: return "min(min(fa, fb), fc)";
        case 0x154: return "max(max(fa, fb), fc)";
        case 0x157: return "max(min(fa, fb), min(max(fa, fb), fc))";
        case 0x185: return "float(int(a))";
        case 0x186: return "float(a)";
        case 0x18b: return "h2f(a & 0xffffu)";
        case 0x191: case 0x192: case 0x193: case 0x194: return std::format("float((a >> {}u) & 255u)", 8 * (op - 0x191));
        case 0x1a0: return "min(fa - floor(fa), uintBitsToFloat(0x3f7fffffu))";
        case 0x1a1: return "trunc(fa)";
        case 0x1a2: return "ceil(fa)";
        case 0x1a3: return "roundEven(fa)";
        case 0x1a4: return "floor(fa)";
        case 0x1a5: return "exp2(fa)";
        case 0x1a7: return "log2(fa)";
        case 0x1aa: return "1.0 / fa";
        case 0x1ae: return "inversesqrt(fa)";
        case 0x1b3: return "sqrt(fa)";
        case 0x1b5: return "sin(fa * 6.28318530718)";
        case 0x1b6: return "cos(fa * 6.28318530718)";
        default: break;
    }
    is_float = false;
    switch (op) {
        case 0x109: return "uint(bitfieldExtract(int(a), 0, 24) * bitfieldExtract(int(b), 0, 24))";
        case 0x10b: return "(a & 0xffffffu) * (b & 0xffffffu)";
        case 0x10d: return "fa < fb ? a : b";
        case 0x10e: return "fa >= fb ? a : b";
        case 0x111: return "uint(min(int(a), int(b)))";
        case 0x112: return "uint(max(int(a), int(b)))";
        case 0x113: return "min(a, b)";
        case 0x114: return "max(a, b)";
        case 0x115: return "a >> (b & 31u)";
        case 0x116: return "b >> (a & 31u)";
        case 0x117: return "uint(int(a) >> int(b & 31u))";
        case 0x118: return "uint(int(b) >> int(a & 31u))";
        case 0x119: return "a << (b & 31u)";
        case 0x11a: return "b << (a & 31u)";
        case 0x11b: return "a & b";
        case 0x11c: return "a | b";
        case 0x11d: return "a ^ b";
        case 0x11e: return "((1u << (a & 31u)) - 1u) << (b & 31u)";
        case 0x122: return "uint(bitCount(a)) + b";
        case 0x12f: return "f2h_rtz(fa) | (f2h_rtz(fb) << 16)";
        case 0x143: return "(a & 0xffffffu) * (b & 0xffffffu) + c";
        case 0x148: return "(c & 31u) == 0u ? 0u : bitfieldExtract(a, int(b & 31u), int(min(c & 31u, 32u - (b & 31u))))";
        case 0x149: return "(c & 31u) == 0u ? 0u : uint(bitfieldExtract(int(a >> (b & 31u)), 0, int(c & 31u)))";
        case 0x14a: return "(a & b) | (~a & c)";
        case 0x153: return "min(min(a, b), c)";
        case 0x156: return "max(max(a, b), c)";
        case 0x169: return "a * b";
        case 0x16b: return "uint(int(a) * int(b))";
        case 0x181: return "a";
        case 0x187: return "isnan(fa) || fa <= 0.0 ? 0u : fa >= 4294967295.0 ? 0xffffffffu : uint(fa)";
        case 0x188: return "isnan(fa) ? 0u : fa <= -2147483648.0 ? 0x80000000u : fa >= 2147483647.0 ? 0x7fffffffu : uint(int(fa))";
        case 0x18a: return "f2h(fa)";
        case 0x18c: return "uint(int(floor(fa + 0.5)))";
        case 0x18d: return "uint(int(floor(fa)))";
        case 0x1b7: return "~a";
        case 0x1b8: return "bitfieldReverse(a)";
        default: return {};
    }
}

bool float_op_float_sources(u32 op) {  // whether abs/neg apply (the float ops)
    bool f;
    (void)alu(op, f);
    return f || op == 0x10d || op == 0x10e || op == 0x12f || op == 0x187 || op == 0x188 || op == 0x18a ||
           op == 0x18c || op == 0x18d;
}

void Writer::vector(const Inst& in) {
    u32 w0 = in.w0, w1 = in.w1;
    u32 op = 0, vd = 0, sd = 106, srcs[3] = {0, 0, 106}, abs = 0, neg = 0, omod = 0;
    bool clamp = false;
    u32 lit = in.lit;
    switch (in.enc) {
        case VOP1:
            op = 0x180 + ((w0 >> 9) & 0xff);
            vd = (w0 >> 17) & 0xff;
            srcs[0] = w0 & 0x1ff;
            break;
        case VOPC:
            op = (w0 >> 17) & 0xff;
            srcs[0] = w0 & 0x1ff;
            srcs[1] = 256 + ((w0 >> 9) & 0xff);
            break;
        case VOP2: {
            u32 o = (w0 >> 25) & 0x3f;
            op = 0x100 + o;
            vd = (w0 >> 17) & 0xff;
            srcs[0] = w0 & 0x1ff;
            srcs[1] = 256 + ((w0 >> 9) & 0xff);
            if (o == 32) {  // v_madmk_f32: src0 * K + vsrc1
                op = 0x141;
                srcs[2] = srcs[1];
                srcs[1] = 255;
            } else if (o == 33) {  // v_madak_f32: src0 * vsrc1 + K
                op = 0x141;
                srcs[2] = 255;
            }
            break;
        }
        default: {  // VOP3
            op = (w0 >> 17) & 0x1ff;
            srcs[0] = w1 & 0x1ff;
            srcs[1] = (w1 >> 9) & 0x1ff;
            srcs[2] = (w1 >> 18) & 0x1ff;
            neg = w1 >> 29;
            omod = (w1 >> 27) & 3;
            vd = w0 & 0xff;
            bool b = (op >= 0x125 && op <= 0x12a) || op == 0x16d || op == 0x16e || op == 0x176 || op == 0x177;
            if (b) {
                sd = (w0 >> 8) & 0x7f;
            } else {
                abs = (w0 >> 8) & 7;
                clamp = (w0 >> 11) & 1;
                sd = vd;
            }
        }
    }
    if (op == 0x180) return;  // v_nop
    if (op < 0x100) {  // comparisons, into a lane mask
        u32 group = op >> 4, cond = op & 15;
        bool x = group & 1;
        std::string c;
        if (group <= 1) {  // f32
            std::string a = fsrc(srcs[0], lit, abs & 1, neg & 1), b = fsrc(srcs[1], lit, abs & 2, neg & 2);
            static const char* f[16] = {"false", "{0} < {1}", "{0} == {1}", "{0} <= {1}", "{0} > {1}", "!isnan({0}) && !isnan({1}) && {0} != {1}",
                                        "{0} >= {1}", "!isnan({0}) && !isnan({1})", "isnan({0}) || isnan({1})", "!({0} >= {1})",
                                        "isnan({0}) || isnan({1}) || {0} == {1}", "!({0} > {1})", "!({0} <= {1})", "!({0} == {1})",
                                        "!({0} < {1})", "true"};
            c = std::vformat(f[cond], std::make_format_args(a, b));
        } else if ((group == 8 || group == 9 || group == 12 || group == 13) && cond < 8) {
            std::string a = src(srcs[0], lit), b = src(srcs[1], lit);
            if (group < 12) a = "int(" + a + ")", b = "int(" + b + ")";
            static const char* f[8] = {"false", "{0} < {1}", "{0} == {1}", "{0} <= {1}", "{0} > {1}", "{0} != {1}", "{0} >= {1}", "true"};
            c = std::vformat(f[cond], std::make_format_args(a, b));
        } else {
            fail(std::format("vector compare {:#x}", op));
            return;
        }
        mask_write(sd, c);
        if (x) line(std::format("exec_lo = {}; exec_hi = exec_lo;", sreg(sd)));
        return;
    }
    if (op == 0x100) {  // v_cndmask_b32: src2 is the lane mask
        std::string m = src(srcs[2], lit);
        std::string a = src(srcs[0], lit), b = src(srcs[1], lit);
        if (abs || neg) {
            a = "floatBitsToUint(" + fsrc(srcs[0], lit, abs & 1, neg & 1) + ")";
            b = "floatBitsToUint(" + fsrc(srcs[1], lit, abs & 2, neg & 2) + ")";
        }
        vwrite(vd, std::format("{} != 0u ? {} : {}", m, b, a));
        return;
    }
    if (op >= 0x125 && op <= 0x12a) {  // with a carry, into a lane mask
        std::string a = src(srcs[0], lit), b = src(srcs[1], lit), ci = op >= 0x128 ? "(" + src(srcs[2], lit) + " != 0u ? 1u : 0u)" : "0u";
        static const char* r[] = {"a + b", "a - b", "b - a", "a + b + ci", "a - b - ci", "b - a - ci"};
        static const char* carry[] = {"a + b < a", "b > a", "a > b", "uint64_t(a) + uint64_t(b) + uint64_t(ci) > 0xffffffffUL",
                                      "uint64_t(b) + uint64_t(ci) > uint64_t(a)", "uint64_t(a) + uint64_t(ci) > uint64_t(b)"};
        u32 k = op - 0x125;
        line(std::format("{{ uint a = {}, b = {}, ci = {}; uint r = {}; bool cy = {}; if (exec_lo != 0u) {} = r; uint m = (exec_lo != 0u && cy) ? 0xffffffffu : 0u; {} = m; {} = m; }}",
                         a, b, ci, r[k], carry[k], vdst(vd), sdst(sd), sdst(sd + 1)));
        return;
    }
    bool is_float;
    std::string e = alu(op, is_float);
    if (e.empty()) {
        fail(std::format("vector op {:#x}", op));
        return;
    }
    bool fl = float_op_float_sources(op);
    std::string decl;
    for (int k = 0; k < 3; k++) {
        bool used = e.find(k == 0 ? "a" : k == 1 ? "b" : "c") != std::string::npos;
        if (!used) continue;
        std::string name(1, char('a' + k));
        std::string uexpr = src(srcs[k], lit);
        if (fl && ((abs >> k) & 1 || (neg >> k) & 1))
            uexpr = "floatBitsToUint(" + fsrc(srcs[k], lit, (abs >> k) & 1, (neg >> k) & 1) + ")";
        decl += std::format("uint {0} = {1}; float f{0} = uintBitsToFloat({0}); ", name, uexpr);
    }
    if (e.find('d') != std::string::npos) decl += std::format("uint d = {}; ", vdst(vd));
    std::string r = is_float ? "(" + e + ")" : e;
    if (is_float) {
        if (omod == 1) r = "(" + r + " * 2.0)";
        if (omod == 2) r = "(" + r + " * 4.0)";
        if (omod == 3) r = "(" + r + " * 0.5)";
        if (clamp) r = "clamp(" + r + ", 0.0, 1.0)";
        r = "floatBitsToUint(" + r + ")";
    }
    line(std::format("{{ {}uint r = {}; if (exec_lo != 0u) {} = r; }}", decl, r, vdst(vd)));
}

// The PS input VGPRs: which bit of SPI_PS_INPUT_ADDR each VGPR comes from
int input_bit(u32 addr, u32 vgpr) {
    static const int sizes[16] = {2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    u32 vg = 0;
    for (int bit = 0; bit < 16; bit++) {
        if (!((addr >> bit) & 1)) continue;
        if (vgpr >= vg && vgpr < vg + u32(sizes[bit])) return bit;
        vg += u32(sizes[bit]);
    }
    return -1;
}

void Writer::interp_op(const Inst& in) {
    if (vertex) {
        fail("v_interp in a vertex shader");
        return;
    }
    u32 w0 = in.w0;
    u32 op = (w0 >> 16) & 3, attr = (w0 >> 10) & 0x3f, chan = (w0 >> 8) & 3, s = w0 & 0xff, dst = (w0 >> 18) & 0xff;
    if (attr >= 32) {
        fail("attribute past 31");
        return;
    }
    std::string q;
    if (op == 2) {  // v_interp_mov_f32: a vertex's value as it is
        if (s != 2) fail("v_interp_mov_f32 of the second or third vertex");
        q = "flat";
    } else {
        u32 addr = st.addr ? st.addr : st.ena;
        int bit = input_bit(addr, s);
        static const char* modes[7] = {"sample", "", "centroid", nullptr, "noperspective sample", "noperspective", "noperspective centroid"};
        if (bit < 0 || bit > 6 || bit == 3) {
            fail("v_interp with barycentrics from an unknown input");
            return;
        }
        q = *modes[bit] ? modes[bit] : "smooth";
        if ((st.input_cntl[attr] >> 10) & 1) q = "flat";
    }
    if (!interp[attr].empty() && interp[attr] != q && !(interp[attr] == "flat" && (st.input_cntl[attr] >> 10) & 1))
        fail(std::format("attribute {} interpolated two ways", attr));
    interp[attr] = q;
    attrs = std::max(attrs, attr + 1);
    static const char xyzw[] = "xyzw";
    vwrite(dst, std::format("floatBitsToUint(a{}.{})", attr, xyzw[chan]));
}

void Writer::image_op(const Inst& in) {
    u32 w0 = in.w0, w1 = in.w1;
    u32 op = (w0 >> 18) & 0x7f, dmask = (w0 >> 8) & 0xf;
    bool da = (w0 >> 14) & 1;
    (void)da;
    if (vertex) {
        fail("images in a vertex shader");
        return;
    }
    u32 vaddr = w1 & 0xff, vdata = (w1 >> 8) & 0xff;
    if (image >= st.image_types.size()) {
        fail("image instructions not found before the draw");
        return;
    }
    u32 type = st.image_types[image];
    bool unnorm = st.unnormalized[image];
    std::string t = std::format("t{}", image);
    image++;
    if (type != 8 && type != 9 && type != 10 && type != 12 && type != 13) {
        fail(std::format("images of type {}", type));
        return;
    }
    auto va = [&](u32 k) {
        vgprs.insert((vaddr + k) & 255);
        return std::format("v{}", (vaddr + k) & 255);
    };
    auto fva = [&](u32 k) { return "uintBitsToFloat(" + va(k) + ")"; };
    std::string result;
    if (op <= 1) {  // image_load(_mip): integer coordinates; the level isn't used (one level)
        std::string c;
        switch (type) {
            case 8: c = std::format("ivec2(int({}), 0)", va(0)); break;
            case 9: c = std::format("ivec2(int({}), int({}))", va(0), va(1)); break;
            case 12: c = std::format("ivec3(int({}), 0, int({}))", va(0), va(1)); break;
            default: c = std::format("ivec3(int({}), int({}), int({}))", va(0), va(1), va(2)); break;
        }
        line(std::format("ivec{0} c{1} = {2}; ivec{0} n{1} = textureSize({3}, 0);", type == 8 || type == 9 ? 2 : 3, image, c, t));
        result = std::format("all(greaterThanEqual(c{0}, ivec{1}(0))) && all(lessThan(c{0}, n{0})) ? texelFetch({2}, c{0}, 0) : vec4(0.0, 0.0, 0.0, 1.0)",
                             image, type == 8 || type == 9 ? 2 : 3, t);
    } else if (op >= 32 && op < 64) {
        u32 k = (op - 32) & 31, m = k & 7;
        if (k & 8) {
            fail("depth-compare sampling");
            return;
        }
        if (k & 16) {
            fail("sampling with offsets");
            return;
        }
        if (m == 2 || m == 3) {
            fail("sampling with given derivatives");
            return;
        }
        u32 at = 0;
        std::string bias;
        if (m == 5 || m == 6) bias = fva(at++);
        u32 n = type == 8 ? 1 : type == 10 || type == 13 ? 3 : 2;
        std::string x = fva(at), y = n > 1 ? fva(at + 1) : "", z = n > 2 ? fva(at + 2) : "";
        std::string lod = m == 4 ? fva(at + n) : "";
        std::string size = std::format("vec3(textureSize({}, 0){})", t, type == 8 || type == 9 ? ", 1" : "");
        if (unnorm) {
            line(std::format("vec3 sz{} = {};", image, size));
            x = std::format("({} / sz{}.x)", x, image);
            if (type == 9 || type == 13 || type == 10) y = std::format("({} / sz{}.y)", y, image);
            if (type == 10) z = std::format("({} / sz{}.z)", z, image);
        }
        std::string c;
        switch (type) {
            case 8: c = std::format("vec2({}, 0.5)", x); break;
            case 9: c = std::format("vec2({}, {})", x, y); break;
            case 12: c = std::format("vec3({}, 0.5, {})", x, y); break;
            default: c = std::format("vec3({}, {}, {})", x, y, z); break;
        }
        if (m == 4) result = std::format("textureLod({}, {}, {})", t, c, lod);
        else if (m == 7) result = std::format("textureLod({}, {}, 0.0)", t, c);
        else if (!bias.empty()) result = std::format("texture({}, {}, {})", t, c, bias);
        else result = std::format("texture({}, {})", t, c);
        (void)sample_suffix;
    } else {
        fail(std::format("image op {}", op));
        return;
    }
    line(std::format("vec4 i{} = {};", image, result));
    u32 r = 0;
    static const char xyzw[] = "xyzw";
    for (u32 c = 0; c < 4; c++)
        if ((dmask >> c) & 1) vwrite((vdata + r++) & 255, std::format("floatBitsToUint(i{}.{})", image, xyzw[c]));
}

// A format load by index (idxen): element vN of the buffer as the CPU unpacked it
void Writer::buffer_op(const Inst& in) {
    u32 w0 = in.w0, w1 = in.w1;
    bool typed = in.enc == MTBUF;
    u32 op = typed ? (w0 >> 16) & 7 : (w0 >> 18) & 0x7f;
    bool offen = (w0 >> 12) & 1, idxen = (w0 >> 13) & 1, addr64 = (w0 >> 15) & 1;
    u32 vaddr = w1 & 0xff, vdata = (w1 >> 8) & 0xff;
    if (!typed && op >= 4 && op <= 7 && cs) {  // buffer_store_format, by index
        if (!idxen || offen || addr64) {
            fail("buffer stores other than by index");
            return;
        }
        vgprs.insert(vaddr);
        u32 slot = cs->store_slot + 3 * store++;
        std::string c[4] = {"0u", "0u", "0u", "0u"};
        for (u32 k = 0; k <= op - 4; k++) c[k] = "unorm8(" + src(256 + ((vdata + k) & 255), 0) + ")";
        line(std::format("if (exec_lo != 0u && v{0} < U[{1}]) {{ uint p = v{0} + U[{2}]; imageStore(S{3}, ivec2(int(p % U[{4}]), int(p / U[{4}])), "
                         "uvec4({5} | ({6} << 8) | ({7} << 16) | ({8} << 24))); }}",
                         vaddr, slot + 2, slot, store - 1, slot + 1, c[0], c[1], c[2], c[3]));
        return;
    }
    if (op > 3) {
        fail(std::format("{} op {}", typed ? "MTBUF" : "MUBUF", op));
        return;
    }
    if (!idxen || offen || addr64 || (!typed && ((w0 >> 16) & 1))) {
        fail("buffer loads other than by index");
        return;
    }
    if ((st.add_tid >> buffer) & 1) {
        fail("buffers indexed by lane");
        return;
    }
    vgprs.insert(vaddr);
    u32 k = buffer++;
    line(std::format("uvec4 b{0} = B{0}[min(v{1}, uint(B{0}.length()) - 1u)];", k, vaddr));
    static const char xyzw[] = "xyzw";
    for (u32 c = 0; c <= op; c++) vwrite((vdata + c) & 255, std::format("b{}.{}", k, xyzw[c]));
}

void Writer::export_op(const Inst& in) {
    u32 w0 = in.w0, w1 = in.w1;
    u32 en = w0 & 0xf, target = (w0 >> 4) & 0x3f;
    bool compr = (w0 >> 10) & 1;
    if (vertex) {  // a position (pos0-3: 12-15) or a parameter (param0-31: 32-63)
        if (compr) {
            fail("compressed exports from a vertex shader");
            return;
        }
        if (target >= 13 && target <= 15) return;  // pos1-3 (clip distances...): not used here
        if (target != 12 && (target < 32 || target > 63)) {
            fail(std::format("vertex export to {}", target));
            return;
        }
        std::string c[4];
        for (u32 k = 0; k < 4; k++) {
            if ((en >> k) & 1) {
                u32 r = (w1 >> (8 * k)) & 0xff;
                vgprs.insert(r);
                c[k] = std::format("uintBitsToFloat(v{})", r);
            } else {
                c[k] = k == 3 ? "1.0" : "0.0";
            }
        }
        std::string to = target == 12 ? "pos" : std::format("P{}", target - 32);
        line(std::format("if (exec_lo != 0u) {} = vec4({}, {}, {}, {});", to, c[0], c[1], c[2], c[3]));
        return;
    }
    if (target == 9) {  // null: the pixel is kept, nothing written
        line("if (exec_lo != 0u) exported = true;");
        return;
    }
    if (target > 7) {
        fail(target == 8 ? "depth export" : std::format("export to {}", target));
        return;
    }
    u32 cf = (st.col_format >> (4 * target)) & 15;
    if (cf == 0) return;
    auto reg = [&](u32 k) -> std::string {
        if (!((en >> k) & 1)) return "0u";
        u32 r = compr ? (k < 2 ? w1 & 0xff : (w1 >> 8) & 0xff) : (w1 >> (8 * k)) & 0xff;
        vgprs.insert(r);
        return std::format("v{}", r);
    };
    std::string v;
    if (compr) {
        std::string lo = (en & 1) ? reg(0) : reg(1), hi = (en & 4) ? reg(2) : reg(3);
        switch (cf) {
            case 4: v = std::format("vec4(h2f({0} & 0xffffu), h2f({0} >> 16), h2f({1} & 0xffffu), h2f({1} >> 16))", lo, hi); break;
            case 5: v = std::format("vec4(unpackUnorm2x16({}), unpackUnorm2x16({}))", lo, hi); break;
            case 6: v = std::format("max(vec4(unpackSnorm2x16({}), unpackSnorm2x16({})), vec4(-1.0))", lo, hi); break;
            default: fail(std::format("integer exports (format {})", cf)); return;
        }
    } else {
        auto F = [&](u32 k) { return "uintBitsToFloat(" + reg(k) + ")"; };
        switch (cf) {
            case 1: v = std::format("vec4({}, 0.0, 0.0, 1.0)", F(0)); break;
            case 2: v = std::format("vec4({}, {}, 0.0, 1.0)", F(0), F(1)); break;
            case 3: v = std::format("vec4({}, 0.0, 0.0, {})", F(0), F(3)); break;
            default: v = std::format("vec4({}, {}, {}, {})", F(0), F(1), F(2), F(3)); break;
        }
    }
    outputs[target] = true;
    line(std::format("if (exec_lo != 0u) {{ o{} = {}; exported = true; }}", target, v));
}

const char* HELPERS = R"(
// a half's bits as a float, and a float to a half (rounded to nearest even, or toward zero), bit
// by bit as the CPU's interpreter does them
float h2f(uint h) {
    uint s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1fu, m = h & 0x3ffu;
    if (e == 0u) { float v = float(m) * (1.0 / 16777216.0); return s != 0u ? -v : v; }
    if (e == 31u) return uintBitsToFloat(s | 0x7f800000u | (m << 13));
    return uintBitsToFloat(s | ((e + 112u) << 23) | (m << 13));
}
uint f2h_mode(float x, bool toward_zero) {
    uint b = floatBitsToUint(x), s = (b >> 16) & 0x8000u, e = (b >> 23) & 0xffu, m = b & 0x7fffffu;
    if (e == 255u) return s | 0x7c00u | (m != 0u ? 0x200u : 0u);
    int he = int(e) - 112;
    if (he >= 31) return s | (toward_zero ? 0x7bffu : 0x7c00u);
    uint q, shift;
    if (he <= 0) {
        if (he < -10) return s;
        m |= 0x800000u;
        shift = uint(14 - he);
    } else {
        shift = 13u;
    }
    q = m >> shift;
    if (!toward_zero) {
        uint rem = m & ((1u << shift) - 1u), halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (q & 1u) != 0u)) q++;
    }
    if (he <= 0) return s | q;
    if (q == 0x400u) { q = 0u; he++; }
    if (he >= 31) return s | (toward_zero ? 0x7bffu : 0x7c00u);
    return s | (uint(he) << 10) | q;
}
uint f2h(float x) { return f2h_mode(x, false); }
uint f2h_rtz(float x) { return f2h_mode(x, true); }
)";

}  // namespace

namespace {

// The instructions, into w.body; false (and w.why) if one can't be translated
bool translate_body(Writer& w, const Program& p) {
    for (const Inst& in : p.code) {
        switch (in.enc) {
            case SOP2: case SOPK: case SOP1: case SOPC: case SOPP: case SMRD: w.scalar(in); break;
            case VOP1: case VOP2: case VOPC: case VOP3: w.vector(in); break;
            case VINTRP: w.interp_op(in); break;
            case MIMG: w.image_op(in); break;
            case MUBUF: case MTBUF: w.buffer_op(in); break;
            case EXP: w.export_op(in); break;
            default: w.fail("instruction format"); break;
        }
        if (!w.why.empty()) return false;
    }
    return true;
}

// The registers' declarations: the SGPRs (the user ones from U), then the VGPRs from init
std::string registers(const Writer& w, u32 user_count, const std::string& exec, std::map<u32, std::string>& init,
                      const std::map<u32, std::string>& sinit = {}) {
    std::string s = std::format("    uint exec_lo = {}, exec_hi = exec_lo;\n", exec);
    s += "    uint vcc_lo = 0u, vcc_hi = 0u, m0 = 0u, dummy = 0u;\n    bool scc = false, exported = false;\n";
    for (u32 k = 0; k < 104; k++)
        s += std::format("    uint s{} = {};\n", k,
                         k < user_count && k < 16 ? std::format("U[{}]", k) : sinit.count(k) ? sinit.at(k) : "0u");
    u32 top = w.vgprs.empty() ? 0 : *w.vgprs.rbegin() + 1;
    for (u32 k = 0; k < top; k++) s += std::format("    uint v{} = {};\n", k, init.count(k) ? init[k] : "0u");
    return s;
}

}  // namespace

Glsl vertex_shader(const Program& p, const VsState& vs) {
    Glsl g;
    if (!p.why.empty()) {
        g.why = p.why;
        return g;
    }
    PsState none;
    none.add_tid = vs.add_tid;
    Writer w(none);
    w.vertex = true;
    if (!translate_body(w, p)) {
        g.why = w.why;
        return g;
    }
    std::string s = "#version 450\n";
    s += "layout(set = 0, binding = 16, std430) readonly buffer Values { uint U[]; };\n";
    for (u32 k = 0; k < w.buffer; k++)
        s += std::format("layout(set = 0, binding = {}, std430) readonly buffer Buffer{} {{ uvec4 B{}[]; }};\n", 17 + k, k, k);
    s += "layout(push_constant) uniform Viewport { vec4 xy; vec2 z; } X;\n";
    for (u32 a = 0; a < vs.attributes; a++) s += std::format("layout(location = {}) out vec4 a{};\n", a, a);
    s += HELPERS;
    s += "\nvoid main() {\n";
    std::map<u32, std::string> init = {{0, "uint(gl_VertexIndex)"}};  // v0: the vertex's index
    s += registers(w, vs.user_count, "0xffffffffu", init);
    s += "    vec4 pos = vec4(0.0, 0.0, 0.0, 1.0);\n";
    for (u32 k = 0; k < 32; k++) s += std::format("    vec4 P{} = vec4(0.0);\n", k);
    s += w.body;
    // the console's viewport, as Vulkan's: x' = X.xy.x * x + X.xy.y * w, and so on (vkdraw)
    s += "    gl_Position = vec4(X.xy.x * pos.x + X.xy.y * pos.w, X.xy.z * pos.y + X.xy.w * pos.w, X.z.x * pos.z + X.z.y * pos.w, pos.w);\n";
    // the pixel shader's attributes, as SPI_PS_INPUT_CNTL picks them from the parameters
    for (u32 a = 0; a < vs.attributes; a++) {
        u32 cntl = vs.input_cntl[a], offset = cntl & 0x3f;
        if (offset & 0x20) {
            u32 dv = (cntl >> 8) & 3;
            float rgb = dv >= 2 ? 1.0f : 0.0f, alpha = dv == 1 || dv == 3 ? 1.0f : 0.0f;
            s += std::format("    a{} = vec4({:.1f}, {:.1f}, {:.1f}, {:.1f});\n", a, rgb, rgb, rgb, alpha);
        } else if (offset < 32) {
            s += std::format("    a{} = P{};\n", a, offset);
        } else {
            s += std::format("    a{} = vec4(0.0);\n", a);
        }
    }
    s += "}\n";
    if (s.find("uint64_t") != std::string::npos)
        s.insert(s.find('\n') + 1, "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n");
    g.text = s;
    g.buffers = w.buffer;
    return g;
}

std::string rect_geometry_shader(u32 attributes) {
    std::string s = "#version 450\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 4) out;\n";
    for (u32 a = 0; a < attributes; a++) s += std::format("layout(location = {}) in vec4 i{}[];\nlayout(location = {}) out vec4 a{};\n", a, a, a, a);
    s += "void main() {\n    for (int k = 0; k < 3; k++) {\n        gl_Position = gl_in[k].gl_Position;\n";
    for (u32 a = 0; a < attributes; a++) s += std::format("        a{0} = i{0}[k];\n", a);
    s += "        EmitVertex();\n    }\n";
    s += "    // the fourth corner: b + c - a\n    gl_Position = gl_in[1].gl_Position + gl_in[2].gl_Position - gl_in[0].gl_Position;\n";
    for (u32 a = 0; a < attributes; a++) s += std::format("    a{0} = i{0}[1] + i{0}[2] - i{0}[0];\n", a);
    s += "    EmitVertex();\n}\n";
    return s;
}

Glsl pixel_shader(const Program& p, const PsState& st) {
    Glsl g;
    if (!p.why.empty()) {
        g.why = p.why;
        return g;
    }
    Writer w(st);
    if (!translate_body(w, p)) {
        g.why = w.why;
        return g;
    }
    std::string s = "#version 450\n";
    s += "layout(set = 0, binding = 0, std430) readonly buffer Values { uint U[]; };\n";
    for (u32 k = 0; k < w.image; k++) {
        u32 type = st.image_types[k];
        const char* sampler = type == 10 ? "sampler3D" : type == 12 || type == 13 ? "sampler2DArray" : "sampler2D";
        s += std::format("layout(set = 0, binding = {}) uniform {} t{};\n", k + 1, sampler, k);
    }
    for (u32 a = 0; a < w.attrs; a++)
        s += std::format("layout(location = {}) {} in vec4 a{};\n", a, w.interp[a].empty() ? "smooth" : w.interp[a], a);
    for (u32 k = 0; k < 8; k++)
        if (w.outputs[k]) s += std::format("layout(location = {}) out vec4 o{};\n", k, k);
    s += HELPERS;
    s += "\nvoid main() {\n";
    // the input VGPRs, as SPI_PS_INPUT_ADDR lays them out and SPI_PS_INPUT_ENA fills them
    std::map<u32, std::string> init;
    {
        static const int sizes[16] = {2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        u32 addr = st.addr ? st.addr : st.ena, vg = 0;
        for (int bit = 0; bit < 16; bit++) {
            if (!((addr >> bit) & 1)) continue;
            if ((st.ena >> bit) & 1) {
                switch (bit) {
                    case 8: init[vg] = "floatBitsToUint(gl_FragCoord.x)"; break;
                    case 9: init[vg] = "floatBitsToUint(gl_FragCoord.y)"; break;
                    case 10: init[vg] = "floatBitsToUint(gl_FragCoord.z)"; break;
                    case 11: init[vg] = "floatBitsToUint(gl_FragCoord.w)"; break;
                    case 12: init[vg] = "floatBitsToUint(gl_FrontFacing ? 1.0 : -1.0)"; break;
                    case 14: init[vg] = "1u"; break;
                    case 15: init[vg] = "uint(gl_FragCoord.x) | (uint(gl_FragCoord.y) << 16)"; break;
                    default: break;  // barycentrics: only v_interp reads them, and it is replaced
                }
            }
            vg += u32(sizes[bit]);
        }
    }
    s += registers(w, st.user_count, "gl_HelperInvocation ? 0u : 0xffffffffu", init);
    s += w.body;
    s += "    if (!exported) discard;\n}\n";
    if (s.find("uint64_t") != std::string::npos)
        s.insert(s.find('\n') + 1, "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n");
    g.text = s;
    g.attributes = w.attrs;
    return g;
}

Glsl compute_shader(const Program& p, const CsState& cs) {
    Glsl g;
    if (!p.why.empty()) {
        g.why = p.why;
        return g;
    }
    PsState none;
    Writer w(none);
    w.cs = &cs;
    if (!translate_body(w, p)) {
        g.why = w.why;
        return g;
    }
    if (w.image || w.buffer) {
        g.why = "images or buffer loads in a compute shader";
        return g;
    }
    std::string s = "#version 450\n";
    s += std::format("layout(local_size_x = {}, local_size_y = {}, local_size_z = {}) in;\n", cs.threads[0], cs.threads[1],
                     cs.threads[2]);
    s += "layout(set = 0, binding = 0, std430) readonly buffer Values { uint U[]; };\n";
    for (u32 k = 0; k < w.store; k++) s += std::format("layout(set = 0, binding = {}, r32ui) uniform writeonly uimage2D S{};\n", 32 + k, k);
    s += HELPERS;
    // a float into 8 unsigned bits, as the CPU's interpreter packs it (shade.cpp)
    s += "uint unorm8(uint x) { float f = uintBitsToFloat(x); f = isnan(f) ? 0.0 : clamp(f, 0.0, 1.0); return uint(roundEven(f * 255.0)); }\n";
    s += "\nvoid main() {\n";
    // v0-v2: the thread in its group; after the user SGPRs, the group's number (x, y, z, as
    // enabled), then the wave's in the group
    std::map<u32, std::string> init = {{0, "gl_LocalInvocationID.x"}, {1, "gl_LocalInvocationID.y"}, {2, "gl_LocalInvocationID.z"}};
    std::map<u32, std::string> sinit;
    u32 next = std::min(cs.user_count, 16u);
    static const char xyz[] = "xyz";
    for (int d = 0; d < 3; d++)
        if ((cs.rsrc2 >> (7 + d)) & 1) sinit[next++] = std::format("gl_WorkGroupID.{}", xyz[d]);
    if ((cs.rsrc2 >> 10) & 1) sinit[next++] = "gl_LocalInvocationIndex / 64u";
    for (u32 k = 0; k < 3; k++) w.vgprs.insert(k);
    s += registers(w, cs.user_count, "0xffffffffu", init, sinit);
    s += w.body;
    s += "}\n";
    if (s.find("uint64_t") != std::string::npos)
        s.insert(s.find('\n') + 1, "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n");
    g.text = s;
    g.buffers = w.store;
    return g;
}

std::string pass_vertex_shader(u32 attributes) {
    std::string s = "#version 450\nlayout(location = 0) in vec4 position;\n";
    for (u32 a = 0; a < attributes; a++) s += std::format("layout(location = {}) in vec4 i{};\n", a + 1, a);
    for (u32 a = 0; a < attributes; a++) s += std::format("layout(location = {}) out vec4 a{};\n", a, a);
    s += "void main() {\n    gl_Position = position;\n";
    for (u32 a = 0; a < attributes; a++) s += std::format("    a{} = i{};\n", a, a);
    s += "}\n";
    return s;
}

}  // namespace mensis::translate
