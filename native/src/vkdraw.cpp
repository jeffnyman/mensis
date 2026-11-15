// SPDX-License-Identifier: GPL-3.0-or-later
//
// Draws on the PC's GPU, through Vulkan, one at a time. The GLSL is compiled to SPIR-V by the
// Vulkan SDK's shaderc. Kept between draws: the compiled shaders and pipelines (one descriptor
// set layout serves them all), the samplers, one buffer for what goes up and one for what
// comes back, the textures, and the render targets. A draw writes up to 8 targets at once, of
// 8-bit RGBA, 16-bit floats or 11- and 10-bit floats.
//
// A texture is kept by its descriptor and a hash of the memory it reads, so a texture that
// hasn't changed isn't decoded or uploaded again (up to 2 GiB of them; those not used lately
// go first). It is uploaded as it is in memory if the GPU reads that format the same way
// (8-bit RGBA, in rows), else as 32-bit floats, decoded on the CPU exactly as the CPU's
// interpreter reads them.
//
// A render target is kept by its address, and goes up from the game's memory only when the
// memory may be newer than the GPU's copy. Its memory is guarded instead, by the pages'
// protection: while the GPU's picture is newer, nothing may touch the memory; when the two are
// the same, the memory may be read. A touch faults, and the fault handler (platform_windows.cpp)
// asks touched(): the picture comes back into the memory, which may then be read; a write
// makes the memory the newer, and the next draw takes it up again. So whatever reads the
// memory sees the picture (the window, the CPU's dispatches and draws, the game itself), but a
// target the GPU draws into again and again stays there in between. A target that shares a
// page with another lets the other go (back into memory) first. A draw that reads a kept
// target as a texture (the whole of it, in rows) is given a copy of it made on the GPU. For
// --gpu-check, the draw goes into a copy instead: a target of that format and size, uploaded
// and read back.
//
// Built where the Vulkan SDK is (Windows, so far: MENSIS_VULKAN); elsewhere run() says so.

#include "vkdraw.hpp"

#ifdef MENSIS_VULKAN

#include <shaderc/shaderc.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <mutex>

namespace mensis::vkdraw {

namespace {

struct Gpu {
    bool tried = false, ok = false;
    std::string why, name;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    u32 family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    bool validation = false, int64 = false, barycentric = false;
    shaderc_compiler_t compiler = nullptr;
    std::map<std::string, std::vector<u32>> spirv;  // compiled, by source
};

Gpu g;
std::mutex g_lock;
std::atomic<u64> g_messages{0};
std::string g_first_message;

VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                                          const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (g_messages++ == 0) g_first_message = data->pMessage;
    if (g_messages <= 10) std::fprintf(stderr, "Vulkan validation (GPU drawing): %s\n", data->pMessage);
    return VK_FALSE;
}

bool fail(const std::string& w) {
    g.why = w;
    return false;
}

bool init() {
    if (g.tried) return g.ok;
    g.tried = true;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Mensis";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* layer = "VK_LAYER_KHRONOS_validation";
    u32 n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    for (auto& l : layers) g.validation |= std::strcmp(l.layerName, layer) == 0;
    const char* ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    mi.pfnUserCallback = on_message;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    if (g.validation) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
        ci.enabledExtensionCount = 1;
        ci.ppEnabledExtensionNames = &ext;
        ci.pNext = &mi;
    }
    if (vkCreateInstance(&ci, nullptr, &g.instance) != VK_SUCCESS) return fail("Vulkan couldn't be started");
    if (g.validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(g.instance, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerEXT m;
        if (create) create(g.instance, &mi, nullptr, &m);
    }
    vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(g.instance, &n, gpus.data());
    int best = -1;
    for (VkPhysicalDevice p : gpus) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(p, &pr);
        if (pr.apiVersion < VK_API_VERSION_1_3) continue;
        u32 qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, qs.data());
        for (u32 i = 0; i < qn; i++) {
            if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best) best = score, g.gpu = p, g.family = i, g.name = pr.deviceName;
            break;
        }
    }
    if (!g.gpu) return fail("no GPU with Vulkan 1.3");
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(g.gpu, &have);
    g.int64 = have.shaderInt64;
    VkPhysicalDeviceFeatures want{};
    want.shaderInt64 = have.shaderInt64;
    if (!have.geometryShader || !have.depthClamp) return fail("a GPU without geometry shaders or depth clamping");
    want.geometryShader = VK_TRUE;  // a rectangle's fourth corner
    want.depthClamp = VK_TRUE;  // depth isn't clipped: the console's draws here don't test it
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.descriptorBindingPartiallyBound = VK_TRUE;  // one layout, its bindings used or not
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.pNext = &v12;
    v13.dynamicRendering = VK_TRUE;
    v13.shaderDemoteToHelperInvocation = VK_TRUE;  // what discard compiles to for Vulkan 1.3
    // a pixel shader given each corner's attributes and its barycentrics, to interpolate as the
    // console does (translate.cpp), where the GPU can
    u32 en = 0;
    vkEnumerateDeviceExtensionProperties(g.gpu, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> exts(en);
    vkEnumerateDeviceExtensionProperties(g.gpu, nullptr, &en, exts.data());
    for (auto& e : exts) g.barycentric |= std::strcmp(e.extensionName, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) == 0;
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR bf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    if (g.barycentric) {
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &bf;
        vkGetPhysicalDeviceFeatures2(g.gpu, &f2);
        g.barycentric = bf.fragmentShaderBarycentric;
        bf.pNext = nullptr;
        v12.pNext = &bf;
    }
    const char* bary_ext = VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME;
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = g.family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &v13;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.pEnabledFeatures = &want;
    if (g.barycentric) {
        di.enabledExtensionCount = 1;
        di.ppEnabledExtensionNames = &bary_ext;
    }
    if (vkCreateDevice(g.gpu, &di, nullptr, &g.device) != VK_SUCCESS) return fail("vkCreateDevice failed");
    vkGetDeviceQueue(g.device, g.family, 0, &g.queue);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = g.family;
    if (vkCreateCommandPool(g.device, &pi, nullptr, &g.pool) != VK_SUCCESS) return fail("vkCreateCommandPool failed");
    g.compiler = shaderc_compiler_initialize();
    if (!g.compiler) return fail("shaderc couldn't be started");
    g.ok = true;
    return true;
}

bool compile(const std::string& src, shaderc_shader_kind kind, std::vector<u32>& out, std::string& why) {
    auto it = g.spirv.find(src);
    if (it != g.spirv.end()) {
        out = it->second;
        return true;
    }
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(o, shaderc_optimization_level_performance);
    shaderc_compilation_result_t r =
        shaderc_compile_into_spv(g.compiler, src.data(), src.size(), kind, kind == shaderc_vertex_shader ? "vs.glsl" : "ps.glsl", "main", o);
    bool ok = shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success;
    if (ok) {
        const u32* p = reinterpret_cast<const u32*>(shaderc_result_get_bytes(r));
        out.assign(p, p + shaderc_result_get_length(r) / 4);
        g.spirv[src] = out;
    } else {
        why = std::string("the GLSL didn't compile: ") + shaderc_result_get_error_message(r);
    }
    shaderc_result_release(r);
    shaderc_compile_options_release(o);
    return ok;
}

u32 memory_type(u32 bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g.gpu, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}

void barrier(VkCommandBuffer cmd, VkImage image, u32 layers, VkImageLayout from, VkImageLayout to, VkAccessFlags sa,
             VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool blend_factor(u32 k, VkBlendFactor& f) {
    switch (k) {
        case 0: f = VK_BLEND_FACTOR_ZERO; return true;
        case 1: f = VK_BLEND_FACTOR_ONE; return true;
        case 2: f = VK_BLEND_FACTOR_SRC_COLOR; return true;
        case 3: f = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; return true;
        case 4: f = VK_BLEND_FACTOR_SRC_ALPHA; return true;
        case 5: f = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; return true;
        case 6: f = VK_BLEND_FACTOR_DST_ALPHA; return true;
        case 7: f = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA; return true;
        case 8: f = VK_BLEND_FACTOR_DST_COLOR; return true;
        case 9: f = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; return true;
        case 10: f = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE; return true;
        case 13: f = VK_BLEND_FACTOR_CONSTANT_COLOR; return true;
        case 14: f = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; return true;
        case 19: f = VK_BLEND_FACTOR_CONSTANT_ALPHA; return true;
        case 20: f = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA; return true;
        default: return false;
    }
}

bool blend_op(u32 k, VkBlendOp& o) {
    static const VkBlendOp ops[5] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
                                     VK_BLEND_OP_REVERSE_SUBTRACT};
    if (k > 4) return false;
    o = ops[k];
    return true;
}

VkSamplerAddressMode address_mode(u32 m) {
    switch (m) {
        case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 4: case 5: case 6: case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;  // as the CPU's interpreter does
    }
}

// ---- what is kept between draws ----

// A buffer of the GPU's own (for copies between images)
struct DeviceBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void destroy() {
        if (buffer) vkDestroyBuffer(g.device, buffer, nullptr), vkFreeMemory(g.device, memory, nullptr);
        *this = {};
    }
    bool reserve(VkDeviceSize want) {  // (the old contents aren't kept)
        if (want <= size) return true;
        destroy();
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = std::max<VkDeviceSize>(want + want / 2, 16 << 20);
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(g.device, &bi, nullptr, &buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(g.device, buffer, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(g.device, &ai, nullptr, &memory) != VK_SUCCESS) {
            vkDestroyBuffer(g.device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }
        vkBindBufferMemory(g.device, buffer, memory, 0);
        size = bi.size;
        return true;
    }
};

// A buffer the CPU writes and reads, mapped
struct HostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    unsigned char* p = nullptr;
    VkDeviceSize size = 0;
    void destroy() {
        if (buffer) vkDestroyBuffer(g.device, buffer, nullptr), vkFreeMemory(g.device, memory, nullptr);
        *this = {};
    }
    // At least `want` bytes (the old contents aren't kept). `cached`: for the CPU to read, in
    // the CPU's own cached memory (the GPU's memory the CPU can see is very slow to read from)
    bool reserve(VkDeviceSize want, VkBufferUsageFlags usage, bool cached = false) {
        if (want <= size) return true;
        destroy();
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = std::max<VkDeviceSize>(want + want / 2, 1 << 20);
        bi.usage = usage;
        if (vkCreateBuffer(g.device, &bi, nullptr, &buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(g.device, buffer, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        ai.memoryTypeIndex = ~0u;
        if (cached) ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (ai.memoryTypeIndex == ~0u) ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, host);
        if (vkAllocateMemory(g.device, &ai, nullptr, &memory) != VK_SUCCESS) {
            vkDestroyBuffer(g.device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }
        vkBindBufferMemory(g.device, buffer, memory, 0);
        void* m;
        vkMapMemory(g.device, memory, 0, VK_WHOLE_SIZE, 0, &m);
        p = static_cast<unsigned char*>(m);
        size = bi.size;
        return true;
    }
};

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    u32 layers = 1;
    VkDeviceSize bytes = 0;
    u64 used = 0;  // the draw that last used it
    void destroy() {
        if (view) vkDestroyImageView(g.device, view, nullptr);
        if (image) vkDestroyImage(g.device, image, nullptr), vkFreeMemory(g.device, memory, nullptr);
        *this = {};
    }
};

bool make_image(GpuImage& im, VkImageType type, VkImageViewType view_type, VkFormat format, u32 w, u32 h, u32 depth, u32 layers,
                VkImageUsageFlags usage, VkComponentMapping swizzle = {}, VkImageCreateFlags flags = 0) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.flags = flags;
    ii.imageType = type;
    ii.format = format;
    ii.extent = {w, h, depth};
    ii.mipLevels = 1;
    ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    if (vkCreateImage(g.device, &ii, nullptr, &im.image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g.device, im.image, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g.device, &ai, nullptr, &im.memory) != VK_SUCCESS) {
        vkDestroyImage(g.device, im.image, nullptr);
        im.image = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(g.device, im.image, im.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    // the view in the image's own format isn't for storage (an sRGB format can't be), only
    // the uint view made beside it (resident())
    VkImageViewUsageCreateInfo vu{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    vu.usage = usage & ~VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT);
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT) vi.pNext = &vu;
    vi.image = im.image;
    vi.viewType = view_type;
    vi.format = format;
    vi.components = swizzle;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCreateImageView(g.device, &vi, nullptr, &im.view);
    im.layers = layers;
    im.bytes = mr.size;
    return true;
}

struct Kept {
    bool ready = false;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    HostBuffer upload, readback;  // a draw's data going up, and its target coming back
    DeviceBuffer scratch;  // kept memory on its way from one shape to another
    std::map<std::string, VkPipeline> pipelines;
    std::map<std::string, VkShaderModule> modules;
    std::map<std::string, GpuImage> textures;
    VkDeviceSize texture_bytes = 0;
    std::map<std::array<u32, 5>, VkSampler> samplers;
    std::map<std::array<u32, 4>, GpuImage> targets;  // --gpu-check's: by format, width, height, place
    std::map<std::string, VkPipeline> compute;  // by source
    u64 draws = 0, texture_uploads = 0, evicted = 0, dispatches = 0;
    u64 target_uploads = 0, readbacks = 0, writes = 0, let_go = 0, target_textures = 0;
    u64 batches = 0;  // command buffers submitted (each waited for)
    u64 regions = 0;  // memory kept on the GPU because a compute shader stored into it
    u64 reshapes = 0;  // kept memory given another shape (on the GPU)
    u64 ns = 0, dispatch_ns = 0;  // the time the draws and dispatches took, uploads included
};
Kept k;

// A render target in the game's memory, kept on the GPU: the pages it is in (lo to hi), and
// which is the newer, the GPU's picture or the memory (or neither)
struct Resident {
    enum State { gpu, same, memory };
    u64 base = 0, bytes = 0, lo = 0, hi = 0;
    u32 format = 0, width = 0, height = 0, bpp = 4;
    GpuImage image;
    VkImageView uints = VK_NULL_HANDLE;  // the same, each texel uints (bpp of them): for a compute shader's loads and stores
    State state = memory;
    // copies of it, for draws that read it as a texture: by format, width, height and swizzle
    std::map<std::array<u32, 4>, GpuImage> copies;
    // views of it in other formats of the same size of texel, for a draw into it as one (the
    // memory a compute shader made kept, then drawn into as a render target, say)
    std::map<u32, VkImageView> views;
};
std::map<u64, Resident> g_resident;  // by address
thread_local bool t_inside = false;  // this thread holds g_lock

constexpr u32 IMAGES = 31, BUFFERS = 31, PS_BUFFERS = 16;  // a pixel shader's textures (1-15, 40-55), buffers (56-71); a vertex shader's buffers (17-31, 72-87)
constexpr u32 STORES = 4, LOADS = 4;  // a compute shader's storage images: stores (32-35), loads (36-39)
constexpr u32 SETS = 2048;  // descriptor sets in a batch (one a draw or dispatch)
constexpr VkDeviceSize ARENA = VkDeviceSize(256) << 20;  // the upload buffer's least size
constexpr VkDeviceSize TEXTURE_BUDGET = VkDeviceSize(2) << 30;

u32 image_binding(u32 i) { return i < 15 ? 1 + i : 40 + (i - 15); }

// One descriptor set layout for every draw, with each binding there to be used or not
bool prepare_kept() {
    if (k.ready) return true;
    const VkShaderStageFlags all = VK_SHADER_STAGE_ALL;
    std::vector<VkDescriptorSetLayoutBinding> b;
    b.push_back({0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all, nullptr});
    for (u32 i = 0; i < IMAGES; i++) b.push_back({image_binding(i), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, all, nullptr});
    for (u32 i = 16; i <= 31; i++) b.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all, nullptr});
    for (u32 i = 72; i < 72 + (BUFFERS - 15); i++) b.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all, nullptr});
    for (u32 i = 56; i < 56 + PS_BUFFERS; i++) b.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all, nullptr});
    for (u32 i = 32; i < 36 + LOADS; i++) b.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, all, nullptr});
    std::vector<VkDescriptorBindingFlags> flags(b.size(), VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    fi.bindingCount = u32(flags.size());
    fi.pBindingFlags = flags.data();
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &fi;
    li.bindingCount = u32(b.size());
    li.pBindings = b.data();
    if (vkCreateDescriptorSetLayout(g.device, &li, nullptr, &k.set_layout) != VK_SUCCESS) return fail("vkCreateDescriptorSetLayout failed");
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &k.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(g.device, &pli, nullptr, &k.layout) != VK_SUCCESS) return fail("vkCreatePipelineLayout failed");
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (2 + BUFFERS + PS_BUFFERS) * SETS},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMAGES * SETS},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (STORES + LOADS) * SETS}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = SETS;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(g.device, &dpi, nullptr, &k.dpool) != VK_SUCCESS) return fail("vkCreateDescriptorPool failed");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(g.device, &cai, &k.cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(g.device, &fci, nullptr, &k.fence);
    k.ready = true;
    return true;
}

VkShaderModule module(const std::string& src, shaderc_shader_kind kind, std::string& why) {
    auto it = k.modules.find(src);
    if (it != k.modules.end()) return it->second;
    std::vector<u32> spv;
    if (!compile(src, kind, spv, why)) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = spv.size() * 4;
    mi.pCode = spv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g.device, &mi, nullptr, &m);
    k.modules[src] = m;
    return m;
}

VkPipeline pipeline(const Draw& d, std::string& why) {
    bool gpu_vertices = !d.indices.empty();
    std::hash<std::string> h;
    std::string key = std::format("{:x}:{:x}:{:x}:{:x}:{}:{}", h(d.vs), h(d.gs), h(d.ps), d.cull, d.attributes, gpu_vertices);
    for (const auto& t : d.targets) key += std::format(":{}.{:x}.{:x}", t.format, t.blend, t.mask);
    auto it = k.pipelines.find(key);
    if (it != k.pipelines.end()) return it->second;
    VkShaderModule vsm = module(d.vs, shaderc_vertex_shader, why), psm = module(d.ps, shaderc_fragment_shader, why);
    VkShaderModule gsm = d.gs.empty() ? VK_NULL_HANDLE : module(d.gs, shaderc_geometry_shader, why);
    if (!vsm || !psm || (!d.gs.empty() && !gsm)) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[3] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    VkShaderModule mods[3] = {vsm, psm, gsm};
    VkShaderStageFlagBits kinds[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT};
    for (int i = 0; i < 3; i++) {
        stages[i].stage = kinds[i];
        stages[i].module = mods[i];
        stages[i].pName = "main";
    }
    u32 stride = (1 + d.attributes) * 16;
    VkVertexInputBindingDescription vb{0, stride, VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> va;
    for (u32 a = 0; a <= d.attributes; a++) va.push_back({a, 0, VK_FORMAT_R32G32B32A32_SFLOAT, a * 16});
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (!gpu_vertices) {  // the CPU's vertices, through vertex attributes
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = u32(va.size());
        vi.pVertexAttributeDescriptions = va.data();
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;  // the CPU's vertices: it culled them already
    if (gpu_vertices) {
        // The CPU's "counter-clockwise" (its area negative, y down) is Vulkan's too
        rs.cullMode = (d.cull & 1 ? VK_CULL_MODE_FRONT_BIT : 0) | (d.cull & 2 ? VK_CULL_MODE_BACK_BIT : 0);
        rs.frontFace = (d.cull >> 2) & 1 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.depthClampEnable = VK_TRUE;
    }
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkPipelineColorBlendAttachmentState> cbs(d.targets.size());
    std::vector<VkFormat> formats(d.targets.size());
    for (size_t i = 0; i < d.targets.size(); i++) {
        const Draw::Target& t = d.targets[i];
        VkPipelineColorBlendAttachmentState& cb = cbs[i];
        formats[i] = VkFormat(t.format);  // 0, VK_FORMAT_UNDEFINED: nothing at this place
        cb.colorWriteMask = t.format ? t.mask & 15 : 0;
        if (t.format && ((t.blend >> 30) & 1)) {
            u32 bc = t.blend;
            bool separate = (bc >> 29) & 1;
            u32 csrc = bc & 31, cfn = (bc >> 5) & 7, cdst = (bc >> 8) & 31;
            u32 asrc = separate ? (bc >> 16) & 31 : csrc, afn = separate ? (bc >> 21) & 7 : cfn, adst = separate ? (bc >> 24) & 31 : cdst;
            cb.blendEnable = VK_TRUE;
            if (!blend_factor(csrc, cb.srcColorBlendFactor) || !blend_factor(cdst, cb.dstColorBlendFactor) ||
                !blend_factor(asrc, cb.srcAlphaBlendFactor) || !blend_factor(adst, cb.dstAlphaBlendFactor) ||
                !blend_op(cfn, cb.colorBlendOp) || !blend_op(afn, cb.alphaBlendOp)) {
                why = std::format("blending {:#x}", bc);
                return VK_NULL_HANDLE;
            }
        }
    }
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bs.attachmentCount = u32(cbs.size());
    bs.pAttachments = cbs.data();
    VkDynamicState dyn3[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    ds.dynamicStateCount = 3;
    ds.pDynamicStates = dyn3;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = u32(formats.size());
    ri.pColorAttachmentFormats = formats.data();
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = gsm ? 3 : 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pColorBlendState = &bs;
    gi.pDynamicState = &ds;
    gi.layout = k.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &gi, nullptr, &p) != VK_SUCCESS) {
        why = "vkCreateGraphicsPipelines failed";
        return VK_NULL_HANDLE;
    }
    k.pipelines[key] = p;
    return p;
}

// nearest: an integer texture's, which can't be filtered
VkSampler sampler(const u32* s, bool nearest = false) {
    std::array<u32, 5> key = {s[0], s[1], s[2], s[3], nearest};
    auto it = k.samplers.find(key);
    if (it != k.samplers.end()) return it->second;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = ((s[2] >> 20) & 3) && !nearest ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = ((s[2] >> 22) & 3) && !nearest ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = address_mode(s[0] & 7);
    si.addressModeV = address_mode((s[0] >> 3) & 7);
    si.addressModeW = address_mode((s[0] >> 6) & 7);
    u32 border = (s[3] >> 30) & 3;
    si.borderColor = border == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    si.maxLod = 0;
    VkSampler v = VK_NULL_HANDLE;
    vkCreateSampler(g.device, &si, nullptr, &v);
    k.samplers[key] = v;
    return v;
}

// Textures not used lately go when there are more than the budget
void evict() {
    if (k.texture_bytes <= TEXTURE_BUDGET) return;
    std::vector<std::pair<u64, std::string>> by_age;
    for (auto& [key, im] : k.textures) by_age.emplace_back(im.used, key);
    std::sort(by_age.begin(), by_age.end());
    for (auto& [used, key] : by_age) {
        if (k.texture_bytes <= TEXTURE_BUDGET / 2) break;
        if (used >= k.draws) continue;  // the draw being recorded uses it
        GpuImage& im = k.textures[key];
        k.texture_bytes -= im.bytes;
        im.destroy();
        k.textures.erase(key);
        k.evicted++;
    }
}

// ---- the batch ----
//
// Draws and dispatches are recorded into one command buffer, which goes to the GPU, to be
// waited for, only when something needs what it did: a target read back into memory (or let
// go), --gpu-check's copies, the end of a command buffer of the game's (flush(), from the
// command processor), or a full batch. Until then, what each draw uploads stays in the upload
// buffer (a draw's place in it is its own), each has a descriptor set of its own, and images
// that go (a texture used once, textures let go to stay in the budget) go once it is done.

bool g_recording = false;
VkDeviceSize g_arena = 0;  // the upload buffer's bytes the batch has used
u32 g_sets = 0;  // its descriptor sets
std::vector<GpuImage> g_doomed;  // images to go once the batch is done
std::vector<VkImageView> g_doomed_views;  // and views

VkCommandBuffer batch() {
    if (!g_recording) {
        vkResetCommandBuffer(k.cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(k.cmd, &begin);
        g_recording = true;
    }
    return k.cmd;
}

// The batch to the GPU, and waited for; then what it used is free again
bool flush_batch() {
    if (!g_recording) return true;
    vkEndCommandBuffer(k.cmd);
    g_recording = false;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &k.cmd;
    vkResetFences(g.device, 1, &k.fence);
    bool ok = vkQueueSubmit(g.queue, 1, &si, k.fence) == VK_SUCCESS &&
              vkWaitForFences(g.device, 1, &k.fence, VK_TRUE, 30'000'000'000ull) == VK_SUCCESS;
    g_arena = 0;
    g_sets = 0;
    vkResetDescriptorPool(g.device, k.dpool, 0);
    for (auto& im : g_doomed) im.destroy();
    g_doomed.clear();
    for (VkImageView v : g_doomed_views) vkDestroyImageView(g.device, v, nullptr);
    g_doomed_views.clear();
    evict();
    k.batches++;
    return ok;
}

// For the next draw or dispatch: `bytes` of the upload buffer (from `base`) and a descriptor
// set, both its own until the batch is done (which may first be flushed, to make room)
bool reserve_batch(VkDeviceSize bytes, VkDeviceSize& base, VkDescriptorSet& set, std::string& why) {
    if (g_sets == SETS || g_arena + bytes > k.upload.size) flush_batch();
    if (!g_recording && bytes > k.upload.size &&
        !k.upload.reserve(std::max(ARENA, bytes * 2), VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) {
        why = "out of memory for the upload";
        return false;
    }
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = k.dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &k.set_layout;
    if (vkAllocateDescriptorSets(g.device, &dai, &set) != VK_SUCCESS) {
        why = "vkAllocateDescriptorSets failed";
        return false;
    }
    g_sets++;
    base = g_arena;
    g_arena = (g_arena + bytes + 255) & ~VkDeviceSize(255);
    return true;
}

// The GPU's picture back into the memory, which may then be read
bool read_back(Resident& r) {
    if (r.state != Resident::gpu) return true;
    if (!flush_batch() || !k.readback.reserve(r.bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    VkCommandBuffer cmd = batch();
    const VkPipelineStageFlags T = VK_PIPELINE_STAGE_TRANSFER_BIT, C = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier(cmd, r.image.image, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, C, T);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {r.width, r.height, 1};
    vkCmdCopyImageToBuffer(cmd, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, k.readback.buffer, 1, &copy);
    barrier(cmd, r.image.image, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, T, C);
    VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hb.buffer = k.readback.buffer;
    hb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, T, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
    if (!flush_batch()) return false;
    platform::protect(r.lo, r.hi - r.lo, 2);
    std::memcpy(reinterpret_cast<void*>(r.base), k.readback.p, r.bytes);
    platform::protect(r.lo, r.hi - r.lo, 1);
    r.state = Resident::same;
    k.readbacks++;
    return true;
}

// The target let go: back into the memory, which is left unguarded
void let_go(u64 base) {
    flush_batch();  // nothing waiting may still use it
    Resident& r = g_resident[base];
    read_back(r);
    platform::protect(r.lo, r.hi - r.lo, 2);
    if (r.uints) vkDestroyImageView(g.device, r.uints, nullptr);
    for (auto& [f, v] : r.views) vkDestroyImageView(g.device, v, nullptr);
    r.image.destroy();
    for (auto& [key, im] : r.copies) im.destroy();
    g_resident.erase(base);
    k.let_go++;
}

// Every kept target in the pages from lo to hi back into the memory
void read_back_pages(u64 lo, u64 hi) {
    for (auto& [base, r] : g_resident)
        if (r.lo < hi && lo < r.hi) read_back(r);
}

// Memory kept: the image at its address if it is the same format and size, else a new one, once
// any it shares pages with are let go. A new one's memory is the newer, to be taken up.
bool make_kept_image(Resident& r, std::string& why);
bool reshape(Resident& r, u32 format, u32 width, u32 rows, std::string& why);

Resident* keep(u64 address, u32 format, u32 width, u32 rows, u32 bpp, std::string& why) {
    u64 bytes = u64(width) * rows * bpp;
    u64 lo = address & ~u64(0xfff), hi = (address + bytes + 0xfff) & ~u64(0xfff);
    // the same memory kept in another shape (a clear of it as a buffer, then a draw into it):
    // made over on the GPU, rather than through memory
    if (auto it = g_resident.find(address);
        it != g_resident.end() && it->second.bpp == bpp && it->second.bytes == bytes && (it->second.width != width || it->second.height != rows))
        if (!reshape(it->second, format, width, rows, why)) return nullptr;
    std::vector<u64> in_way;
    for (auto& [base, r] : g_resident)
        if (r.lo < hi && lo < r.hi && !(base == address && r.bpp == bpp && r.width == width && r.height == rows))
            in_way.push_back(base);
    for (u64 base : in_way) let_go(base);
    auto it = g_resident.find(address);
    if (it != g_resident.end()) return &it->second;
    Resident r;
    r.base = address, r.bytes = bytes, r.lo = lo, r.hi = hi;
    r.format = format, r.width = width, r.height = rows, r.bpp = bpp;
    if (!make_kept_image(r, why)) return nullptr;
    return &(g_resident[address] = std::move(r));
}

// Kept memory's image, as its format and shape say, and its view as uints
bool make_kept_image(Resident& r, std::string& why) {
    if (!make_image(r.image, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VkFormat(r.format), r.width, r.height, 1, 1,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_STORAGE_BIT,
                    {}, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)) {
        why = "out of GPU memory";
        return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = r.image.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = r.bpp == 16 ? VK_FORMAT_R32G32B32A32_UINT : r.bpp == 8 ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32_UINT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(g.device, &vi, nullptr, &r.uints);
    return true;
}

// The same memory, given another shape: its texels in order, through a buffer, all on the GPU
// (if what it holds is still wanted: else it is simply made again, to be taken up)
bool reshape(Resident& r, u32 format, u32 width, u32 rows, std::string& why) {
    Resident n;
    n.format = format, n.width = width, n.height = rows, n.bpp = r.bpp;
    if (r.state != Resident::memory && r.bytes > k.scratch.size && !flush_batch()) return false;  // (the scratch buffer in use)
    if (!make_kept_image(n, why)) return false;
    if (r.state != Resident::memory) {
        if (!k.scratch.reserve(r.bytes)) {
            n.image.destroy();
            why = "out of GPU memory";
            return false;
        }
        VkCommandBuffer cmd = batch();
        const VkPipelineStageFlags T = VK_PIPELINE_STAGE_TRANSFER_BIT, C = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = k.scratch.buffer;
        bb.size = VK_WHOLE_SIZE;
        bb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;  // (an earlier reshape done reading it)
        bb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, T, T, 0, 0, nullptr, 1, &bb, 0, nullptr);
        barrier(cmd, r.image.image, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, C, T);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {r.width, r.height, 1};
        vkCmdCopyImageToBuffer(cmd, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, k.scratch.buffer, 1, &c);
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, T, T, 0, 0, nullptr, 1, &bb, 0, nullptr);
        barrier(cmd, n.image.image, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, T);
        c.imageExtent = {width, rows, 1};
        vkCmdCopyBufferToImage(cmd, k.scratch.buffer, n.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cmd, n.image.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, T, C);
    }
    // the old image goes once the batch is done
    g_doomed.push_back(r.image);
    if (r.uints) g_doomed_views.push_back(r.uints);
    for (auto& [f, v] : r.views) g_doomed_views.push_back(v);
    r.views.clear();
    r.image = n.image, r.uints = n.uints;
    r.format = format, r.width = width, r.height = rows;
    k.reshapes++;
    return true;
}

// The kept memory's image as that format (the same size of texel), for a draw into it
VkImageView view_as(Resident& r, u32 format) {
    if (format == r.format) return r.image.view;
    VkImageView& v = r.views[format];
    if (!v) {
        VkImageViewUsageCreateInfo vu{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        vu.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.pNext = &vu;
        vi.image = r.image.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VkFormat(format);
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(g.device, &vi, nullptr, &v);
    }
    return v;
}

// A draw's target, kept
Resident* resident(const Draw& d, const Draw::Target& t, std::string& why) {
    return keep(t.address, t.format, d.width, std::max(d.rows, d.height), t.bpp, why);
}

VkComponentSwizzle swizzle(u32 sel) {
    switch (sel) {
        case 0: return VK_COMPONENT_SWIZZLE_ZERO;
        case 1: return VK_COMPONENT_SWIZZLE_ONE;
        case 4: return VK_COMPONENT_SWIZZLE_R;
        case 5: return VK_COMPONENT_SWIZZLE_G;
        case 6: return VK_COMPONENT_SWIZZLE_B;
        default: return VK_COMPONENT_SWIZZLE_A;
    }
}

bool draw(Draw& d, std::string& why) {
    if (!prepare_kept()) {
        why = g.why;
        return false;
    }
    bool gpu_vertices = !d.indices.empty();
    if (d.textures.size() > IMAGES || d.vs_buffers.size() > BUFFERS || d.ps_buffers.size() > PS_BUFFERS) {
        why = d.textures.size() > IMAGES ? "more than 31 images"
              : d.vs_buffers.size() > BUFFERS ? "more than 31 vertex shader buffers" : "more than 16 pixel shader buffers";
        return false;
    }
    size_t nt = d.targets.size();
    bool kept = false;
    for (const auto& t : d.targets)
        if (t.format) kept = t.address != 0;
    VkPipeline pipe = pipeline(d, why);
    if (!pipe) return false;
    // the targets: kept, or --gpu-check's copies, by their format, size and place
    std::vector<Resident*> res(nt, nullptr);
    if (kept)
        for (size_t i = 0; i < nt; i++)
            if (d.targets[i].format && !(res[i] = resident(d, d.targets[i], why))) return false;
    // a texture read as it is in memory: copied on the GPU from a kept target there, if it is
    // that target laid out the same way; else any kept target there goes back into memory first
    std::vector<Resident*> from_target(d.textures.size(), nullptr);
    for (size_t i = 0; i < d.textures.size(); i++) {
        const Texture& t = d.textures[i];
        if (!t.raw) continue;
        auto it = g_resident.find(u64(t.raw));
        if (it != g_resident.end() && it->second.state != Resident::memory && t.type == 9 && t.layers == 1 &&
            t.bpp == it->second.bpp && t.pitch == it->second.width && t.width <= it->second.width && t.height <= it->second.height) {
            from_target[i] = &it->second;
            continue;
        }
        u64 at = u64(t.raw), end = at + u64(t.pitch) * t.height * t.layers * t.bpp;
        read_back_pages(at & ~u64(0xfff), (end + 0xfff) & ~u64(0xfff));
    }
    k.draws++;

    // everything going up, in one buffer: each piece's place in it
    VkDeviceSize at = 0;
    auto place = [&](VkDeviceSize bytes) {
        VkDeviceSize p = at;
        at = (at + std::max<VkDeviceSize>(bytes, 16) + 255) & ~VkDeviceSize(255);
        return p;
    };
    std::vector<VkDeviceSize> target_bytes(nt, 0), at_target(nt, 0), at_back(nt, 0);
    std::vector<bool> upload(nt, false);
    VkDeviceSize back_bytes = 0;
    for (size_t i = 0; i < nt; i++) {
        if (!d.targets[i].format) continue;
        target_bytes[i] = kept ? res[i]->bytes : VkDeviceSize(d.width) * d.height * d.targets[i].bpp;
        upload[i] = !kept || res[i]->state == Resident::memory;
        at_target[i] = place(upload[i] ? target_bytes[i] : 0);
        at_back[i] = back_bytes;
        back_bytes = (back_bytes + target_bytes[i] + 255) & ~VkDeviceSize(255);
    }
    VkDeviceSize at_values = place(d.values.size() * 4);
    VkDeviceSize at_vertices = place(d.vertices.size() * 4), at_indices = place(d.indices.size() * 4);
    VkDeviceSize at_vs_values = place(d.vs_values.size() * 4);
    std::vector<VkDeviceSize> at_buffers;
    for (auto& b : d.vs_buffers) at_buffers.push_back(place(b.size() * 4));
    std::vector<VkDeviceSize> at_ps_buffers;
    for (auto& b : d.ps_buffers) at_ps_buffers.push_back(place(b.size() * 4));
    // the textures not kept yet, made and uploaded
    std::vector<GpuImage*> images(d.textures.size());
    std::vector<VkDeviceSize> at_texels(d.textures.size(), ~VkDeviceSize(0));
    std::vector<GpuImage> unkept(d.textures.size());
    for (size_t i = 0; i < d.textures.size(); i++) {
        Texture& t = d.textures[i];
        if (Resident* r = from_target[i]) {  // an image kept for it, by its format, size and swizzle
            u32 sw = t.swizzle[0] | t.swizzle[1] << 3 | t.swizzle[2] << 6 | t.swizzle[3] << 9;
            GpuImage& im = r->copies[{t.format, t.width, t.height, sw}];
            VkComponentMapping map{swizzle(t.swizzle[0]), swizzle(t.swizzle[1]), swizzle(t.swizzle[2]), swizzle(t.swizzle[3])};
            if (!im.image && !make_image(im, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VkFormat(t.format), t.width, t.height, 1, 1,
                                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, map)) {
                why = "out of GPU memory";
                return false;
            }
            images[i] = &im;
            continue;
        }
        if (!t.key.empty()) {
            auto it = k.textures.find(t.key);
            if (it != k.textures.end()) {
                images[i] = &it->second;
                images[i]->used = k.draws;
                continue;
            }
            if (!t.format && t.rgba.empty()) {  // let go since has_texture() said it was kept
                why = "a texture let go meanwhile";
                return false;
            }
        }
        bool three = t.type == 10;
        u32 layers = three ? 1 : t.layers;
        VkFormat f = t.format ? VkFormat(t.format) : t.integer ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT;
        VkComponentMapping map{};
        if (t.format) map = {swizzle(t.swizzle[0]), swizzle(t.swizzle[1]), swizzle(t.swizzle[2]), swizzle(t.swizzle[3])};
        GpuImage& im = t.key.empty() ? unkept[i] : k.textures[t.key];
        if (!make_image(im, three ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                        three ? VK_IMAGE_VIEW_TYPE_3D : t.type == 11 || t.type == 12 || t.type == 13 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, f,
                        t.width, t.height, three ? t.layers : 1, layers, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, map)) {
            if (!t.key.empty()) k.textures.erase(t.key);
            why = "out of GPU memory";
            return false;
        }
        im.used = k.draws;
        if (!t.key.empty()) k.texture_bytes += im.bytes;
        images[i] = &im;
        at_texels[i] = place(t.format ? VkDeviceSize(t.pitch) * t.height * t.layers * t.bpp : t.rgba.size() * 4);
        k.texture_uploads++;
    }
    // its place in the batch: the offsets above are from its start
    VkDeviceSize base;
    VkDescriptorSet set;
    if (!kept && !flush_batch()) {  // --gpu-check's copies come back at once: nothing else waiting
        why = "the GPU didn't finish the draws before";
        return false;
    }
    if (!reserve_batch(at, base, set, why)) return false;
    if (!kept && !k.readback.reserve(back_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) {
        why = "out of memory for the readback";
        return false;
    }
    for (auto& a : at_target) a += base;
    for (auto& a : at_buffers) a += base;
    for (auto& a : at_ps_buffers) a += base;
    for (auto& a : at_texels)
        if (a != ~VkDeviceSize(0)) a += base;
    at_values += base, at_vertices += base, at_indices += base, at_vs_values += base;
    unsigned char* up = k.upload.p;
    for (size_t i = 0; i < nt; i++)
        if (d.targets[i].format && upload[i])
            std::memcpy(up + at_target[i], kept ? reinterpret_cast<const unsigned char*>(d.targets[i].address) : d.targets[i].target,
                        target_bytes[i]);
    std::memcpy(up + at_values, d.values.data(), d.values.size() * 4);
    std::memcpy(up + at_vertices, d.vertices.data(), d.vertices.size() * 4);
    std::memcpy(up + at_indices, d.indices.data(), d.indices.size() * 4);
    std::memcpy(up + at_vs_values, d.vs_values.data(), d.vs_values.size() * 4);
    for (size_t i = 0; i < d.vs_buffers.size(); i++) std::memcpy(up + at_buffers[i], d.vs_buffers[i].data(), d.vs_buffers[i].size() * 4);
    for (size_t i = 0; i < d.ps_buffers.size(); i++)
        std::memcpy(up + at_ps_buffers[i], d.ps_buffers[i].data(), d.ps_buffers[i].size() * 4);
    for (size_t i = 0; i < d.textures.size(); i++) {
        if (at_texels[i] == ~VkDeviceSize(0)) continue;
        const Texture& t = d.textures[i];
        if (t.format) std::memcpy(up + at_texels[i], t.raw, size_t(t.pitch) * t.height * t.layers * t.bpp);
        else std::memcpy(up + at_texels[i], t.rgba.data(), t.rgba.size() * 4);
    }

    std::vector<GpuImage*> target(nt, nullptr);
    for (size_t i = 0; i < nt; i++) {
        if (!d.targets[i].format) continue;
        if (kept) {
            target[i] = &res[i]->image;
            continue;
        }
        GpuImage& im = k.targets[{d.targets[i].format, d.width, d.height, u32(i)}];
        if (!im.image && !make_image(im, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VkFormat(d.targets[i].format), d.width, d.height, 1, 1,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            why = "out of GPU memory";
            return false;
        }
        target[i] = &im;
    }

    // the descriptors
    std::vector<VkDescriptorBufferInfo> binfo;
    std::vector<VkDescriptorImageInfo> iinfo;
    binfo.reserve(2 + d.vs_buffers.size() + d.ps_buffers.size());
    iinfo.reserve(d.textures.size());
    std::vector<VkWriteDescriptorSet> writes;
    auto write_buffer = [&](u32 binding, VkDeviceSize offset, VkDeviceSize bytes) {
        binfo.push_back({k.upload.buffer, offset, std::max<VkDeviceSize>(bytes, 16)});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &binfo.back();
        writes.push_back(w);
    };
    write_buffer(0, at_values, d.values.size() * 4);
    for (u32 i = 0; i < d.textures.size(); i++) {
        iinfo.push_back({sampler(d.textures[i].sampler, d.textures[i].integer), images[i]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = image_binding(i);
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &iinfo.back();
        writes.push_back(w);
    }
    if (gpu_vertices) {
        write_buffer(16, at_vs_values, d.vs_values.size() * 4);
        for (u32 i = 0; i < d.vs_buffers.size(); i++) write_buffer(i < 15 ? 17 + i : 72 + (i - 15), at_buffers[i], d.vs_buffers[i].size() * 4);
    }
    for (u32 i = 0; i < d.ps_buffers.size(); i++) write_buffer(56 + i, at_ps_buffers[i], d.ps_buffers[i].size() * 4);
    vkUpdateDescriptorSets(g.device, u32(writes.size()), writes.data(), 0, nullptr);

    // the commands, into the batch
    VkCommandBuffer cmd = batch();
    const VkPipelineStageFlags T = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkPipelineStageFlags C = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkAccessFlags RW = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // the textures that are kept targets, copied first (before this draw changes its own)
    for (size_t i = 0; i < d.textures.size(); i++) {
        Resident* r = from_target[i];
        if (!r) continue;
        GpuImage& im = *images[i];
        barrier(cmd, im.image, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, T);
        barrier(cmd, r->image.image, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, C, T);
        VkImageCopy ic{};
        ic.srcSubresource = ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        ic.extent = {d.textures[i].width, d.textures[i].height, 1};
        vkCmdCopyImage(cmd, r->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        barrier(cmd, r->image.image, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, RW, T, C);
        barrier(cmd, im.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, T, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
        k.target_textures++;
    }
    std::vector<VkRenderingAttachmentInfo> atts(nt, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO});
    for (size_t i = 0; i < nt; i++) {
        if (!target[i]) continue;  // nothing here: an attachment with no image
        VkBufferImageCopy copy{};
        copy.bufferOffset = at_target[i];
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {d.width, kept ? res[i]->height : d.height, 1};
        VkImage image = target[i]->image;
        if (upload[i]) {
            barrier(cmd, image, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, T);
            vkCmdCopyBufferToImage(cmd, k.upload.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier(cmd, image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    RW, T, C);
        } else {  // as the last draw or readback left it
            barrier(cmd, image, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, RW, C | T, C);
        }
        atts[i].imageView = kept ? view_as(*res[i], d.targets[i].format) : target[i]->view;
        atts[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    for (size_t i = 0; i < d.textures.size(); i++) {
        if (at_texels[i] == ~VkDeviceSize(0)) continue;
        const Texture& t = d.textures[i];
        GpuImage& im = *images[i];
        barrier(cmd, im.image, im.layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, T);
        VkBufferImageCopy c{};
        c.bufferOffset = at_texels[i];
        c.bufferRowLength = t.format ? t.pitch : 0;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, im.layers};
        c.imageExtent = {t.width, t.height, t.type == 10 ? t.layers : 1};
        vkCmdCopyBufferToImage(cmd, k.upload.buffer, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cmd, im.image, im.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, T,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
    }
    VkRenderingInfo rinfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rinfo.renderArea = {{0, 0}, {d.width, d.height}};
    rinfo.layerCount = 1;
    rinfo.colorAttachmentCount = u32(nt);
    rinfo.pColorAttachments = atts.data();
    vkCmdBeginRendering(cmd, &rinfo);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, k.layout, 0, 1, &set, 0, nullptr);
    VkViewport viewport{0, 0, float(d.width), float(d.height), 0, 1};
    VkRect2D scissor{{d.scissor[0], d.scissor[1]}, {u32(d.scissor[2] - d.scissor[0]), u32(d.scissor[3] - d.scissor[1])}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdSetBlendConstants(cmd, d.blend_color);
    if (gpu_vertices) {
        vkCmdPushConstants(cmd, k.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof d.viewport, d.viewport);
        vkCmdBindIndexBuffer(cmd, k.upload.buffer, at_indices, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, u32(d.indices.size()), 1, 0, 0, 0);
    } else {
        vkCmdBindVertexBuffers(cmd, 0, 1, &k.upload.buffer, &at_vertices);
        vkCmdDraw(cmd, u32(d.vertices.size() / ((1 + d.attributes) * 4)), 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
    if (!kept) {  // --gpu-check's copies come back now
        for (size_t i = 0; i < nt; i++) {
            if (!target[i]) continue;
            barrier(cmd, target[i]->image, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, C, T);
            VkBufferImageCopy copy{};
            copy.bufferOffset = at_back[i];
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {d.width, d.height, 1};
            vkCmdCopyImageToBuffer(cmd, target[i]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, k.readback.buffer, 1, &copy);
        }
        VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hb.buffer = k.readback.buffer;
        hb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, T, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
    }
    for (auto& im : unkept)
        if (im.image) g_doomed.push_back(im);  // once the batch is done
    if (!kept && !flush_batch()) {
        why = "the GPU didn't finish the draw";
        return false;
    }
    for (size_t i = 0; i < nt; i++) {
        if (!target[i]) continue;
        if (!kept) {
            std::memcpy(d.targets[i].result, k.readback.p + at_back[i], target_bytes[i]);
            continue;
        }
        if (upload[i]) k.target_uploads++;
        if (res[i]->state != Resident::gpu) {  // the memory is now the older: guarded from any touch
            platform::protect(res[i]->lo, res[i]->hi - res[i]->lo, 0);
            res[i]->state = Resident::gpu;
        }
    }
    return true;
}

}  // namespace

bool barycentric() {
    std::lock_guard lock(g_lock);
    return init() && g.barycentric;
}

bool has_texture(const std::string& key) {
    std::lock_guard lock(g_lock);
    return k.textures.count(key) != 0;
}

bool dispatch(Dispatch& d, std::string& why) {
    std::lock_guard lock(g_lock);
    struct Inside {
        Inside() { t_inside = true; }
        ~Inside() { t_inside = false; }
    } inside;
    auto t0 = std::chrono::steady_clock::now();
    if (!init() || !prepare_kept()) {
        why = g.why;
        return false;
    }
    if (d.stores.size() > STORES || d.loads.size() > LOADS || d.arrays.size() > PS_BUFFERS) {
        why = "more than 4 stores, 4 loads of kept memory or 16 other loads";
        return false;
    }
    if (d.cs.find("uint64_t") != std::string::npos && !g.int64) {
        why = "the GPU has no 64-bit integers";
        return false;
    }
    // the kept memory each load and store is in (a store makes it kept, if it isn't)
    auto in_kept = [](const Dispatch::Access& a) -> Resident* {
        for (auto& [base, r] : g_resident)
            if (a.address >= base && a.address + a.bytes <= base + r.bytes && r.bpp == a.bpp && (a.address - base) % a.bpp == 0 && r.uints)
                return &r;
        return nullptr;
    };
    std::vector<Resident*> from(d.loads.size()), into(d.stores.size());
    for (size_t i = 0; i < d.loads.size(); i++)
        if (!(from[i] = in_kept(d.loads[i]))) {
            why = "a load of kept memory no longer kept";
            return false;
        }
    for (size_t i = 0; i < d.stores.size(); i++) {
        const Dispatch::Access& s = d.stores[i];
        if ((into[i] = in_kept(s))) continue;
        // only whole pages: memory sharing a page with other memory would keep being let go,
        // as one or the other is touched
        if ((s.address | s.bytes) & 0xfff) {
            why = "a store into memory not kept on the GPU, not whole pages";
            return false;
        }
        // kept from now on: shaped as a load's image of the same size of element, if the
        // buffer is a number of its rows (a copy of it, say), else as a row as wide as can be
        u64 n = s.bytes / s.bpp;
        u32 width = 0;
        for (Resident* r : from)
            if (r->bpp == s.bpp && n % r->width == 0) width = r->width;
        if (!width)
            for (u32 w = u32(std::min<u64>(n, 16384)); w >= 1; w--)
                if (n % w == 0) {
                    width = w;
                    break;
                }
        if (n / width > 16384) {
            why = "a store into a buffer too big to keep as an image";
            return false;
        }
        static const u32 uint_format[17] = {0, 0, 0, 0, 98, 0, 0, 0, 101, 0, 0, 0, 0, 0, 0, 0, 107};  // R32_UINT, R32G32_, R32G32B32A32_
        if (!(into[i] = keep(s.address, uint_format[s.bpp], width, u32(n / width), s.bpp, why))) return false;
        k.regions++;
    }
    // (keeping memory may let go of other memory it overlaps: each image is found again)
    for (size_t i = 0; i < d.loads.size(); i++)
        if (!(from[i] = in_kept(d.loads[i]))) {
            why = "a load of memory let go";
            return false;
        }
    for (size_t i = 0; i < d.stores.size(); i++)
        if (!(into[i] = in_kept(d.stores[i]))) {
            why = "a store into memory let go";
            return false;
        }
    // their places in U: the first element, the width, the elements
    auto place = [&](u32 slot, const Dispatch::Access& a, const Resident& r) {
        if (d.values.size() < slot + 3) d.values.resize(slot + 3);
        d.values[slot] = u32((a.address - r.base) / a.bpp);
        d.values[slot + 1] = r.width;
        d.values[slot + 2] = a.records;
    };
    for (size_t i = 0; i < d.stores.size(); i++) place(d.store_slot + 3 * u32(i), d.stores[i], *into[i]);
    for (size_t i = 0; i < d.loads.size(); i++) place(d.load_slot + 3 * d.load_index[i], d.loads[i], *from[i]);
    auto it = k.compute.find(d.cs);
    VkPipeline pipe = it == k.compute.end() ? VK_NULL_HANDLE : it->second;
    if (!pipe) {
        VkShaderModule m = module(d.cs, shaderc_compute_shader, why);
        if (!m) return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = m;
        ci.stage.pName = "main";
        ci.layout = k.layout;
        if (vkCreateComputePipelines(g.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe) != VK_SUCCESS) {
            why = "vkCreateComputePipelines failed";
            return false;
        }
        k.compute[d.cs] = pipe;
    }
    // what goes up: U, the other loads' elements, and kept memory newer in memory than there
    std::set<Resident*> used(into.begin(), into.end());
    used.insert(from.begin(), from.end());
    VkDeviceSize at = 0;
    auto reserve = [&](VkDeviceSize bytes) {
        VkDeviceSize p = at;
        at = (at + std::max<VkDeviceSize>(bytes, 16) + 255) & ~VkDeviceSize(255);
        return p;
    };
    VkDeviceSize at_values = reserve(d.values.size() * 4);
    std::vector<VkDeviceSize> at_arrays;
    for (auto& a : d.arrays) at_arrays.push_back(reserve(a.size() * 4));
    std::map<Resident*, VkDeviceSize> at_upload;
    for (Resident* r : used)
        if (r->state == Resident::memory) at_upload[r] = reserve(r->bytes);
    VkDeviceSize base;
    VkDescriptorSet set;
    if (!reserve_batch(at, base, set, why)) return false;
    unsigned char* up = k.upload.p + base;
    std::memcpy(up + at_values, d.values.data(), d.values.size() * 4);
    for (size_t i = 0; i < d.arrays.size(); i++) std::memcpy(up + at_arrays[i], d.arrays[i].data(), d.arrays[i].size() * 4);
    for (auto& [r, a] : at_upload) std::memcpy(up + a, reinterpret_cast<const void*>(r->base), r->bytes);

    std::vector<VkDescriptorBufferInfo> bi;
    bi.reserve(1 + d.arrays.size());
    std::vector<VkDescriptorImageInfo> ii;
    ii.reserve(d.stores.size() + d.loads.size());
    std::vector<VkWriteDescriptorSet> writes;
    auto write_buffer = [&](u32 binding, VkDeviceSize offset, VkDeviceSize bytes) {
        bi.push_back({k.upload.buffer, base + offset, std::max<VkDeviceSize>(bytes, 16)});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi.back();
        writes.push_back(w);
    };
    auto write_image = [&](u32 binding, Resident* r) {
        ii.push_back({VK_NULL_HANDLE, r->uints, VK_IMAGE_LAYOUT_GENERAL});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &ii.back();
        writes.push_back(w);
    };
    write_buffer(0, at_values, d.values.size() * 4);
    for (size_t i = 0; i < d.stores.size(); i++) write_image(32 + u32(i), into[i]);
    for (size_t i = 0; i < d.loads.size(); i++) write_image(36 + u32(i), from[i]);
    for (size_t i = 0; i < d.arrays.size(); i++) write_buffer(56 + u32(i), at_arrays[i], d.arrays[i].size() * 4);
    vkUpdateDescriptorSets(g.device, u32(writes.size()), writes.data(), 0, nullptr);

    VkCommandBuffer cmd = batch();
    const VkPipelineStageFlags T = VK_PIPELINE_STAGE_TRANSFER_BIT, C = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               X = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const VkAccessFlags RW = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        SRW = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    for (Resident* r : used) {  // from as draws leave it (or from memory), for the shader to read and write
        if (at_upload.count(r)) {
            barrier(cmd, r->image.image, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, T);
            VkBufferImageCopy c{};
            c.bufferOffset = base + at_upload[r];
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {r->width, r->height, 1};
            vkCmdCopyBufferToImage(cmd, k.upload.buffer, r->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            barrier(cmd, r->image.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, SRW, T, X);
            k.target_uploads++;
        } else {
            barrier(cmd, r->image.image, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, SRW, C | T, X);
        }
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, d.groups[0], d.groups[1], d.groups[2]);
    for (Resident* r : used)
        barrier(cmd, r->image.image, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, RW, X, C);
    // the memory: older than what was written; the same as what was only taken up and read
    std::set<Resident*> written(into.begin(), into.end());
    for (Resident* r : used) {
        Resident::State s = written.count(r) ? Resident::gpu : Resident::same;
        if (r->state == s || (r->state == Resident::gpu && s == Resident::same)) continue;
        platform::protect(r->lo, r->hi - r->lo, s == Resident::gpu ? 0 : 1);
        r->state = s;
    }
    k.dispatches++;
    k.dispatch_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    return true;
}

bool kept(u64 address, u64 bytes, u32 bpp) {
    std::lock_guard lock(g_lock);
    for (auto& [base, r] : g_resident)
        if (address >= base && address + bytes <= base + r.bytes && r.bpp == bpp && (address - base) % bpp == 0) return true;
    return false;
}

bool flush() {
    std::lock_guard lock(g_lock);
    if (!g.ok) return true;
    struct Inside {
        Inside() { t_inside = true; }
        ~Inside() { t_inside = false; }
    } inside;
    return flush_batch();
}

bool touched(u64 addr, bool write) {
    if (t_inside) return false;  // this thread is in the middle of a draw
    std::lock_guard lock(g_lock);
    t_inside = true;
    bool ours = false;
    for (auto& [base, r] : g_resident) {
        if (addr < r.lo || addr >= r.hi) continue;
        ours = true;
        if (r.state == Resident::gpu && !read_back(r)) {
            ours = false;  // the GPU failed: the fault stands
        } else if (r.state == Resident::same && write) {  // the memory is now the newer
            platform::protect(r.lo, r.hi - r.lo, 2);
            r.state = Resident::memory;
            k.writes++;
        }
        break;  // (if another thread has dealt with it already, the access is simply tried again)
    }
    t_inside = false;
    return ours;
}

bool run(Draw& d, std::string& why) {
    std::lock_guard lock(g_lock);
    struct Inside {
        Inside() { t_inside = true; }
        ~Inside() { t_inside = false; }
    } inside;
    auto t0 = std::chrono::steady_clock::now();
    if (!init()) {
        why = g.why;
        return false;
    }
    if ((d.ps.find("uint64_t") != std::string::npos || d.vs.find("uint64_t") != std::string::npos) && !g.int64) {
        why = "the GPU has no 64-bit integers";
        return false;
    }
    bool ok = draw(d, why);
    k.ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    return ok;
}

std::string device() {
    std::lock_guard lock(g_lock);
    if (!g.ok) return g.tried ? "Vulkan couldn't be used: " + g.why : "";
    std::string s = "on the " + g.name;
    if (!g.validation) return s + " (the validation layer isn't installed)";
    if (u64 n = g_messages.load()) return s + std::format(" (the validation layer reported {} problems, the first: {})", n, g_first_message);
    return s + " (the validation layer reported no problems)";
}

std::string report() {
    std::lock_guard lock(g_lock);
    if (!k.draws) return {};
    return std::format("{} draws on the GPU took {:.1f} s, uploads included; {} pipelines; {} textures uploaded, {} kept at "
                       "the end ({} MiB), {} let go. Render targets kept there: {} at the end; taken up from memory {} times, "
                       "read back into it {} times (when touched), written there {} times, {} let go; read as textures {} times",
                       k.draws, double(k.ns) / 1e9, k.pipelines.size(), k.texture_uploads, k.textures.size(),
                       k.texture_bytes >> 20, k.evicted, g_resident.size(), k.target_uploads, k.readbacks, k.writes, k.let_go, k.target_textures) +
           (k.dispatches ? std::format(". {} compute dispatches there took {:.1f} s", k.dispatches, double(k.dispatch_ns) / 1e9) : "") +
           std::format(". Kept there because compute shaders stored into it: {} pieces of memory; given another shape there {} times. "
                       "Sent to the GPU in {} batches, each waited for", k.regions, k.reshapes, k.batches);
}

}  // namespace mensis::vkdraw

#else  // no Vulkan SDK where this was built

namespace mensis::vkdraw {
bool has_texture(const std::string&) { return false; }
bool barycentric() { return false; }
bool flush() { return true; }
bool kept(u64, u64, u32) { return false; }
bool touched(u64, bool) { return false; }
bool dispatch(Dispatch&, std::string& why) {
    why = "no Vulkan in this build (the Windows build has it)";
    return false;
}
bool run(Draw&, std::string& why) {
    why = "no Vulkan in this build (the Windows build has it)";
    return false;
}
std::string device() { return {}; }
std::string report() { return {}; }
}  // namespace mensis::vkdraw

#endif
