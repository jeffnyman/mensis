// SPDX-License-Identifier: GPL-3.0-or-later
//
// A disassembler for the PS4 GPU's shaders. The GPU is AMD's GCN, second generation (Sea
// Islands, "CI"); its instruction formats and opcodes are in AMD's "Sea Islands Series
// Instruction Set Architecture" reference. The text follows the usual GCN assembly syntax
// (as LLVM's and AMD's tools write it), closely enough to read; it isn't meant for assembling.
//
// An instruction is one or two dwords, and the first says which format it is: scalar (s_)
// instructions run once for the whole wavefront of 64 threads, on SGPRs (s0, s1...); vector
// (v_) instructions run for each thread, on its VGPRs (v0, v1...). Memory, image, local-share
// and export instructions are the two-dword formats. One more dword, a literal, follows an
// instruction whose operand says so (255).
//
// A shader compiled for the PS4 ends with a footer, "OrbShdr", which gives its length.

#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "loader.hpp"

namespace mensis {
namespace {

// ---- opcode names, by format ----

const char* const SOP2[] = {
    "s_add_u32", "s_sub_u32", "s_add_i32", "s_sub_i32", "s_addc_u32", "s_subb_u32", "s_min_i32",
    "s_min_u32", "s_max_i32", "s_max_u32", "s_cselect_b32", "s_cselect_b64", nullptr, nullptr,
    "s_and_b32", "s_and_b64", "s_or_b32", "s_or_b64", "s_xor_b32", "s_xor_b64", "s_andn2_b32",
    "s_andn2_b64", "s_orn2_b32", "s_orn2_b64", "s_nand_b32", "s_nand_b64", "s_nor_b32",
    "s_nor_b64", "s_xnor_b32", "s_xnor_b64", "s_lshl_b32", "s_lshl_b64", "s_lshr_b32",
    "s_lshr_b64", "s_ashr_i32", "s_ashr_i64", "s_bfm_b32", "s_bfm_b64", "s_mul_i32", "s_bfe_u32",
    "s_bfe_i32", "s_bfe_u64", "s_bfe_i64", "s_cbranch_g_fork", "s_absdiff_i32"};

const char* const SOPK[] = {
    "s_movk_i32", nullptr, "s_cmovk_i32", "s_cmpk_eq_i32", "s_cmpk_lg_i32", "s_cmpk_gt_i32",
    "s_cmpk_ge_i32", "s_cmpk_lt_i32", "s_cmpk_le_i32", "s_cmpk_eq_u32", "s_cmpk_lg_u32",
    "s_cmpk_gt_u32", "s_cmpk_ge_u32", "s_cmpk_lt_u32", "s_cmpk_le_u32", "s_addk_i32",
    "s_mulk_i32", "s_cbranch_i_fork", "s_getreg_b32", "s_setreg_b32", nullptr,
    "s_setreg_imm32_b32"};

const char* const SOP1[] = {
    nullptr, nullptr, nullptr, "s_mov_b32", "s_mov_b64", "s_cmov_b32", "s_cmov_b64", "s_not_b32",
    "s_not_b64", "s_wqm_b32", "s_wqm_b64", "s_brev_b32", "s_brev_b64", "s_bcnt0_i32_b32",
    "s_bcnt0_i32_b64", "s_bcnt1_i32_b32", "s_bcnt1_i32_b64", "s_ff0_i32_b32", "s_ff0_i32_b64",
    "s_ff1_i32_b32", "s_ff1_i32_b64", "s_flbit_i32_b32", "s_flbit_i32_b64", "s_flbit_i32",
    "s_flbit_i32_i64", "s_sext_i32_i8", "s_sext_i32_i16", "s_bitset0_b32", "s_bitset0_b64",
    "s_bitset1_b32", "s_bitset1_b64", "s_getpc_b64", "s_setpc_b64", "s_swappc_b64", "s_rfe_b64",
    nullptr, "s_and_saveexec_b64", "s_or_saveexec_b64", "s_xor_saveexec_b64",
    "s_andn2_saveexec_b64", "s_orn2_saveexec_b64", "s_nand_saveexec_b64", "s_nor_saveexec_b64",
    "s_xnor_saveexec_b64", "s_quadmask_b32", "s_quadmask_b64", "s_movrels_b32", "s_movrels_b64",
    "s_movreld_b32", "s_movreld_b64", "s_cbranch_join", nullptr, "s_abs_i32", "s_mov_fed_b32"};

const char* const SOPC[] = {
    "s_cmp_eq_i32", "s_cmp_lg_i32", "s_cmp_gt_i32", "s_cmp_ge_i32", "s_cmp_lt_i32",
    "s_cmp_le_i32", "s_cmp_eq_u32", "s_cmp_lg_u32", "s_cmp_gt_u32", "s_cmp_ge_u32",
    "s_cmp_lt_u32", "s_cmp_le_u32", "s_bitcmp0_b32", "s_bitcmp1_b32", "s_bitcmp0_b64",
    "s_bitcmp1_b64", "s_setvskip"};

const char* const SOPP[] = {
    "s_nop", "s_endpgm", "s_branch", nullptr, "s_cbranch_scc0", "s_cbranch_scc1",
    "s_cbranch_vccz", "s_cbranch_vccnz", "s_cbranch_execz", "s_cbranch_execnz", "s_barrier",
    "s_setkill", "s_waitcnt", "s_sethalt", "s_sleep", "s_setprio", "s_sendmsg", "s_sendmsghalt",
    "s_trap", "s_icache_inv", "s_incperflevel", "s_decperflevel", "s_ttracedata",
    "s_cbranch_cdbgsys", "s_cbranch_cdbguser", "s_cbranch_cdbgsys_or_user",
    "s_cbranch_cdbgsys_and_user"};

const char* const SMRD[] = {
    "s_load_dword", "s_load_dwordx2", "s_load_dwordx4", "s_load_dwordx8", "s_load_dwordx16",
    nullptr, nullptr, nullptr, "s_buffer_load_dword", "s_buffer_load_dwordx2",
    "s_buffer_load_dwordx4", "s_buffer_load_dwordx8", "s_buffer_load_dwordx16"};

const char* const VOP2[] = {
    "v_cndmask_b32", "v_readlane_b32", "v_writelane_b32", "v_add_f32", "v_sub_f32",
    "v_subrev_f32", "v_mac_legacy_f32", "v_mul_legacy_f32", "v_mul_f32", "v_mul_i32_i24",
    "v_mul_hi_i32_i24", "v_mul_u32_u24", "v_mul_hi_u32_u24", "v_min_legacy_f32",
    "v_max_legacy_f32", "v_min_f32", "v_max_f32", "v_min_i32", "v_max_i32", "v_min_u32",
    "v_max_u32", "v_lshr_b32", "v_lshrrev_b32", "v_ashr_i32", "v_ashrrev_i32", "v_lshl_b32",
    "v_lshlrev_b32", "v_and_b32", "v_or_b32", "v_xor_b32", "v_bfm_b32", "v_mac_f32",
    "v_madmk_f32", "v_madak_f32", "v_bcnt_u32_b32", "v_mbcnt_lo_u32_b32", "v_mbcnt_hi_u32_b32",
    "v_add_i32", "v_sub_i32", "v_subrev_i32", "v_addc_u32", "v_subb_u32", "v_subbrev_u32",
    "v_ldexp_f32", "v_cvt_pkaccum_u8_f32", "v_cvt_pknorm_i16_f32", "v_cvt_pknorm_u16_f32",
    "v_cvt_pkrtz_f16_f32", "v_cvt_pk_u16_u32", "v_cvt_pk_i16_i32"};

const char* const VOP1[] = {
    "v_nop", "v_mov_b32", "v_readfirstlane_b32", "v_cvt_i32_f64", "v_cvt_f64_i32",
    "v_cvt_f32_i32", "v_cvt_f32_u32", "v_cvt_u32_f32", "v_cvt_i32_f32", "v_mov_fed_b32",
    "v_cvt_f16_f32", "v_cvt_f32_f16", "v_cvt_rpi_i32_f32", "v_cvt_flr_i32_f32",
    "v_cvt_off_f32_i4", "v_cvt_f32_f64", "v_cvt_f64_f32", "v_cvt_f32_ubyte0", "v_cvt_f32_ubyte1",
    "v_cvt_f32_ubyte2", "v_cvt_f32_ubyte3", "v_cvt_u32_f64", "v_cvt_f64_u32", "v_trunc_f64",
    "v_ceil_f64", "v_rndne_f64", "v_floor_f64", nullptr, nullptr, nullptr, nullptr, nullptr,
    "v_fract_f32", "v_trunc_f32", "v_ceil_f32", "v_rndne_f32", "v_floor_f32", "v_exp_f32",
    "v_log_clamp_f32", "v_log_f32", "v_rcp_clamp_f32", "v_rcp_legacy_f32", "v_rcp_f32",
    "v_rcp_iflag_f32", "v_rsq_clamp_f32", "v_rsq_legacy_f32", "v_rsq_f32", "v_rcp_f64",
    "v_rcp_clamp_f64", "v_rsq_f64", "v_rsq_clamp_f64", "v_sqrt_f32", "v_sqrt_f64", "v_sin_f32",
    "v_cos_f32", "v_not_b32", "v_bfrev_b32", "v_ffbh_u32", "v_ffbl_b32", "v_ffbh_i32",
    "v_frexp_exp_i32_f64", "v_frexp_mant_f64", "v_fract_f64", "v_frexp_exp_i32_f32",
    "v_frexp_mant_f32", "v_clrexcp", "v_movreld_b32", "v_movrels_b32", "v_movrelsd_b32",
    "v_log_legacy_f32", "v_exp_legacy_f32"};

// The VOP3-only operations, from 0x140
const char* const VOP3[] = {
    "v_mad_legacy_f32", "v_mad_f32", "v_mad_i32_i24", "v_mad_u32_u24", "v_cubeid_f32",
    "v_cubesc_f32", "v_cubetc_f32", "v_cubema_f32", "v_bfe_u32", "v_bfe_i32", "v_bfi_b32",
    "v_fma_f32", "v_fma_f64", "v_lerp_u8", "v_alignbit_b32", "v_alignbyte_b32", "v_mullit_f32",
    "v_min3_f32", "v_min3_i32", "v_min3_u32", "v_max3_f32", "v_max3_i32", "v_max3_u32",
    "v_med3_f32", "v_med3_i32", "v_med3_u32", "v_sad_u8", "v_sad_hi_u8", "v_sad_u16",
    "v_sad_u32", "v_cvt_pk_u8_f32", "v_div_fixup_f32", "v_div_fixup_f64", "v_lshl_b64",
    "v_lshr_b64", "v_ashr_i64", "v_add_f64", "v_mul_f64", "v_min_f64", "v_max_f64",
    "v_ldexp_f64", "v_mul_lo_u32", "v_mul_hi_u32", "v_mul_lo_i32", "v_mul_hi_i32",
    "v_div_scale_f32", "v_div_scale_f64", "v_div_fmas_f32", "v_div_fmas_f64", "v_msad_u8",
    "v_qsad_pk_u16_u8", "v_mqsad_pk_u16_u8", "v_trig_preop_f64", "v_mqsad_u32_u8",
    "v_mad_u64_u32", "v_mad_i64_i32"};

template <size_t N>
const char* lookup(const char* const (&table)[N], u32 op) {
    return op < N ? table[op] : nullptr;
}

// VOPC (and VOP3's first 256): the comparisons
std::string vopc_name(u32 op) {
    static const char* f[] = {"f", "lt", "eq", "le", "gt", "lg", "ge", "o",
                              "u", "nge", "nlg", "ngt", "nle", "neq", "nlt", "tru"};
    static const char* i[] = {"f", "lt", "eq", "le", "gt", "ne", "ge", "t"};
    u32 group = op >> 4, low = op & 15;
    if (group < 8) {
        static const char* kind[] = {"v_cmp_", "v_cmpx_", "v_cmp_", "v_cmpx_",
                                     "v_cmps_", "v_cmpsx_", "v_cmps_", "v_cmpsx_"};
        return std::format("{}{}_{}", kind[group], f[low], group & 2 ? "f64" : "f32");
    }
    const char* kind = group & 1 ? "v_cmpx_" : "v_cmp_";
    static const char* type[] = {"i32", "i32", "i64", "i64", "u32", "u32", "u64", "u64"};
    if (low < 8) return std::format("{}{}_{}", kind, i[low], type[group - 8]);
    if (low == 8 && group < 12) return std::format("{}class_{}", kind, group < 10 ? "f32" : "f64");
    return {};
}

const std::map<u32, const char*>& ds_names() {
    static const std::map<u32, const char*> m = {
        {0, "ds_add_u32"}, {1, "ds_sub_u32"}, {2, "ds_rsub_u32"}, {3, "ds_inc_u32"},
        {4, "ds_dec_u32"}, {5, "ds_min_i32"}, {6, "ds_max_i32"}, {7, "ds_min_u32"},
        {8, "ds_max_u32"}, {9, "ds_and_b32"}, {10, "ds_or_b32"}, {11, "ds_xor_b32"},
        {12, "ds_mskor_b32"}, {13, "ds_write_b32"}, {14, "ds_write2_b32"},
        {15, "ds_write2st64_b32"}, {16, "ds_cmpst_b32"}, {17, "ds_cmpst_f32"},
        {18, "ds_min_f32"}, {19, "ds_max_f32"}, {20, "ds_nop"}, {25, "ds_gws_init"},
        {26, "ds_gws_sema_v"}, {27, "ds_gws_sema_br"}, {28, "ds_gws_sema_p"},
        {29, "ds_gws_barrier"}, {30, "ds_write_b8"}, {31, "ds_write_b16"},
        {32, "ds_add_rtn_u32"}, {33, "ds_sub_rtn_u32"}, {34, "ds_rsub_rtn_u32"},
        {35, "ds_inc_rtn_u32"}, {36, "ds_dec_rtn_u32"}, {37, "ds_min_rtn_i32"},
        {38, "ds_max_rtn_i32"}, {39, "ds_min_rtn_u32"}, {40, "ds_max_rtn_u32"},
        {41, "ds_and_rtn_b32"}, {42, "ds_or_rtn_b32"}, {43, "ds_xor_rtn_b32"},
        {44, "ds_mskor_rtn_b32"}, {45, "ds_wrxchg_rtn_b32"}, {46, "ds_wrxchg2_rtn_b32"},
        {47, "ds_wrxchg2st64_rtn_b32"}, {48, "ds_cmpst_rtn_b32"}, {49, "ds_cmpst_rtn_f32"},
        {50, "ds_min_rtn_f32"}, {51, "ds_max_rtn_f32"}, {52, "ds_wrap_rtn_b32"},
        {53, "ds_swizzle_b32"}, {54, "ds_read_b32"}, {55, "ds_read2_b32"},
        {56, "ds_read2st64_b32"}, {57, "ds_read_i8"}, {58, "ds_read_u8"}, {59, "ds_read_i16"},
        {60, "ds_read_u16"}, {61, "ds_consume"}, {62, "ds_append"}, {63, "ds_ordered_count"},
        {64, "ds_add_u64"}, {65, "ds_sub_u64"}, {66, "ds_rsub_u64"}, {67, "ds_inc_u64"},
        {68, "ds_dec_u64"}, {69, "ds_min_i64"}, {70, "ds_max_i64"}, {71, "ds_min_u64"},
        {72, "ds_max_u64"}, {73, "ds_and_b64"}, {74, "ds_or_b64"}, {75, "ds_xor_b64"},
        {76, "ds_mskor_b64"}, {77, "ds_write_b64"}, {78, "ds_write2_b64"},
        {79, "ds_write2st64_b64"}, {80, "ds_cmpst_b64"}, {81, "ds_cmpst_f64"},
        {82, "ds_min_f64"}, {83, "ds_max_f64"}, {96, "ds_add_rtn_u64"}, {97, "ds_sub_rtn_u64"},
        {98, "ds_rsub_rtn_u64"}, {99, "ds_inc_rtn_u64"}, {100, "ds_dec_rtn_u64"},
        {101, "ds_min_rtn_i64"}, {102, "ds_max_rtn_i64"}, {103, "ds_min_rtn_u64"},
        {104, "ds_max_rtn_u64"}, {105, "ds_and_rtn_b64"}, {106, "ds_or_rtn_b64"},
        {107, "ds_xor_rtn_b64"}, {108, "ds_mskor_rtn_b64"}, {109, "ds_wrxchg_rtn_b64"},
        {110, "ds_wrxchg2_rtn_b64"}, {111, "ds_wrxchg2st64_rtn_b64"}, {112, "ds_cmpst_rtn_b64"},
        {113, "ds_cmpst_rtn_f64"}, {114, "ds_min_rtn_f64"}, {115, "ds_max_rtn_f64"},
        {118, "ds_read_b64"}, {119, "ds_read2_b64"}, {120, "ds_read2st64_b64"},
        {222, "ds_write_b96"}, {223, "ds_write_b128"}, {254, "ds_read_b96"},
        {255, "ds_read_b128"}};
    return m;
}

// MUBUF: the name, and how many data registers
struct BufOp { const char* name; int data; };
const std::map<u32, BufOp>& mubuf_ops() {
    static const std::map<u32, BufOp> m = [] {
        std::map<u32, BufOp> t = {
            {0, {"buffer_load_format_x", 1}}, {1, {"buffer_load_format_xy", 2}},
            {2, {"buffer_load_format_xyz", 3}}, {3, {"buffer_load_format_xyzw", 4}},
            {4, {"buffer_store_format_x", 1}}, {5, {"buffer_store_format_xy", 2}},
            {6, {"buffer_store_format_xyz", 3}}, {7, {"buffer_store_format_xyzw", 4}},
            {8, {"buffer_load_ubyte", 1}}, {9, {"buffer_load_sbyte", 1}},
            {10, {"buffer_load_ushort", 1}}, {11, {"buffer_load_sshort", 1}},
            {12, {"buffer_load_dword", 1}}, {13, {"buffer_load_dwordx2", 2}},
            {14, {"buffer_load_dwordx4", 4}}, {15, {"buffer_load_dwordx3", 3}},
            {24, {"buffer_store_byte", 1}}, {26, {"buffer_store_short", 1}},
            {28, {"buffer_store_dword", 1}}, {29, {"buffer_store_dwordx2", 2}},
            {30, {"buffer_store_dwordx4", 4}}, {31, {"buffer_store_dwordx3", 3}},
            {112, {"buffer_wbinvl1_vol", 0}}, {113, {"buffer_wbinvl1", 0}}};
        static const char* atomics[] = {"swap", "cmpswap", "add", "sub", "rsub", "smin", "umin",
                                        "smax", "umax", "and", "or", "xor", "inc", "dec",
                                        "fcmpswap", "fmin", "fmax"};
        static std::vector<std::string> names;  // kept for the table's pointers
        names.reserve(34);
        for (u32 k = 0; k < 17; k++) {
            int n = std::string_view(atomics[k]).find("cmpswap") != std::string_view::npos ? 2 : 1;
            names.push_back(std::format("buffer_atomic_{}", atomics[k]));
            t[48 + k] = {names.back().c_str(), n};
            names.push_back(std::format("buffer_atomic_{}_x2", atomics[k]));
            t[80 + k] = {names.back().c_str(), n * 2};
        }
        return t;
    }();
    return m;
}

const char* const MTBUF[] = {
    "tbuffer_load_format_x", "tbuffer_load_format_xy", "tbuffer_load_format_xyz",
    "tbuffer_load_format_xyzw", "tbuffer_store_format_x", "tbuffer_store_format_xy",
    "tbuffer_store_format_xyz", "tbuffer_store_format_xyzw"};

std::string mimg_name(u32 op) {
    static const char* sample[] = {"", "_cl", "_d", "_d_cl", "_l", "_b", "_b_cl", "_lz",
                                   "_c", "_c_cl", "_c_d", "_c_d_cl", "_c_l", "_c_b", "_c_b_cl", "_c_lz"};
    static const std::map<u32, const char*> other = {
        {0, "image_load"}, {1, "image_load_mip"}, {2, "image_load_pck"},
        {3, "image_load_pck_sgn"}, {4, "image_load_mip_pck"}, {5, "image_load_mip_pck_sgn"},
        {8, "image_store"}, {9, "image_store_mip"}, {10, "image_store_pck"},
        {11, "image_store_mip_pck"}, {14, "image_get_resinfo"}, {15, "image_atomic_swap"},
        {16, "image_atomic_cmpswap"}, {17, "image_atomic_add"}, {18, "image_atomic_sub"},
        {19, "image_atomic_rsub"}, {20, "image_atomic_smin"}, {21, "image_atomic_umin"},
        {22, "image_atomic_smax"}, {23, "image_atomic_umax"}, {24, "image_atomic_and"},
        {25, "image_atomic_or"}, {26, "image_atomic_xor"}, {27, "image_atomic_inc"},
        {28, "image_atomic_dec"}, {29, "image_atomic_fcmpswap"}, {30, "image_atomic_fmin"},
        {31, "image_atomic_fmax"}, {96, "image_get_lod"}, {104, "image_sample_cd"},
        {105, "image_sample_cd_cl"}, {106, "image_sample_c_cd"}, {107, "image_sample_c_cd_cl"},
        {108, "image_sample_cd_o"}, {109, "image_sample_cd_cl_o"}, {110, "image_sample_c_cd_o"},
        {111, "image_sample_c_cd_cl_o"}};
    if (op >= 32 && op < 96) {
        u32 k = (op - 32) & 31;
        return std::format("{}{}{}", op < 64 ? "image_sample" : "image_gather4", sample[k & 15],
                           k >= 16 ? "_o" : "");
    }
    auto it = other.find(op);
    return it == other.end() ? std::string() : it->second;
}

// ---- operands ----

// How many registers an operand of `name` takes, from its type suffixes: 0 the destination,
// 1-3 the sources. "_i32_b64" is a 32-bit result of 64-bit sources; one suffix is all of them.
int width(std::string_view name, int which) {
    std::vector<int> bits;
    for (size_t k = 0; k + 2 < name.size(); k++) {
        if (name[k] != '_' || !std::strchr("biuf", name[k + 1])) continue;
        size_t e = k + 2;
        int v = 0;
        while (e < name.size() && name[e] >= '0' && name[e] <= '9') v = v * 10 + (name[e++] - '0');
        if (e > k + 2 && (e == name.size() || name[e] == '_')) bits.push_back(v);
    }
    // the exceptions: a 64-bit value and a 32-bit shift, offset or bit index
    static const char* second32[] = {"s_lshl_b64", "s_lshr_b64", "s_ashr_i64", "s_bfe_u64",
                                     "s_bfe_i64", "s_bitcmp0_b64", "s_bitcmp1_b64", "v_lshl_b64",
                                     "v_lshr_b64", "v_ashr_i64", "v_ldexp_f64", "v_trig_preop_f64",
                                     "v_cmp_class_f64", "v_cmpx_class_f64"};
    for (const char* n : second32)
        if (name == n && which == 2) return 1;
    if ((name == "s_bitset0_b64" || name == "s_bitset1_b64" || name == "s_bfm_b64") && which > 0) return 1;
    if (name == "v_mad_u64_u32" || name == "v_mad_i64_i32") return which == 1 || which == 2 ? 1 : 2;
    if (bits.empty()) return 1;
    int b = bits.size() >= 2 && which > 0 ? bits[1] : bits[0];
    return b == 64 ? 2 : 1;
}

std::string range(const char* prefix, u32 first, int n) {
    return n <= 1 ? std::format("{}{}", prefix, first) : std::format("{}[{}:{}]", prefix, first, first + u32(n) - 1);
}

struct Decoder {
    const u32* p;
    u32 avail;  // dwords readable from p
    u32 size = 1;  // dwords so far: the instruction, then its second dword or literal
    bool literal = false, bad = false;
    bool float_op = false;  // show a literal as a float too

    u32 dword(u32 k) {
        if (k >= avail) {
            bad = true;
            return 0;
        }
        return p[k];
    }

    std::string lit() {
        if (!literal) {
            literal = true;
            size++;
        }
        u32 v = dword(size - 1);
        if (!float_op) return std::format("{:#x}", v);
        float f;
        std::memcpy(&f, &v, 4);
        return std::format("{:#x} ({})", v, f);
    }

    // A scalar operand (8 bits): an SGPR, a special register, a constant or the literal
    std::string scalar(u32 r, int n = 1) {
        if (r <= 103) return range("s", r, n);
        switch (r) {
            case 104: return n == 2 ? "flat_scratch" : "flat_scratch_lo";
            case 105: return "flat_scratch_hi";
            case 106: return n == 2 ? "vcc" : "vcc_lo";
            case 107: return "vcc_hi";
            case 108: return n == 2 ? "tba" : "tba_lo";
            case 109: return "tba_hi";
            case 110: return n == 2 ? "tma" : "tma_lo";
            case 111: return "tma_hi";
            case 124: return "m0";
            case 126: return n == 2 ? "exec" : "exec_lo";
            case 127: return "exec_hi";
            case 251: return "vccz";
            case 252: return "execz";
            case 253: return "scc";
            case 254: return "lds_direct";
            case 255: return lit();
            default: break;
        }
        if (r >= 112 && r <= 123) return range("ttmp", r - 112, n);
        if (r >= 128 && r <= 192) return std::to_string(r - 128);
        if (r >= 193 && r <= 208) return std::to_string(192 - int(r));
        static const char* f[] = {"0.5", "-0.5", "1.0", "-1.0", "2.0", "-2.0", "4.0", "-4.0"};
        if (r >= 240 && r <= 247) return f[r - 240];
        return std::format("(operand {})", r);
    }

    // A vector instruction's source (9 bits): 256 and up are VGPRs
    std::string source(u32 r, int n = 1) { return r >= 256 ? range("v", r - 256, n) : scalar(r, n); }
};

std::string vgpr(u32 r, int n = 1) { return range("v", r, n); }

struct Insn {
    u32 at = 0, size = 1;
    std::string text;
    long target = -1;  // a branch's destination, in dwords
    bool known = true;
    bool end = false;  // s_endpgm
    bool ret = false;  // s_setpc_b64: a return, as a fetch shader ends
    int call_sgpr = -1;  // s_swappc_b64 / s_setpc_b64: the SGPR pair it jumps to
};

std::string waitcnt(u32 v) {
    std::string s;
    u32 vm = v & 0xf, exp = (v >> 4) & 7, lgkm = (v >> 8) & 0xf;
    if (vm != 0xf) s += std::format("vmcnt({}) ", vm);
    if (exp != 7) s += std::format("expcnt({}) ", exp);
    if (lgkm != 0xf) s += std::format("lgkmcnt({}) ", lgkm);
    if (s.empty()) return "(nothing)";
    s.pop_back();
    return s;
}

std::string with_mods(std::string s, bool neg, bool abs) {
    if (abs) s = "|" + s + "|";
    if (neg) s = "-" + s;
    return s;
}

Insn decode(const u32* p, u32 avail, u32 at) {
    Insn in;
    in.at = at;
    Decoder d{p, avail};
    u32 w = p[0];
    std::string& t = in.text;
    auto unknown_op = [&](const char* format, u32 op) {
        in.known = false;
        t = std::format("({} op {:#x}: not known)", format, op);
    };

    if (!(w >> 31)) {
        u32 top7 = w >> 25;
        if (top7 == 0x3f) {  // VOP1: vdst, op, src0
            u32 op = (w >> 9) & 0xff;
            const char* n = lookup(VOP1, op);
            if (!n) return unknown_op("VOP1", op), in.size = d.size, in;
            d.float_op = std::string_view(n).find("f32") != std::string_view::npos;
            std::string_view name = n;
            if (op == 0) t = n;
            else if (name == "v_readfirstlane_b32")
                t = std::format("{} {}, {}", n, d.scalar((w >> 17) & 0xff), d.source(w & 0x1ff));
            else
                t = std::format("{} {}, {}", n, vgpr((w >> 17) & 0xff, width(name, 0)),
                                d.source(w & 0x1ff, width(name, 1)));
        } else if (top7 == 0x3e) {  // VOPC: op, vsrc1, src0; the result to vcc
            u32 op = (w >> 17) & 0xff;
            std::string n = vopc_name(op);
            if (n.empty()) return unknown_op("VOPC", op), in.size = d.size, in;
            d.float_op = n.ends_with("f32");
            t = std::format("{} vcc, {}, {}", n, d.source(w & 0x1ff, width(n, 1)),
                            vgpr((w >> 9) & 0xff, width(n, 2)));
        } else {  // VOP2: op, vdst, vsrc1, src0
            u32 op = (w >> 25) & 0x3f;
            const char* n = lookup(VOP2, op);
            if (!n) return unknown_op("VOP2", op), in.size = d.size, in;
            std::string_view name = n;
            d.float_op = name.find("f32") != std::string_view::npos;
            std::string dst = vgpr((w >> 17) & 0xff), s0 = d.source(w & 0x1ff), s1 = vgpr((w >> 9) & 0xff);
            if (name == "v_cndmask_b32") t = std::format("{} {}, {}, {}, vcc", n, dst, s0, s1);
            else if (name == "v_madmk_f32") t = std::format("{} {}, {}, {}, {}", n, dst, s0, d.lit(), s1);
            else if (name == "v_madak_f32") t = std::format("{} {}, {}, {}, {}", n, dst, s0, s1, d.lit());
            else if (op >= 37 && op <= 39) t = std::format("{} {}, vcc, {}, {}", n, dst, s0, s1);
            else if (op >= 40 && op <= 42) t = std::format("{} {}, vcc, {}, {}, vcc", n, dst, s0, s1);
            else t = std::format("{} {}, {}, {}", n, dst, s0, s1);
        }
    } else if ((w >> 30) == 2) {  // the scalar formats
        u32 top9 = w >> 23;
        if (top9 == 0x17d) {  // SOP1: sdst, op, ssrc0
            u32 op = (w >> 8) & 0xff;
            const char* n = lookup(SOP1, op);
            if (!n) return unknown_op("SOP1", op), in.size = d.size, in;
            std::string_view name = n;
            u32 dst = (w >> 16) & 0x7f, src = w & 0xff;
            if (name == "s_getpc_b64") t = std::format("{} {}", n, d.scalar(dst, 2));
            else if (name == "s_setpc_b64" || name == "s_rfe_b64" || name == "s_cbranch_join") {
                t = std::format("{} {}", n, d.scalar(src, width(name, 1)));
                if (name == "s_setpc_b64") {
                    in.ret = true;
                    if (src <= 103) in.call_sgpr = int(src);
                }
            } else {
                t = std::format("{} {}, {}", n, d.scalar(dst, width(name, 0)), d.scalar(src, width(name, 1)));
                if (name == "s_swappc_b64" && src <= 103) in.call_sgpr = int(src);
            }
        } else if (top9 == 0x17e) {  // SOPC: op, ssrc1, ssrc0
            u32 op = (w >> 16) & 0x7f;
            const char* n = lookup(SOPC, op);
            if (!n) return unknown_op("SOPC", op), in.size = d.size, in;
            t = std::format("{} {}, {}", n, d.scalar(w & 0xff, width(n, 1)), d.scalar((w >> 8) & 0xff, width(n, 2)));
        } else if (top9 == 0x17f) {  // SOPP: op, simm16
            u32 op = (w >> 16) & 0x7f, imm = w & 0xffff;
            const char* n = lookup(SOPP, op);
            if (!n) return unknown_op("SOPP", op), in.size = d.size, in;
            std::string_view name = n;
            if (name == "s_endpgm") {
                t = n;
                in.end = true;
            } else if (name == "s_branch" || name.starts_with("s_cbranch")) {
                in.target = long(at) + 1 + std::int16_t(imm);
                t = std::format("{} label_{:04x}", n, u32(in.target) * 4);
            } else if (name == "s_waitcnt") {
                t = std::format("{} {}", n, waitcnt(imm));
            } else if (name == "s_barrier" || name == "s_icache_inv" || name == "s_ttracedata") {
                t = n;
            } else {
                t = std::format("{} {}", n, imm);
            }
        } else if ((w >> 28) == 0xb) {  // SOPK: op, sdst, simm16
            u32 op = (w >> 23) & 0x1f, imm = w & 0xffff;
            const char* n = lookup(SOPK, op);
            if (!n) return unknown_op("SOPK", op), in.size = d.size, in;
            std::string_view name = n;
            u32 dst = (w >> 16) & 0x7f;
            if (name.starts_with("s_getreg") || name.starts_with("s_setreg")) {
                std::string hw = std::format("hwreg({}, {}, {})", imm & 0x3f, (imm >> 6) & 0x1f, ((imm >> 11) & 0x1f) + 1);
                if (name == "s_setreg_imm32_b32") t = std::format("{} {}, {}", n, hw, d.lit());
                else if (name == "s_setreg_b32") t = std::format("{} {}, {}", n, hw, d.scalar(dst));
                else t = std::format("{} {}, {}", n, d.scalar(dst), hw);
            } else if (name == "s_cbranch_i_fork") {
                in.target = long(at) + 1 + std::int16_t(imm);
                t = std::format("{} {}, label_{:04x}", n, d.scalar(dst, 2), u32(in.target) * 4);
            } else {
                t = std::format("{} {}, {:#x}", n, d.scalar(dst), imm);
            }
        } else {  // SOP2: op, sdst, ssrc1, ssrc0
            u32 op = (w >> 23) & 0x7f;
            const char* n = lookup(SOP2, op);
            if (!n) return unknown_op("SOP2", op), in.size = d.size, in;
            t = std::format("{} {}, {}, {}", n, d.scalar((w >> 16) & 0x7f, width(n, 0)),
                            d.scalar(w & 0xff, width(n, 1)), d.scalar((w >> 8) & 0xff, width(n, 2)));
        }
    } else {
        u32 enc = w >> 26;
        if (enc == 0x30 || enc == 0x31) {  // SMRD: op, sdst, sbase, imm, offset
            u32 op = (w >> 22) & 0x1f, dst = (w >> 15) & 0x7f, base = ((w >> 9) & 0x3f) * 2;
            bool imm = (w >> 8) & 1;
            u32 off = w & 0xff;
            if (op == 0x1e) t = std::format("s_memtime {}", d.scalar(dst, 2));
            else if (op == 0x1f) t = "s_dcache_inv";
            else if (op == 0x1d) t = "s_dcache_inv_vol";
            else {
                const char* n = lookup(SMRD, op);
                if (!n) return unknown_op("SMRD", op), in.size = d.size, in;
                std::string_view name = n;
                int count = name.ends_with("x16") ? 16 : name.ends_with("x8") ? 8 : name.ends_with("x4") ? 4
                          : name.ends_with("x2") ? 2 : 1;
                bool buffer = name.starts_with("s_buffer");
                std::string offset = imm ? std::format("{:#x}", off * 4)  // in dwords; shown in bytes
                                         : off == 255 ? d.lit() : d.scalar(off);
                t = std::format("{} {}, {}, {}", n, d.scalar(dst, count), d.scalar(base, buffer ? 4 : 2), offset);
            }
        } else if (enc == 0x32) {  // VINTRP: vdst, op, attr, chan, vsrc
            u32 op = (w >> 16) & 3, attr = (w >> 10) & 0x3f, chan = (w >> 8) & 3;
            static const char* names[] = {"v_interp_p1_f32", "v_interp_p2_f32", "v_interp_mov_f32", nullptr};
            static const char* chans = "xyzw";
            if (!names[op]) return unknown_op("VINTRP", op), in.size = d.size, in;
            u32 src = w & 0xff;
            std::string s = op == 2 ? (src == 0 ? "p10" : src == 1 ? "p20" : src == 2 ? "p0" : "?") : vgpr(src);
            t = std::format("{} {}, {}, attr{}.{}", names[op], vgpr((w >> 18) & 0xff), s, attr, chans[chan]);
        } else {  // the two-dword formats
            in.size = d.size = 2;
            u32 w2 = d.dword(1);
            if (d.bad) {
                in.known = false;
                t = "(cut short: the instruction's second dword is past the end)";
                return in;
            }
            if (enc == 0x34) {  // VOP3
                u32 op = (w >> 17) & 0x1ff;
                std::string name;
                int sources = 2;
                bool cmp = false, sdst = false, vop1 = false;
                if (op < 0x100) {
                    name = vopc_name(op);
                    cmp = true;
                } else if (op < 0x140) {
                    if (const char* n = lookup(VOP2, op - 0x100)) name = n;
                    if (op >= 0x125 && op <= 0x12a) sdst = true;
                    // v_cndmask's condition and v_addc/v_subb's carry are a third source
                    sources = op == 0x100 || (op >= 0x128 && op <= 0x12a) ? 3 : 2;
                } else if (op < 0x180) {
                    if (const char* n = lookup(VOP3, op - 0x140)) name = n;
                    sources = (op >= 0x161 && op <= 0x16c) || op == 0x174 ? 2 : 3;
                    sdst = op == 0x16d || op == 0x16e || op == 0x176 || op == 0x177;
                } else {
                    if (const char* n = lookup(VOP1, op - 0x180)) name = n;
                    sources = 1;
                    vop1 = true;
                }
                if (name.empty()) return unknown_op("VOP3", op), in;
                d.float_op = name.find("f32") != std::string::npos;
                u32 abs = sdst ? 0 : (w >> 8) & 7, neg = w2 >> 29;
                bool clamp = !sdst && ((w >> 11) & 1);
                u32 omod = (w2 >> 27) & 3;
                u32 src[3] = {w2 & 0x1ff, (w2 >> 9) & 0x1ff, (w2 >> 18) & 0x1ff};
                std::vector<std::string> ops;
                if (cmp) ops.push_back(d.scalar(w & 0xff, 2));
                else if (name == "v_readlane_b32" || name == "v_readfirstlane_b32") ops.push_back(d.scalar(w & 0xff));
                else if (!(vop1 && name == "v_nop")) ops.push_back(vgpr(w & 0xff, width(name, 0)));
                if (sdst) ops.push_back(d.scalar((w >> 8) & 0x7f, 2));
                if (!(vop1 && name == "v_nop"))
                    for (int k = 0; k < sources; k++) {
                        int n = width(name, k + 1);
                        if (k == 2 && (name == "v_cndmask_b32" || (op >= 0x128 && op <= 0x12a))) n = 2;
                        ops.push_back(with_mods(d.source(src[k], n), (neg >> k) & 1, (abs >> k) & 1));
                    }
                t = name;
                for (size_t k = 0; k < ops.size(); k++) t += (k ? ", " : " ") + ops[k];
                if (clamp) t += " clamp";
                static const char* omods[] = {"", " mul:2", " mul:4", " div:2"};
                t += omods[omod];
            } else if (enc == 0x36) {  // DS
                u32 op = (w >> 18) & 0xff;
                auto it = ds_names().find(op);
                if (it == ds_names().end()) return unknown_op("DS", op), in;
                std::string_view name = it->second;
                int n = name.ends_with("128") ? 4 : name.ends_with("96") ? 3 : width(name, 0);
                bool two = name.find("2_") != std::string_view::npos || name.find("2st64") != std::string_view::npos;
                bool ret = name.find("read") != std::string_view::npos || name.find("rtn") != std::string_view::npos ||
                           name == "ds_swizzle_b32" || name == "ds_consume" || name == "ds_append" ||
                           name == "ds_ordered_count";
                bool data0 = !(name.find("read") != std::string_view::npos || name == "ds_consume" ||
                               name == "ds_append" || name == "ds_nop" || name == "ds_swizzle_b32");
                bool data1 = (two && name.find("read") == std::string_view::npos) ||
                             name.find("cmpst") != std::string_view::npos || name.find("mskor") != std::string_view::npos;
                if (name.find("wrxchg2") != std::string_view::npos) data1 = true;
                std::vector<std::string> ops;
                if (ret) ops.push_back(vgpr(w2 >> 24, two ? n * 2 : n));
                if (name != "ds_consume" && name != "ds_append" && name != "ds_nop") ops.push_back(vgpr(w2 & 0xff));
                if (data0) ops.push_back(vgpr((w2 >> 8) & 0xff, n));
                if (data1) ops.push_back(vgpr((w2 >> 16) & 0xff, n));
                t = std::string(name);
                for (size_t k = 0; k < ops.size(); k++) t += (k ? ", " : " ") + ops[k];
                u32 o0 = w & 0xff, o1 = (w >> 8) & 0xff;
                if (two) {
                    if (o0) t += std::format(" offset0:{}", o0);
                    if (o1) t += std::format(" offset1:{}", o1);
                } else if (o0 | o1) {
                    t += std::format(" offset:{}", o0 | (o1 << 8));
                }
                if ((w >> 17) & 1) t += " gds";
            } else if (enc == 0x38 || enc == 0x3a) {  // MUBUF, MTBUF
                bool typed = enc == 0x3a;
                u32 op = typed ? (w >> 16) & 7 : (w >> 18) & 0x7f;
                std::string name;
                int n = 1;
                if (typed) {
                    name = MTBUF[op];
                    n = int(op & 3) + 1;
                } else {
                    auto it = mubuf_ops().find(op);
                    if (it == mubuf_ops().end()) return unknown_op("MUBUF", op), in;
                    name = it->second.name;
                    n = it->second.data;
                }
                if (n == 0) {
                    t = name;
                } else {
                    bool offen = (w >> 12) & 1, idxen = (w >> 13) & 1, addr64 = (w >> 15) & 1;
                    int addrs = addr64 ? 2 : int(offen) + int(idxen);
                    if ((w2 >> 23) & 1) n++;  // tfe: one more, whether it was resident
                    t = std::format("{} {}, {}, {}, {}", name, vgpr((w2 >> 8) & 0xff, n),
                                    addrs ? vgpr(w2 & 0xff, addrs) : std::string("off"),
                                    d.scalar(((w2 >> 16) & 0x1f) * 4, 4), d.scalar(w2 >> 24));
                    if (offen) t += " offen";
                    if (idxen) t += " idxen";
                    if (addr64) t += " addr64";
                    if (w & 0xfff) t += std::format(" offset:{}", w & 0xfff);
                    if (typed) {  // the data format, and how its numbers read
                        static const char* dfmt[] = {"invalid", "8", "16", "8_8", "32", "16_16", "10_11_11",
                                                     "11_11_10", "10_10_10_2", "2_10_10_10", "8_8_8_8", "32_32",
                                                     "16_16_16_16", "32_32_32", "32_32_32_32", "reserved"};
                        static const char* nfmt[] = {"unorm", "snorm", "uscaled", "sscaled",
                                                     "uint", "sint", "snorm_ogl", "float"};
                        t += std::format(" format:[{},{}]", dfmt[(w >> 19) & 0xf], nfmt[(w >> 23) & 7]);
                    }
                    if ((w >> 14) & 1) t += " glc";
                    if ((w2 >> 22) & 1) t += " slc";
                    if (!typed && ((w >> 16) & 1)) t += " lds";
                    if ((w2 >> 23) & 1) t += " tfe";
                }
            } else if (enc == 0x3c) {  // MIMG
                u32 op = (w >> 18) & 0x7f;
                std::string name = mimg_name(op);
                if (name.empty()) return unknown_op("MIMG", op), in;
                u32 dmask = (w >> 8) & 0xf;
                int n = name.starts_with("image_gather4") ? 4 : std::max(1, std::popcount(dmask));
                if ((w >> 16) & 1) n++;  // tfe
                bool r128 = (w >> 15) & 1;
                bool sampled = name.starts_with("image_sample") || name.starts_with("image_gather4") || name == "image_get_lod";
                t = std::format("{} {}, {}, {}", name, vgpr((w2 >> 8) & 0xff, n), vgpr(w2 & 0xff),
                                d.scalar(((w2 >> 16) & 0x1f) * 4, r128 ? 4 : 8));
                if (sampled) t += ", " + d.scalar(((w2 >> 21) & 0x1f) * 4, 4);
                t += std::format(" dmask:{:#x}", dmask);
                if ((w >> 12) & 1) t += " unorm";
                if ((w >> 13) & 1) t += " glc";
                if ((w >> 25) & 1) t += " slc";
                if ((w >> 14) & 1) t += " da";
                if (r128) t += " r128";
                if ((w >> 16) & 1) t += " tfe";
                if ((w >> 17) & 1) t += " lwe";
            } else if (enc == 0x3e) {  // EXP
                u32 en = w & 0xf, target = (w >> 4) & 0x3f;
                bool compr = (w >> 10) & 1;
                std::string where = target <= 7 ? std::format("mrt{}", target)
                                  : target == 8 ? "mrtz" : target == 9 ? "null"
                                  : target >= 12 && target <= 15 ? std::format("pos{}", target - 12)
                                  : target >= 32 && target <= 63 ? std::format("param{}", target - 32)
                                  : std::format("(target {})", target);
                t = "exp " + where;
                for (u32 k = 0; k < 4; k++) {
                    // compressed: two registers, each two 16-bit values; en's bits go in pairs
                    u32 reg = compr ? (k < 2 ? w2 & 0xff : (w2 >> 8) & 0xff) : (w2 >> (8 * k)) & 0xff;
                    t += (k ? ", " : " ") + ((en >> k) & 1 ? vgpr(reg) : std::string("off"));
                }
                if (compr) t += " compr";
                if ((w >> 11) & 1) t += " done";
                if ((w >> 12) & 1) t += " vm";
            } else if (enc == 0x37) {
                in.known = false;
                t = "(FLAT: not decoded)";
            } else {
                in.known = false;
                t = "(not an instruction this disassembler knows)";
            }
        }
    }
    in.size = d.size;
    if (d.bad) {
        in.known = false;
        t += " (cut short: its literal is past the end)";
    }
    return in;
}

// The footer after a shader's code: "OrbShdr", a version, then a word whose top 24 bits are
// the code's length in bytes; two hash words follow at 16 and 20.
struct Footer { bool found = false; u32 length = 0, hash0 = 0, hash1 = 0; };

Footer footer_at(const u32* p, u32 avail) {
    Footer f;
    if (avail < 6) return f;
    if (std::memcmp(p, "OrbShdr", 7) != 0) return f;
    f.found = true;
    f.length = p[2] >> 8;
    f.hash0 = p[4];
    f.hash1 = p[5];
    return f;
}

}  // namespace

GcnListing gcn_disassemble(const u32* code, u32 avail) {
    GcnListing out;
    std::vector<Insn> list;
    u32 end_at = avail;  // the footer's length, once found
    u32 i = 0;
    while (i < end_at) {
        Insn in = decode(code + i, end_at - i, i);
        if (in.call_sgpr >= 0) out.calls.push_back(in.call_sgpr);
        if (!in.known) out.unknown++;
        list.push_back(in);
        i += in.size;
        if (in.end) {
            // the footer follows the code, within a little padding; until it's found, the code
            // ends here (a branch past the end would be one the footer accounts for)
            if (!out.has_footer) {
                for (u32 k = i; k < std::min(avail, i + 64); k++) {
                    Footer f = footer_at(code + k, avail - k);
                    if (!f.found) continue;
                    out.has_footer = true;
                    out.hash0 = f.hash0;
                    out.hash1 = f.hash1;
                    out.footer_at = k * 4;
                    end_at = std::min(avail, std::max(i, f.length / 4));
                    break;
                }
            }
            if (!out.has_footer || i >= end_at) break;
        }
        if (in.ret && (!out.has_footer || i >= end_at)) break;  // a fetch shader has no footer
        if (list.size() > 100000) break;
    }
    out.bytes = i * 4;
    out.instructions = u32(list.size());
    std::map<long, bool> labels;
    for (const Insn& in : list)
        if (in.target >= 0) labels[in.target] = true;
    for (const Insn& in : list) {
        if (labels.count(in.at)) out.lines.push_back(std::format("label_{:04x}:", in.at * 4));
        std::string raw = std::format("{:08x}", code[in.at]);
        for (u32 k = 1; k < in.size; k++) raw += std::format(" {:08x}", code[in.at + k]);
        out.lines.push_back(std::format("  {:04x}  {:<26} {}", in.at * 4, raw, in.text));
    }
    return out;
}

}  // namespace mensis
