// SPDX-License-Identifier: GPL-3.0-or-later
//
// Draws on the PC's GPU, through Vulkan (vkdraw.cpp): the game's vertex and pixel shaders,
// translated (translate.cpp), into render targets (up to 8 at once) in the game's memory,
// each kept on the GPU from draw to draw and brought back into the memory when something
// touches it (the default, where there is Vulkan); or into copies, to compare with what the
// CPU drew (--gpu-check). If the vertex shader isn't translated, the vertices are the CPU's, passed on
// by a trivial one.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "loader.hpp"

namespace mensis::vkdraw {

struct Texture {
    u32 type = 9;  // the T#'s: 8 1D, 9 2D, 10 3D, 11 a cube (as a 2D array of its faces), 12 1D array, 13 2D array
    u32 width = 1, height = 1, layers = 1;  // layers: an array's slices, or a 3D image's depth
    u32 sampler[4] = {};  // the S#, if it is sampled
    // Kept on the GPU between draws under this key (the T# and a hash of the memory it reads),
    // if it isn't empty; then the texels are only needed if has_texture(key) is false
    std::string key;
    // The texels: as 32-bit floats, each as the shader reads it, slice by slice, row by row;
    // or, if format (a VkFormat) isn't 0, as they are in memory, `pitch` texels to a row, `bpp`
    // bytes each, with the T#'s dst_sel as swizzle
    std::vector<float> rgba;
    u32 format = 0, pitch = 0, bpp = 4, swizzle[4] = {4, 5, 6, 7};
    bool integer = false;  // rgba holds integers' bits: a texture of 32-bit uints, read nearest
    const unsigned char* raw = nullptr;  // (null: held in blocks, below)
    // Or block-compressed (format a BC one): `block` bytes a 4x4 block, the blocks in rows of
    // pitch / 4 (pitch a multiple of 4), held here
    u32 block = 0;
    std::vector<unsigned char> blocks;
};

// A buffer's elements (a uvec4 each), shared with the kept ones it came from
using Buffer = std::shared_ptr<const std::vector<u32>>;

struct Draw {
    std::string vs, ps;  // GLSL
    // or kept elsewhere for as long as the run (a translation kept), not copied: then these
    const std::string* vs_kept = nullptr;
    const std::string* ps_kept = nullptr;
    const std::string& vs_text() const { return vs_kept ? *vs_kept : vs; }
    const std::string& ps_text() const { return ps_kept ? *ps_kept : ps; }
    u64 vs_id = 0, ps_id = 0;  // each one's hash, if known (made once, where it is kept): else made here
    u32 attributes = 0;
    std::vector<float> vertices;  // the CPU's: each vertex's position (clip space), then attributes
    // Or the vertex shader on the GPU too (indices not empty): each triangle's three vertices'
    // indices (a rectangle's three corners, with the geometry shader gs adding the fourth),
    // the vertex shader's values and buffers (a uvec4 an element), the console's viewport as
    // the vertex shader applies it (x' = a x + b w for x, y, z), and culling
    std::vector<u32> indices;
    std::string gs;
    std::vector<u32> vs_values;
    std::vector<Buffer> vs_buffers;
    float viewport[8] = {};
    u32 cull = 0;  // PA_SU_SC_MODE_CNTL: bit 0 the front faces, bit 1 the back, bit 2 front is clockwise
    std::vector<u32> values;  // the shader's U: user SGPRs and what its scalar loads read
    std::vector<Buffer> ps_buffers;  // the pixel shader's buffers, as the vertex shader's
    std::vector<Texture> textures;
    // The render targets, by their place (0-7), all the same size, `bpp` bytes a pixel in
    // rows: each the game's memory (`address`), kept on the GPU; or, if address is 0, read
    // from `target` and the result written to `result`
    struct Target {
        u32 format = 0;  // a VkFormat; 0: nothing at this place
        u32 bpp = 4;
        u64 address = 0;
        const unsigned char* target = nullptr;
        unsigned char* result = nullptr;
        u32 blend = 0, mask = 15;  // CB_BLENDn_CONTROL, and which of R, G, B, A are written
    };
    std::vector<Target> targets;
    // The depth buffer, if there is one (format 0: none): D16_UNORM or D32_SFLOAT, its size, in
    // rows in the game's memory (kept on the GPU), or for --gpu-check read from `target`
    // (not written back); tested with `func` (a VkCompareOp: the console's numbering), and
    // written if `write`
    struct Depth {
        u32 format = 0, bpp = 4, width = 0, height = 0;
        u64 address = 0;
        const unsigned char* target = nullptr;
        unsigned char* result = nullptr;  // --gpu-check's: the depth after the draw, if it writes it
        bool test = true, write = false;
        u32 func = 7;
        // with stencil (format D32_SFLOAT_S8_UINT): its 8 bits a pixel at saddress (or, for
        // --gpu-check, read from starget), and each face's test and operations, as Vulkan's
        bool stencil = false, stencil_test = false;
        u64 saddress = 0;
        const unsigned char* starget = nullptr;
        struct Face {
            u32 fail = 0, pass = 0, zfail = 0, func = 7, cmask = 255, wmask = 255, ref = 0;  // VkStencilOp, VkCompareOp
        } front, back;
    } depth;
    u32 width = 0, height = 0;
    u32 rows = 0;  // the kept targets' rows in memory: past their height, to a multiple of 8 (0: their height)
    float blend_color[4] = {};
    int scissor[4] = {};  // x0, y0, x1, y1
};

// A compute dispatch: its GLSL, its U (the places of its loads and stores in it filled in
// here), the groups; each buffer store's memory (kept on the GPU from now on, if it isn't),
// each load's of memory kept on the GPU (and which load it is), and the other loads' elements,
// unpacked
struct Dispatch {
    std::string cs;
    std::vector<u32> values;
    u32 store_slot = 0, load_slot = 0;
    u32 groups[3] = {1, 1, 1};
    struct Access {
        u64 address = 0, bytes = 0;
        u32 records = 0;  // the buffer's elements
        u32 bpp = 4;  // an element's bytes: 4, 8 or 16
    };
    std::vector<Access> stores, loads;
    std::vector<u32> load_index;
    std::vector<std::vector<u32>> arrays;
    // An array's memory as it is (address, bytes), if it is kept on the GPU (its array left
    // empty): copied from there on the GPU, not read back first
    std::vector<std::pair<u64, u64>> array_from;
};
bool dispatch(Dispatch& d, std::string& why);
// Whether that memory is kept on the GPU, an element of bpp bytes a texel
bool kept(u64 address, u64 bytes, u32 bpp);
// A depth buffer kept on the GPU, cleared there (all of it, or the rectangle from x0, y0 to x1,
// y1): its depth to that value if depth, its stencil to the other if it is 0 to 255 (else left
// as it is); false if it isn't kept there as that size, or has no stencil buffer to clear
bool clear_depth(u64 address, u32 width, u32 height, float value, int stencil = -1, bool depth = true, u32 x0 = 0, u32 y0 = 0,
                 u32 x1 = ~0u, u32 y1 = ~0u);
// A depth buffer's fast clear (HTILE) seen to on the GPU, the HTILE buffer and the depth buffer
// both kept there: filled with the value if every tile is marked cleared. False if it can't be.
bool clear_depth_by_htile(u64 htile, u64 tiles, u64 address, u32 width, u32 height, float value);
// If that memory is kept on the GPU, a number that changes whenever anything writes it (a draw,
// a dispatch, the CPU); else 0
u64 version(u64 address);

bool has_texture(const std::string& key);
// Whether textures can be sent as their compressed blocks (BC1-7, textureCompressionBC)
bool compressed();
// Whether a pixel shader can have its triangle's corners' attributes and its barycentrics
// (VK_KHR_fragment_shader_barycentric), to interpolate as the console does
bool barycentric();
// A fault at addr (a write, or a read): if it is in a render target kept on the GPU, the
// picture goes back into the memory, or the memory is let be written, and true: the access
// can be tried again
bool touched(u64 addr, bool write, u64 rip = 0);
// What the loader is doing on this thread (for the count of targets read back when touched)
extern thread_local const char* t_doing;
struct Doing {
    const char* was;
    explicit Doing(const char* what) : was(t_doing) { t_doing = what; }
    ~Doing() { t_doing = was; }
};
// Draws it (into the batch, kept on the GPU; --gpu-check's copies at once); false, with the
// reason, if it couldn't
bool run(Draw& d, std::string& why);
// The draws and dispatches so far to the GPU, waited for (the end of a command buffer of the
// game's)
bool flush();
std::string device();  // the GPU it runs on, and what the validation layer reported
std::string report();  // what is kept on the GPU, and how long the draws took there

}  // namespace mensis::vkdraw
