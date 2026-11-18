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

thread_local const char* t_doing = nullptr;

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
    bool validation = false, int64 = false, barycentric = false, bc = false;
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
    // the validation layer, if it is installed, for --gpu-check's runs (it checks every call
    // the loader makes, on the CPU: elsewhere it would cost the most of the GPU's time)
    for (auto& l : layers) g.validation |= M.gpu_check && std::strcmp(l.layerName, layer) == 0;
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
    want.textureCompressionBC = have.textureCompressionBC;  // textures sent as their compressed blocks
    g.bc = have.textureCompressionBC;
    if (!have.geometryShader || !have.depthClamp) return fail("a GPU without geometry shaders or depth clamping");
    // quad operations in pixel shaders (ds_swizzle_b32 in its quad mode, for derivatives)
    VkPhysicalDeviceSubgroupProperties sub{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 pr2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    pr2.pNext = &sub;
    vkGetPhysicalDeviceProperties2(g.gpu, &pr2);
    if (!(sub.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT) || !(sub.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT))
        return fail("a GPU without quad operations in pixel shaders");
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

u64 g_compile_ns = 0, g_compiled = 0;  // GLSL compiled into SPIR-V: the time, how many
bool compile(const std::string& src, shaderc_shader_kind kind, std::vector<u32>& out, std::string& why) {
    auto it = g.spirv.find(src);
    if (it != g.spirv.end()) {
        out = it->second;
        return true;
    }
    auto t0 = std::chrono::steady_clock::now();
    struct Tally {
        std::chrono::steady_clock::time_point t0;
        ~Tally() {
            g_compile_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            g_compiled++;
        }
    } tally{t0};
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(o, shaderc_optimization_level_zero);
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
             VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, layers};
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
    u64 version = 0;  // a copy of kept memory's: the version it was copied from
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
    VkImageAspectFlags view_aspect = format == VK_FORMAT_D32_SFLOAT_S8_UINT ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                     : format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                                                       : VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange = {view_aspect, 0, 1, 0, layers};
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
    u64 batches = 0;  // command buffers submitted (each waited for, if not then before the next)
    u64 regions = 0;  // memory kept on the GPU because a compute shader stored into it
    u64 reshapes = 0;  // kept memory given another shape (on the GPU)
    u64 depth_clears = 0;  // depth buffers cleared there
    u64 target_textures_same = 0;  // kept memory read as a texture, its copy still the same
    u64 layers_from_kept = 0;  // textures' layers copied from kept memory there
    u64 htile_checks = 0;  // depth buffers' fast clears seen to there (their HTILE buffers kept there)
    u64 arrays_kept = 0;  // compute shaders' arrays copied from kept memory there
    u64 ns = 0, dispatch_ns = 0;  // the time the draws and dispatches took, uploads included
    u64 wait_ns = 0, readback_ns = 0, upload_ns = 0;  // waiting for batches; reading targets back; copying uploads
    u64 upload_bytes = 0, texture_upload_bytes = 0;
    u64 buffer_upload_ns = 0, buffer_upload_bytes = 0;  // of which: the draws' buffers (vertices, indices, loads)
};
Kept k;

// The batch before, while it may still be on the GPU (busy): what was its, to be finished once
// it is done; else, those for the next batch (see "the batch", below)
struct Spare {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    HostBuffer upload;
    std::vector<GpuImage> doomed;
    std::vector<VkImageView> doomed_views;
    std::vector<const char*> what;
    u64 first_draw = 0;
    u32 queries = 0;
    bool busy = false;
} g_spare;
u64 g_first_draw = 0;  // the batch's first draw
u32 g_queries = 0;  // its timestamps' first, in the query pool
constexpr u32 TIMESTAMPS = 32768;  // a batch's at most (past that, the rest go uncounted)

// A render target in the game's memory, kept on the GPU: the pages it is in (lo to hi), and
// which is the newer, the GPU's picture or the memory (or neither)
struct Resident {
    enum State { gpu, same, memory };
    u64 base = 0, bytes = 0, lo = 0, hi = 0;
    u32 format = 0, width = 0, height = 0, bpp = 4;
    bool depth = false;  // a depth buffer (D16_UNORM, D32_SFLOAT, D32_SFLOAT_S8_UINT): no other views of it, no uints
    u64 sbase = 0, slo = 0, shi = 0;  // and its stencil's memory (8 bits a pixel), if it has one
    u64 version = 0;  // new each time anything writes it (from a count of all such writes)
    GpuImage image;
    VkImageView uints = VK_NULL_HANDLE;  // the same, each texel uints (bpp of them): for a compute shader's loads and stores
    State state = memory;
    // copies of it, for draws that read it as a texture: by format, width, height, swizzle, and
    // where in it the texture starts
    std::map<std::array<u64, 5>, GpuImage> copies;
    // views of it in other formats of the same size of texel, for a draw into it as one (the
    // memory a compute shader made kept, then drawn into as a render target, say)
    std::map<u32, VkImageView> views;
};
std::map<u64, Resident> g_resident;  // by address
u64 g_writes = 0;  // writes into kept memory, counted: each one's version
void written(Resident& r) { r.version = ++g_writes; }

bool is_depth(u32 format) {
    return format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}
// Kept memory's pages guarded (0: from any touch; 1: from writes; 2: not), the stencil's too
void guard(const Resident& r, int access) {
    platform::protect(r.lo, r.hi - r.lo, access);
    if (r.sbase) platform::protect(r.slo, r.shi - r.slo, access);
}
bool in_pages(const Resident& r, u64 lo, u64 hi) { return (r.lo < hi && lo < r.hi) || (r.sbase && r.slo < hi && lo < r.shi); }
// Where kept memory's image rests between uses, which part of it holds the memory, and what may
// have used it there last
VkImageLayout rest(const Resident& r) {
    return r.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
VkImageAspectFlags aspect(const Resident& r) {
    return r.sbase ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : r.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}
VkPipelineStageFlags rest_stage(const Resident& r) {
    return r.depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                   : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
}
VkAccessFlags rest_access(const Resident& r) {
    return r.depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                   : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
}
thread_local bool t_inside = false;  // this thread holds g_lock

constexpr u32 IMAGES = 31, BUFFERS = 63, PS_BUFFERS = 16;  // a pixel shader's textures (1-15, 40-55), buffers (56-71); a vertex shader's buffers (17-31, 72-119)
constexpr u32 STORES = 4, LOADS = 4;  // a compute shader's storage images: stores (32-35), loads (36-39)
constexpr u32 SETS = 2048;  // descriptor sets in a batch (one a draw or dispatch)
constexpr VkDeviceSize ARENA = VkDeviceSize(256) << 20;  // the upload buffer's least size
// The textures kept on the GPU: a quarter of its own memory (its largest device-local heap),
// at least 2 GiB. A texture let go may well be needed again, and decoded again.
VkDeviceSize texture_budget() {
    static VkDeviceSize budget = 0;
    if (!budget) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g.gpu, &mp);
        VkDeviceSize most = 0;
        for (u32 i = 0; i < mp.memoryHeapCount; i++)
            if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) most = std::max(most, mp.memoryHeaps[i].size);
        budget = std::max(VkDeviceSize(2) << 30, most / 4);
    }
    return budget;
}

u32 image_binding(u32 i) { return i < 15 ? 1 + i : 40 + (i - 15); }

// The draws' larger buffers (unpacked elements, from 64 KiB), kept on the GPU in one pool: a
// buffer the draw before used too (the same one, shared: the memory it came from unchanged)
// goes up once, not with every draw. The pool is a ring (an eighth of the GPU's memory, from
// 256 MiB to 2 GiB): a buffer goes in where the last went, and those it lands on are let go
// (a copy into the pool waits, in the batch, for the draws before that read it).
struct BufferPool {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0, head = 0;
    bool tried = false;
    struct Entry {
        std::weak_ptr<const std::vector<u32>> data;
        VkDeviceSize at = 0, bytes = 0;
        u64 draw = 0;  // the last draw that used it
    };
    std::unordered_map<const std::vector<u32>*, Entry> entries;
    std::map<VkDeviceSize, const std::vector<u32>*> by_place;
    u64 sent = 0, found = 0, let_go = 0, sent_bytes = 0;
} g_pool;
constexpr VkDeviceSize POOLED = 64 << 10;

bool pool_ready() {
    if (g_pool.buffer) return true;
    if (g_pool.tried) return false;
    g_pool.tried = true;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g.gpu, &mp);
    VkDeviceSize most = 0;
    for (u32 i = 0; i < mp.memoryHeapCount; i++)
        if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) most = std::max(most, mp.memoryHeaps[i].size);
    VkDeviceSize size = std::clamp(most / 8, VkDeviceSize(256) << 20, VkDeviceSize(2) << 30);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(g.device, &bi, nullptr, &g_pool.buffer) != VK_SUCCESS) return g_pool.buffer = VK_NULL_HANDLE, false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g.device, g_pool.buffer, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == ~0u || vkAllocateMemory(g.device, &ai, nullptr, &g_pool.memory) != VK_SUCCESS) {
        vkDestroyBuffer(g.device, g_pool.buffer, nullptr);
        g_pool.buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(g.device, g_pool.buffer, g_pool.memory, 0);
    g_pool.size = size;
    return true;
}

// A buffer's place in the pool, for the draw `draw`: true if it has one (send: it must first be
// copied there); false if it goes up with the draw, as the smaller ones do
bool pooled(const Buffer& b, u64 draw, VkDeviceSize& at, bool& send) {
    VkDeviceSize bytes = (VkDeviceSize(b->size()) * 4 + 255) & ~VkDeviceSize(255);
    if (bytes < POOLED || !pool_ready() || bytes > g_pool.size / 4) return false;
    auto it = g_pool.entries.find(b.get());
    if (it != g_pool.entries.end()) {
        if (it->second.data.lock() == b) {
            it->second.draw = draw;
            at = it->second.at;
            send = false;
            g_pool.found++;
            return true;
        }
        g_pool.by_place.erase(it->second.at);  // another buffer, since gone, was at that address
        g_pool.entries.erase(it);
    }
    if (g_pool.head + bytes > g_pool.size) g_pool.head = 0;
    VkDeviceSize lo = g_pool.head, hi = lo + bytes;
    // those in the way: let go, unless this draw uses one (then this one goes up with it)
    std::vector<const std::vector<u32>*> doomed;
    auto p = g_pool.by_place.lower_bound(lo);
    if (p != g_pool.by_place.begin()) --p;
    for (; p != g_pool.by_place.end() && p->first < hi; ++p) {
        const auto& e = g_pool.entries[p->second];
        if (e.at + e.bytes <= lo) continue;
        if (e.draw == draw) return false;
        doomed.push_back(p->second);
    }
    for (auto* key : doomed) {
        g_pool.by_place.erase(g_pool.entries[key].at);
        g_pool.entries.erase(key);
        g_pool.let_go++;
    }
    g_pool.entries[b.get()] = {b, lo, bytes, draw};
    g_pool.by_place[lo] = b.get();
    g_pool.head = hi;
    g_pool.sent++;
    g_pool.sent_bytes += bytes;
    at = lo;
    send = true;
    return true;
}

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
    if (vkCreateDescriptorPool(g.device, &dpi, nullptr, &k.dpool) != VK_SUCCESS ||
        vkCreateDescriptorPool(g.device, &dpi, nullptr, &g_spare.dpool) != VK_SUCCESS)
        return fail("vkCreateDescriptorPool failed");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(g.device, &cai, &k.cmd);
    vkAllocateCommandBuffers(g.device, &cai, &g_spare.cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(g.device, &fci, nullptr, &k.fence);
    vkCreateFence(g.device, &fci, nullptr, &g_spare.fence);
    g_spare.queries = TIMESTAMPS;
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
    // (the shaders by their hashes: made once where they are kept, if they are, as most are)
    std::string key = std::format("{:x}:{:x}:{:x}:{:x}:{}:{}", d.vs_id ? d.vs_id : h(d.vs_text()), h(d.gs), d.ps_id ? d.ps_id : h(d.ps_text()), d.cull,
                                  d.attributes, gpu_vertices);
    for (const auto& t : d.targets) key += std::format(":{}.{:x}.{:x}", t.format, t.blend, t.mask);
    key += std::format(":z{}.{}.{}.{}", d.depth.format, d.depth.test, d.depth.write, d.depth.func);
    if (d.depth.stencil_test)
        for (const auto* f : {&d.depth.front, &d.depth.back})
            key += std::format(":s{}.{}.{}.{}.{}.{}.{}", f->fail, f->pass, f->zfail, f->func, f->cmask, f->wmask, f->ref);
    auto it = k.pipelines.find(key);
    if (it != k.pipelines.end()) return it->second;
    VkShaderModule vsm = module(d.vs_text(), shaderc_vertex_shader, why), psm = module(d.ps_text(), shaderc_fragment_shader, why);
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
    ri.depthAttachmentFormat = VkFormat(d.depth.format);
    VkPipelineDepthStencilStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dsi.depthTestEnable = d.depth.format && d.depth.test;
    dsi.depthWriteEnable = d.depth.format && d.depth.write;
    dsi.depthCompareOp = VkCompareOp(d.depth.func);  // the console's order is Vulkan's
    dsi.stencilTestEnable = d.depth.stencil_test;
    auto face = [](const Draw::Depth::Face& f) {
        return VkStencilOpState{VkStencilOp(f.fail), VkStencilOp(f.pass), VkStencilOp(f.zfail), VkCompareOp(f.func), f.cmask, f.wmask, f.ref};
    };
    dsi.front = face(d.depth.front);
    dsi.back = face(d.depth.back);
    if (d.depth.stencil) ri.stencilAttachmentFormat = VkFormat(d.depth.format);
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
    gi.pDepthStencilState = &dsi;
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

// A texture's texels' bytes, as uploaded
VkDeviceSize texel_bytes(const Texture& t) {
    if (t.block) return t.blocks.size();
    return t.format ? VkDeviceSize(t.pitch) * t.height * t.layers * t.bpp : t.rgba.size() * 4;
}

// Textures not used lately go when there are more than the budget (not those used by draws
// from `from` on: in a batch not yet done)
void evict(u64 from) {
    if (k.texture_bytes <= texture_budget()) return;
    std::vector<std::pair<u64, std::string>> by_age;
    for (auto& [key, im] : k.textures) by_age.emplace_back(im.used, key);
    std::sort(by_age.begin(), by_age.end());
    for (auto& [used, key] : by_age) {
        if (k.texture_bytes <= texture_budget() / 2) break;
        if (used >= from) continue;
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
// go), --gpu-check's copies, or a full batch. Until then, what each draw uploads stays in the
// upload buffer (a draw's place in it is its own), each has a descriptor set of its own, and
// images that go (a texture used once, textures let go to stay in the budget) go once it is
// done.
//
// At the end of a command buffer of the game's (flush(), from the command processor), the
// batch goes without waiting: the next is recorded while the GPU does it, with the spare's
// command buffer, fence, upload buffer and descriptor pool (the two swapped). Nothing needs
// what it did yet: whatever reads it (the memory touched, a read-back) waits for it then.
// Before the next goes, it is waited for (so at most one is on the GPU unwaited).

bool g_recording = false;
VkDeviceSize g_arena = 0;  // the upload buffer's bytes the batch has used
u32 g_sets = 0;  // its descriptor sets
std::vector<GpuImage> g_doomed;  // images to go once the batch is done
std::vector<VkImageView> g_doomed_views;  // and views

// The GPU's time, measured: a timestamp after each part of each draw and dispatch, the time
// from the one before it counted to that part (by what it is), once the batch is done
struct Timing {
    VkQueryPool pool = VK_NULL_HANDLE;
    bool tried = false;
    double period = 1;  // nanoseconds a tick
    u64 mask = ~0ull;  // the bits a timestamp has
    std::vector<const char*> what;  // the batch's timestamps (the first: its start)
    std::map<std::string, u64> ns;
} g_time;

bool timing_ready() {
    if (g_time.pool) return true;
    if (g_time.tried) return false;
    g_time.tried = true;
    if (g.validation) return false;  // (with it, the timestamps would cost more than they tell)
    u32 qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.gpu, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(g.gpu, &qn, qs.data());
    u32 bits = g.family < qn ? qs[g.family].timestampValidBits : 0;
    if (!bits) return false;
    VkPhysicalDeviceProperties pr;
    vkGetPhysicalDeviceProperties(g.gpu, &pr);
    g_time.period = pr.limits.timestampPeriod;
    g_time.mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 2 * TIMESTAMPS;  // (the batch's, and the spare's)
    if (vkCreateQueryPool(g.device, &qi, nullptr, &g_time.pool) != VK_SUCCESS) g_time.pool = VK_NULL_HANDLE;
    return g_time.pool != VK_NULL_HANDLE;
}

// The GPU's work recorded since the last mark: `what` (outside a render pass's draws)
void mark(VkCommandBuffer cmd, const char* what) {
    if (!g_time.pool || g_time.what.empty() || g_time.what.size() >= TIMESTAMPS) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_time.pool, g_queries + u32(g_time.what.size()));
    g_time.what.push_back(what);
}

VkCommandBuffer batch() {
    if (!g_recording) {
        vkResetCommandBuffer(k.cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(k.cmd, &begin);
        g_recording = true;
        g_first_draw = k.draws;
        g_time.what.clear();
        if (timing_ready()) {
            vkCmdResetQueryPool(k.cmd, g_time.pool, g_queries, TIMESTAMPS);
            vkCmdWriteTimestamp(k.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_time.pool, g_queries);
            g_time.what.push_back("start");
        }
    }
    return k.cmd;
}

// A batch done: its timestamps counted, its descriptor sets and the images it was the last to
// use let go
void finished(std::vector<const char*>& what, u32 queries, VkDescriptorPool dpool, std::vector<GpuImage>& doomed,
              std::vector<VkImageView>& doomed_views) {
    if (what.size() > 1) {
        std::vector<u64> ts(what.size());
        if (vkGetQueryPoolResults(g.device, g_time.pool, queries, u32(ts.size()), ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT) ==
            VK_SUCCESS)
            for (size_t i = 1; i < ts.size(); i++) {
                u64 d = (ts[i] - ts[i - 1]) & g_time.mask;
                if (d > g_time.mask / 2) d = 0;  // (one that finished before the one before it)
                g_time.ns[what[i]] += u64(double(d) * g_time.period);
            }
    }
    what.clear();
    vkResetDescriptorPool(g.device, dpool, 0);
    for (auto& im : doomed) im.destroy();
    doomed.clear();
    for (VkImageView v : doomed_views) vkDestroyImageView(g.device, v, nullptr);
    doomed_views.clear();
}

// The batch before, if not yet waited for: waited for, and finished
bool wait_spare() {
    if (!g_spare.busy) return true;
    bool ok = vkWaitForFences(g.device, 1, &g_spare.fence, VK_TRUE, 30'000'000'000ull) == VK_SUCCESS;
    g_spare.busy = false;
    finished(g_spare.what, g_spare.queries, g_spare.dpool, g_spare.doomed, g_spare.doomed_views);
    return ok;
}

// The batch to the GPU, and (`wait`) waited for, with the one before; then what they used is
// free again. Else it goes on without waiting (the one before waited for)
// Why batches went, and the targets read back (by why, and what touched them), counted
std::map<std::string, u64> g_batch_why, g_readback_why;
std::map<std::string, std::pair<u64, u64>> g_readback_ns;  // and by why alone: how many, and the time they took (waits included)
bool flush_batch(const char* why = "other", bool wait = true) {
    auto t0 = std::chrono::steady_clock::now();
    auto waited = [&] { k.wait_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()); };
    if (!g_recording) {
        bool ok = !wait || wait_spare();
        waited();
        return ok;
    }
    g_batch_why[why]++;
    vkEndCommandBuffer(k.cmd);
    g_recording = false;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &k.cmd;
    vkResetFences(g.device, 1, &k.fence);
    bool ok = vkQueueSubmit(g.queue, 1, &si, k.fence) == VK_SUCCESS;
    ok = wait_spare() && ok;
    if (wait || !ok) {
        ok = ok && vkWaitForFences(g.device, 1, &k.fence, VK_TRUE, 30'000'000'000ull) == VK_SUCCESS;
        finished(g_time.what, g_queries, k.dpool, g_doomed, g_doomed_views);
    } else {  // this batch now the spare's, still on the GPU; the spare's (done) for the next
        std::swap(k.cmd, g_spare.cmd);
        std::swap(k.fence, g_spare.fence);
        std::swap(k.dpool, g_spare.dpool);
        std::swap(k.upload, g_spare.upload);
        std::swap(g_doomed, g_spare.doomed);
        std::swap(g_doomed_views, g_spare.doomed_views);
        std::swap(g_time.what, g_spare.what);
        std::swap(g_first_draw, g_spare.first_draw);
        std::swap(g_queries, g_spare.queries);
        g_spare.busy = true;
    }
    waited();
    g_arena = 0;
    g_sets = 0;
    evict(g_spare.busy ? g_spare.first_draw : k.draws);  // (k.draws: the draw being recorded, if one is)
    k.batches++;
    return ok;
}

// For the next draw or dispatch: `bytes` of the upload buffer (from `base`) and a descriptor
// set, both its own until the batch is done (which may first be flushed, to make room)
bool reserve_batch(VkDeviceSize bytes, VkDeviceSize& base, VkDescriptorSet& set, std::string& why) {
    if (g_sets == SETS || g_arena + bytes > k.upload.size) flush_batch("the batch full");
    if (!g_recording && bytes > k.upload.size &&
        !k.upload.reserve(std::max(ARENA, bytes * 2), VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
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

// The GPU's picture back into the memory, which may then be read; `why` for the count
bool read_back(Resident& r, const std::string& why) {
    auto t0 = std::chrono::steady_clock::now();
    struct Done {
        std::chrono::steady_clock::time_point t0;
        std::pair<u64, u64>* by = nullptr;
        ~Done() {
            u64 ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            k.readback_ns += ns;
            if (by) by->first++, by->second += ns;
        }
    } done{t0};
    if (r.state != Resident::gpu) return true;
    done.by = &g_readback_ns[why];
    VkDeviceSize sbytes = r.sbase ? VkDeviceSize(r.width) * r.height : 0, sat = (r.bytes + 255) & ~VkDeviceSize(255);
    g_readback_why[std::format("{} ({}x{} at {:#x})", why, r.width, r.height, r.base)]++;
    // the copy back at the end of the batch (after what it drew there), the two waited for at once
    // (the read-back buffer isn't in use: only a waited-for batch uses it)
    if (!k.readback.reserve(sat + sbytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    VkCommandBuffer cmd = batch();
    const VkPipelineStageFlags T = VK_PIPELINE_STAGE_TRANSFER_BIT;
    barrier(cmd, r.image.image, 1, rest(r), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rest_access(r), VK_ACCESS_TRANSFER_READ_BIT, rest_stage(r), T,
            aspect(r));
    VkBufferImageCopy copy{};
    copy.imageSubresource = {r.sbase ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : aspect(r), 0, 0, 1};
    copy.imageExtent = {r.width, r.height, 1};
    vkCmdCopyImageToBuffer(cmd, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, k.readback.buffer, 1, &copy);
    if (r.sbase) {
        copy.bufferOffset = sat;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        vkCmdCopyImageToBuffer(cmd, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, k.readback.buffer, 1, &copy);
    }
    barrier(cmd, r.image.image, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rest(r), VK_ACCESS_TRANSFER_READ_BIT, rest_access(r), T, rest_stage(r),
            aspect(r));
    VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hb.buffer = k.readback.buffer;
    hb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, T, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
    mark(cmd, "reading kept memory back");
    if (!flush_batch("reading a target back")) return false;
    guard(r, 2);
    std::memcpy(reinterpret_cast<void*>(r.base), k.readback.p, r.bytes);
    if (r.sbase) std::memcpy(reinterpret_cast<void*>(r.sbase), k.readback.p + sat, sbytes);
    guard(r, 1);
    r.state = Resident::same;
    k.readbacks++;
    return true;
}

// The target let go: back into the memory, which is left unguarded
void let_go(u64 base) {
    flush_batch("a target let go");  // nothing waiting may still use it
    Resident& r = g_resident[base];
    read_back(r, "let go");
    guard(r, 2);
    if (r.uints) vkDestroyImageView(g.device, r.uints, nullptr);
    for (auto& [f, v] : r.views) vkDestroyImageView(g.device, v, nullptr);
    r.image.destroy();
    for (auto& [key, im] : r.copies) im.destroy();
    g_resident.erase(base);
    k.let_go++;
}

// Every kept target in the pages from lo to hi back into the memory
void read_back_pages(u64 lo, u64 hi, const char* why) {
    for (auto& [base, r] : g_resident)
        if (in_pages(r, lo, hi)) read_back(r, why);
}

// Memory kept: the image at its address if it is the same format and size, else a new one, once
// any it shares pages with are let go. A new one's memory is the newer, to be taken up.
bool make_kept_image(Resident& r, std::string& why);
bool reshape(Resident& r, u32 format, u32 width, u32 rows, std::string& why);

// The memory kept on the GPU that holds a texture's layer, in rows of `pitch` texels, as it is
// there (the newer, or the same as the memory); else none
Resident* layer_kept(u64 address, u32 pitch, u32 width, u32 height, u32 bpp) {
    u64 bytes = (u64(height - 1) * pitch + width) * bpp;
    for (auto& [base, r] : g_resident)
        if (address >= base && address + bytes <= base + r.bytes && r.bpp == bpp && (address - base) % bpp == 0 && !r.depth &&
            !r.sbase && r.state != Resident::memory)
            return &r;
    return nullptr;
}


Resident* keep(u64 address, u32 format, u32 width, u32 rows, u32 bpp, std::string& why, u64 sbase = 0) {
    u64 bytes = u64(width) * rows * bpp;
    u64 lo = address & ~u64(0xfff), hi = (address + bytes + 0xfff) & ~u64(0xfff);
    u64 slo = sbase & ~u64(0xfff), shi = (sbase + u64(width) * rows + 0xfff) & ~u64(0xfff);
    // the same memory kept in another shape (a clear of it as a buffer, then a draw into it):
    // made over on the GPU, rather than through memory
    if (auto it = g_resident.find(address);
        it != g_resident.end() && it->second.bpp == bpp && it->second.bytes == bytes && !it->second.sbase && !sbase &&
        (it->second.width != width || it->second.height != rows || it->second.depth != is_depth(format)))
        if (!reshape(it->second, format, width, rows, why)) return nullptr;
    std::vector<u64> in_way;
    for (auto& [base, r] : g_resident)
        if ((in_pages(r, lo, hi) || (sbase && in_pages(r, slo, shi))) &&
            !(base == address && r.bpp == bpp && r.width == width && r.height == rows && r.sbase == sbase))
            in_way.push_back(base);
    for (u64 base : in_way) let_go(base);
    auto it = g_resident.find(address);
    if (it != g_resident.end()) return &it->second;
    Resident r;
    r.base = address, r.bytes = bytes, r.lo = lo, r.hi = hi;
    r.format = format, r.width = width, r.height = rows, r.bpp = bpp, r.depth = is_depth(format);
    if (sbase) r.sbase = sbase, r.slo = slo, r.shi = shi;
    if (!make_kept_image(r, why)) return nullptr;
    return &(g_resident[address] = std::move(r));
}

// Kept memory's image, as its format and shape say, and its view as uints (not a depth buffer's)
bool make_kept_image(Resident& r, std::string& why) {
    if (r.depth) {
        if (!make_image(r.image, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VkFormat(r.format), r.width, r.height, 1, 1,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            why = "out of GPU memory";
            return false;
        }
        return true;
    }
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
    n.format = format, n.width = width, n.height = rows, n.bpp = r.bpp, n.depth = is_depth(format);
    if (r.state != Resident::memory && r.bytes > k.scratch.size && !flush_batch("the scratch buffer")) return false;  // (the scratch buffer in use)
    if (!make_kept_image(n, why)) return false;
    if (r.state != Resident::memory) {
        if (!k.scratch.reserve(r.bytes)) {
            n.image.destroy();
            why = "out of GPU memory";
            return false;
        }
        VkCommandBuffer cmd = batch();
        const VkPipelineStageFlags T = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = k.scratch.buffer;
        bb.size = VK_WHOLE_SIZE;
        bb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;  // (an earlier reshape done reading it)
        bb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, T, T, 0, 0, nullptr, 1, &bb, 0, nullptr);
        barrier(cmd, r.image.image, 1, rest(r), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rest_access(r), VK_ACCESS_TRANSFER_READ_BIT, rest_stage(r),
                T, aspect(r));
        VkBufferImageCopy c{};
        c.imageSubresource = {aspect(r), 0, 0, 1};
        c.imageExtent = {r.width, r.height, 1};
        vkCmdCopyImageToBuffer(cmd, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, k.scratch.buffer, 1, &c);
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, T, T, 0, 0, nullptr, 1, &bb, 0, nullptr);
        barrier(cmd, n.image.image, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, T, aspect(n));
        c.imageSubresource = {aspect(n), 0, 0, 1};
        c.imageExtent = {width, rows, 1};
        vkCmdCopyBufferToImage(cmd, k.scratch.buffer, n.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cmd, n.image.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rest(n), VK_ACCESS_TRANSFER_WRITE_BIT, rest_access(n), T, rest_stage(n),
                aspect(n));
        mark(cmd, "kept memory given another shape");
    }
    // the old image goes once the batch is done
    g_doomed.push_back(r.image);
    if (r.uints) g_doomed_views.push_back(r.uints);
    for (auto& [f, v] : r.views) g_doomed_views.push_back(v);
    r.views.clear();
    r.image = n.image, r.uints = n.uints;
    r.format = format, r.width = width, r.height = rows, r.depth = n.depth;
    k.reshapes++;
    return true;
}

// The kept memory's image as that format (the same size of texel), for a draw into it
VkImageView view_as(Resident& r, u32 format) {
    if (format == r.format || r.depth) return r.image.view;
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

// The CPU's time in a draw on the GPU, by part (each part from its mark to the next; waits
// for the GPU in it included)
std::map<std::string, u64> g_part_ns;
struct Parts {
    const char* what;
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    explicit Parts(const char* w) : what(w) {}
    void to(const char* w) {
        auto now = std::chrono::steady_clock::now();
        g_part_ns[what] += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count());
        what = w, t = now;
    }
    ~Parts() { to(nullptr); }
};

bool draw(Draw& d, std::string& why) {
    Parts part("the pipeline, found");
    if (!prepare_kept()) {
        why = g.why;
        return false;
    }
    bool gpu_vertices = !d.indices.empty();
    if (d.textures.size() > IMAGES || d.vs_buffers.size() > BUFFERS || d.ps_buffers.size() > PS_BUFFERS) {
        why = d.textures.size() > IMAGES ? "more than 31 images"
              : d.vs_buffers.size() > BUFFERS ? "more than 63 vertex shader buffers" : "more than 16 pixel shader buffers";
        return false;
    }
    size_t nt = d.targets.size();
    bool kept = d.depth.format && d.depth.address;
    for (const auto& t : d.targets)
        if (t.format) kept = t.address != 0;
    size_t pipelines = k.pipelines.size();
    VkPipeline pipe = pipeline(d, why);
    if (k.pipelines.size() != pipelines) part.what = "the pipeline, made (its shaders compiled)";
    if (!pipe) return false;
    part.to("the targets and depth buffer, found or made");
    // the depth buffer, then the targets: kept, or --gpu-check's copies, by their format, size
    // and place
    Resident* zres = nullptr;
    if (kept && d.depth.format &&
        !(zres = keep(d.depth.address, d.depth.format, d.depth.width, d.depth.height, d.depth.bpp, why, d.depth.stencil ? d.depth.saddress : 0)))
        return false;
    std::vector<Resident*> res(nt, nullptr);
    if (kept)
        for (size_t i = 0; i < nt; i++)
            if (d.targets[i].format && !(res[i] = resident(d, d.targets[i], why))) return false;
    part.to("textures in kept memory, found (or read back)");
    // a texture read as it is in memory: copied on the GPU from kept memory there (of the same
    // size of texel): straight from its image if it is that image laid out the same way, else
    // through a buffer, the image's texels in order, the texture's rows picked out of them (a
    // depth buffer's depth, read as a texture, or a texture in memory a compute shader wrote);
    // else whatever is kept there goes back into memory first
    std::vector<Resident*> from_target(d.textures.size(), nullptr);
    std::vector<bool> direct(d.textures.size(), false);
    std::vector<VkDeviceSize> offset(d.textures.size(), 0);
    VkDeviceSize scratch_needed = 0;
    for (size_t i = 0; i < d.textures.size(); i++) {
        const Texture& t = d.textures[i];
        if (!t.raw) continue;
        u64 at = u64(t.raw), end = at + u64(t.pitch) * t.height * t.layers * t.bpp;
        Resident* r = nullptr;
        for (auto& [base, kr] : g_resident)
            if (at >= base && end <= base + kr.bytes && kr.bpp == t.bpp && (at - base) % t.bpp == 0) r = &kr;
        if (r && r->state != Resident::memory && t.type == 9 && t.layers == 1) {
            from_target[i] = r;
            offset[i] = at - r->base;
            direct[i] = !r->depth && at == r->base && t.pitch == r->width && t.width <= r->width && t.height <= r->height;
            if (!direct[i]) scratch_needed = std::max<VkDeviceSize>(scratch_needed, r->bytes);
            continue;
        }
        read_back_pages(at & ~u64(0xfff), (end + 0xfff) & ~u64(0xfff), "a texture in its pages, not of it");
    }
    // textures' layers in kept memory: each from its image, through the scratch buffer
    std::vector<std::vector<Resident*>> kept_from(d.textures.size());
    for (size_t i = 0; i < d.textures.size(); i++)
        for (const auto& kl : d.textures[i].kept_layers) {
            const Texture& t = d.textures[i];
            Resident* r = layer_kept(kl.address, t.kept_pitch, t.width, t.height, t.bpp);
            if (!r) {
                why = "a texture's layer in kept memory let go meanwhile";
                return false;
            }
            kept_from[i].push_back(r);
            scratch_needed = std::max<VkDeviceSize>(scratch_needed, r->bytes);
        }
    if (scratch_needed > k.scratch.size && (!flush_batch("the scratch buffer") || !k.scratch.reserve(scratch_needed))) {  // (not in use, to be made bigger)
        why = "out of GPU memory";
        return false;
    }
    k.draws++;

    part.to("the upload's places, the pool's, the textures made");
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
    // the depth buffer, if it goes up: always a copy's; kept memory's if the memory is the newer
    VkDeviceSize zbytes = VkDeviceSize(d.depth.width) * d.depth.height * d.depth.bpp;
    bool zupload = d.depth.format && (!kept || (zres && zres->state == Resident::memory));
    // --gpu-check's: the depth written, back too
    bool zback = !kept && d.depth.format && d.depth.write && d.depth.result;
    VkDeviceSize at_zback = back_bytes;
    if (zback) back_bytes = (back_bytes + zbytes + 255) & ~VkDeviceSize(255);
    VkDeviceSize at_depth = place(zupload ? zbytes : 0);
    VkDeviceSize sbytes = d.depth.stencil ? VkDeviceSize(d.depth.width) * d.depth.height : 0, at_stencil = place(zupload ? sbytes : 0);
    VkDeviceSize at_values = place(d.values.size() * 4);
    VkDeviceSize at_vertices = place(d.vertices.size() * 4), at_indices = place(d.indices.size() * 4);
    VkDeviceSize at_vs_values = place(d.vs_values.size() * 4);
    // each buffer: in the pool (sent there first, from the upload, if it isn't there yet), or
    // in the upload alone
    struct InPool {
        VkDeviceSize at = 0;
        bool pooled = false, send = false;
    };
    std::vector<InPool> vs_pool(d.vs_buffers.size()), ps_pool(d.ps_buffers.size());
    // (if the draw fails before they are sent, the pool forgets them)
    struct Unsent {
        std::vector<const std::vector<u32>*> keys;
        bool sent = false;
        ~Unsent() {
            if (sent) return;
            for (auto* key : keys)
                if (auto it = g_pool.entries.find(key); it != g_pool.entries.end()) {
                    g_pool.by_place.erase(it->second.at);
                    g_pool.entries.erase(it);
                }
        }
    } unsent;
    auto place_buffer = [&](const Buffer& b, InPool& p) {
        p.pooled = pooled(b, k.draws, p.at, p.send);
        if (p.send) unsent.keys.push_back(b.get());
        return !p.pooled || p.send ? place(b->size() * 4) : 0;
    };
    std::vector<VkDeviceSize> at_buffers;
    for (size_t i = 0; i < d.vs_buffers.size(); i++) at_buffers.push_back(place_buffer(d.vs_buffers[i], vs_pool[i]));
    std::vector<VkDeviceSize> at_ps_buffers;
    for (size_t i = 0; i < d.ps_buffers.size(); i++) at_ps_buffers.push_back(place_buffer(d.ps_buffers[i], ps_pool[i]));
    // the textures not kept yet, made and uploaded
    std::vector<GpuImage*> images(d.textures.size());
    std::vector<VkDeviceSize> at_texels(d.textures.size(), ~VkDeviceSize(0));
    std::vector<GpuImage> unkept(d.textures.size());
    for (size_t i = 0; i < d.textures.size(); i++) {
        Texture& t = d.textures[i];
        if (Resident* r = from_target[i]) {  // an image kept for it, by its format, size, swizzle and place
            u32 sw = t.swizzle[0] | t.swizzle[1] << 3 | t.swizzle[2] << 6 | t.swizzle[3] << 9;
            GpuImage& im = r->copies[{t.format, t.width, t.height, sw, offset[i] | (u64(t.pitch) << 40) | (u64(direct[i]) << 63)}];
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
            if ((!t.format && t.rgba.empty()) || ((t.block || (t.format && !t.raw)) && t.blocks.empty())) {  // let go since has_texture() said it was kept
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
        at_texels[i] = place(texel_bytes(t));
        k.texture_uploads++;
    }
    part.to("its place in the batch (the batch sent first, if full)");
    // its place in the batch: the offsets above are from its start
    VkDeviceSize base;
    VkDescriptorSet set;
    if (!kept && !flush_batch("--gpu-check")) {  // --gpu-check's copies come back at once: nothing else waiting
        why = "the GPU didn't finish the draws before";
        return false;
    }
    if (!reserve_batch(at, base, set, why)) return false;
    if (!kept && !k.readback.reserve(back_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) {
        why = "out of memory for the readback";
        return false;
    }
    part.to("copying into the upload");
    for (auto& a : at_target) a += base;
    at_depth += base;
    at_stencil += base;
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
    if (zupload) std::memcpy(up + at_depth, kept ? reinterpret_cast<const unsigned char*>(d.depth.address) : d.depth.target, zbytes);
    if (zupload && sbytes)
        std::memcpy(up + at_stencil, kept ? reinterpret_cast<const unsigned char*>(d.depth.saddress) : d.depth.starget, sbytes);
    std::memcpy(up + at_values, d.values.data(), d.values.size() * 4);
    auto copy0 = std::chrono::steady_clock::now();
    std::memcpy(up + at_vertices, d.vertices.data(), d.vertices.size() * 4);
    std::memcpy(up + at_indices, d.indices.data(), d.indices.size() * 4);
    std::memcpy(up + at_vs_values, d.vs_values.data(), d.vs_values.size() * 4);
    k.buffer_upload_bytes += (d.vertices.size() + d.indices.size() + d.vs_values.size()) * 4;
    for (size_t i = 0; i < d.vs_buffers.size(); i++)
        if (!vs_pool[i].pooled || vs_pool[i].send) {
            std::memcpy(up + at_buffers[i], d.vs_buffers[i]->data(), d.vs_buffers[i]->size() * 4);
            k.buffer_upload_bytes += d.vs_buffers[i]->size() * 4;
        }
    for (size_t i = 0; i < d.ps_buffers.size(); i++)
        if (!ps_pool[i].pooled || ps_pool[i].send) {
            std::memcpy(up + at_ps_buffers[i], d.ps_buffers[i]->data(), d.ps_buffers[i]->size() * 4);
            k.buffer_upload_bytes += d.ps_buffers[i]->size() * 4;
        }
    auto buffers1 = std::chrono::steady_clock::now();
    k.buffer_upload_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(buffers1 - copy0).count());
    for (size_t i = 0; i < d.textures.size(); i++) {
        if (at_texels[i] == ~VkDeviceSize(0)) continue;
        const Texture& t = d.textures[i];
        size_t bytes = texel_bytes(t);
        if (t.block) std::memcpy(up + at_texels[i], t.blocks.data(), bytes);
        else if (t.format) std::memcpy(up + at_texels[i], t.raw ? t.raw : t.blocks.data(), bytes);
        else std::memcpy(up + at_texels[i], t.rgba.data(), bytes);
        k.texture_upload_bytes += bytes;
    }
    k.upload_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - copy0).count());

    part.to("the descriptors");
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
    auto write_buffer = [&](u32 binding, VkDeviceSize offset, VkDeviceSize bytes, VkBuffer buffer = VK_NULL_HANDLE) {
        binfo.push_back({buffer ? buffer : k.upload.buffer, offset, std::max<VkDeviceSize>(bytes, 16)});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &binfo.back();
        writes.push_back(w);
    };
    // a buffer's descriptor: its place in the pool, or in the upload
    auto write_pooled = [&](u32 binding, VkDeviceSize offset, const Buffer& b, const InPool& p) {
        if (p.pooled) write_buffer(binding, p.at, b->size() * 4, g_pool.buffer);
        else write_buffer(binding, offset, b->size() * 4);
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
        for (u32 i = 0; i < d.vs_buffers.size(); i++)
            write_pooled(i < 15 ? 17 + i : 72 + (i - 15), at_buffers[i], d.vs_buffers[i], vs_pool[i]);
    }
    for (u32 i = 0; i < d.ps_buffers.size(); i++) write_pooled(56 + i, at_ps_buffers[i], d.ps_buffers[i], ps_pool[i]);
    vkUpdateDescriptorSets(g.device, u32(writes.size()), writes.data(), 0, nullptr);

    part.to("the commands, recorded");
    // the commands, into the batch
    VkCommandBuffer cmd = batch();
    const VkPipelineStageFlags T = VK_PIPELINE_STAGE_TRANSFER_BIT;
    // the buffers new to the pool, into it: once the draws before have read what was there
    {
        std::vector<VkBufferCopy> sends;
        for (size_t i = 0; i < d.vs_buffers.size(); i++)
            if (vs_pool[i].send) sends.push_back({at_buffers[i], vs_pool[i].at, d.vs_buffers[i]->size() * 4});
        for (size_t i = 0; i < d.ps_buffers.size(); i++)
            if (ps_pool[i].send) sends.push_back({at_ps_buffers[i], ps_pool[i].at, d.ps_buffers[i]->size() * 4});
        if (!sends.empty()) {
            const VkPipelineStageFlags S = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
                                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, S | T, T, 0, 1, &mb, 0, nullptr, 0, nullptr);
            vkCmdCopyBuffer(cmd, k.upload.buffer, g_pool.buffer, u32(sends.size()), sends.data());
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, T, S, 0, 1, &mb, 0, nullptr, 0, nullptr);
            mark(cmd, "draws: buffers into the pool");
        }
        unsent.sent = true;
    }
    const VkPipelineStageFlags C = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkAccessFlags RW = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // the textures that are kept targets, copied first (before this draw changes its own)
    for (size_t i = 0; i < d.textures.size(); i++) {
        Resident* r = from_target[i];
        if (!r) continue;
        GpuImage& im = *images[i];
        const Texture& t = d.textures[i];
        // (copied already, and the memory not written since: as it is)
        if (!r->version) r->version = ++g_writes;
        if (im.version == r->version) {
            k.target_textures_same++;
            continue;
        }
        im.version = r->version;
        barrier(cmd, im.image, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, T);
        barrier(cmd, r->image.image, 1, rest(*r), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rest_access(*r), VK_ACCESS_TRANSFER_READ_BIT,
                rest_stage(*r), T, aspect(*r));
        if (direct[i]) {
            VkImageCopy ic{};
            ic.srcSubresource = ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            ic.extent = {t.width, t.height, 1};
            vkCmdCopyImage(cmd, r->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        } else {  // through the scratch buffer: the image's texels in order, then the texture's rows from them
            VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.buffer = k.scratch.buffer;
            bb.size = VK_WHOLE_SIZE;
            bb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            bb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, T, T, 0, 0, nullptr, 1, &bb, 0, nullptr);
            VkBufferImageCopy c{};
            c.imageSubresource = {r->depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
            c.imageExtent = {r->width, r->height, 1};
            vkCmdCopyImageToBuffer(cmd, r->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, k.scratch.buffer, 1, &c);
            bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, T, T, 0, 0, nullptr, 1, &bb, 0, nullptr);
            VkBufferImageCopy c2{};
            c2.bufferOffset = offset[i];
            c2.bufferRowLength = t.pitch;
            c2.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c2.imageExtent = {t.width, t.height, 1};
            vkCmdCopyBufferToImage(cmd, k.scratch.buffer, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c2);
        }
        barrier(cmd, r->image.image, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rest(*r), VK_ACCESS_TRANSFER_READ_BIT, rest_access(*r), T,
                rest_stage(*r), aspect(*r));
        barrier(cmd, im.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, T, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
        k.target_textures++;
        mark(cmd, "draws: kept memory copied, to read as a texture");
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
        if (!kept_from[i].empty()) {  // its layers in kept memory, over what went up for them
            barrier(cmd, im.image, im.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, T, T);
            for (size_t j = 0; j < kept_from[i].size(); j++) {
                Resident& r = *kept_from[i][j];
                const Texture::KeptLayer& kl = t.kept_layers[j];
                VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bb.buffer = k.scratch.buffer;
                bb.size = VK_WHOLE_SIZE;
                bb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;  // (an earlier copy done with it)
                bb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, T, T, 0, 0, nullptr, 1, &bb, 0, nullptr);
                barrier(cmd, r.image.image, 1, rest(r), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rest_access(r), VK_ACCESS_TRANSFER_READ_BIT,
                        rest_stage(r), T, aspect(r));
                VkBufferImageCopy whole{};
                whole.imageSubresource = {aspect(r), 0, 0, 1};
                whole.imageExtent = {r.width, r.height, 1};
                vkCmdCopyImageToBuffer(cmd, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, k.scratch.buffer, 1, &whole);
                barrier(cmd, r.image.image, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rest(r), VK_ACCESS_TRANSFER_READ_BIT, rest_access(r), T,
                        rest_stage(r), aspect(r));
                bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmd, T, T, 0, 0, nullptr, 1, &bb, 0, nullptr);
                VkBufferImageCopy one{};
                one.bufferOffset = kl.address - r.base;
                one.bufferRowLength = t.kept_pitch;
                one.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kl.layer, 1};
                one.imageExtent = {t.width, t.height, 1};
                vkCmdCopyBufferToImage(cmd, k.scratch.buffer, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &one);
                k.layers_from_kept++;
            }
            mark(cmd, "draws: textures' layers copied from kept memory");
        }
        barrier(cmd, im.image, im.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, T,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
    }
    VkRenderingAttachmentInfo zatt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    u32 area_w = d.width, area_h = d.height;
    VkImage zimage = VK_NULL_HANDLE;
    if (d.depth.format) {
        GpuImage* zi = nullptr;
        if (kept) {
            zi = &zres->image;
        } else {  // --gpu-check's copy
            zi = &k.targets[{d.depth.format, d.depth.width, d.depth.height, 100}];
            if (!zi->image && !make_image(*zi, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VkFormat(d.depth.format), d.depth.width, d.depth.height,
                                          1, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
                why = "out of GPU memory";
                return false;
            }
        }
        const VkPipelineStageFlags Z = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        const VkAccessFlags ZRW = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        const VkImageLayout ZL = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const VkImageAspectFlags ZA = d.depth.stencil ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        if (zupload) {
            barrier(cmd, zi->image, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, T, ZA);
            VkBufferImageCopy c{};
            c.bufferOffset = at_depth;
            c.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            c.imageExtent = {d.depth.width, d.depth.height, 1};
            vkCmdCopyBufferToImage(cmd, k.upload.buffer, zi->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            if (sbytes) {
                c.bufferOffset = at_stencil;
                c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
                vkCmdCopyBufferToImage(cmd, k.upload.buffer, zi->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            }
            barrier(cmd, zi->image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ZL, VK_ACCESS_TRANSFER_WRITE_BIT, ZRW, T, Z, ZA);
        } else {  // as the last draw or readback left it
            barrier(cmd, zi->image, 1, ZL, ZL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, ZRW, Z | T, Z, ZA);
        }
        zimage = zi->image;
        zatt.imageView = zi->view;
        zatt.imageLayout = ZL;
        zatt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        zatt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        area_w = nt ? std::min(area_w, d.depth.width) : d.depth.width;
        area_h = nt ? std::min(area_h, d.depth.height) : d.depth.height;
    }
    mark(cmd, "draws: targets, textures and depth uploaded");
    VkRenderingInfo rinfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rinfo.renderArea = {{0, 0}, {area_w, area_h}};
    rinfo.layerCount = 1;
    rinfo.colorAttachmentCount = u32(nt);
    rinfo.pColorAttachments = atts.data();
    if (d.depth.format) rinfo.pDepthAttachment = &zatt;
    if (d.depth.stencil) rinfo.pStencilAttachment = &zatt;
    vkCmdBeginRendering(cmd, &rinfo);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, k.layout, 0, 1, &set, 0, nullptr);
    VkViewport viewport{0, 0, float(d.width), float(d.height), 0, 1};
    int sx1 = std::min(d.scissor[2], int(area_w)), sy1 = std::min(d.scissor[3], int(area_h));  // (inside the attachments)
    VkRect2D scissor{{d.scissor[0], d.scissor[1]}, {u32(std::max(sx1 - d.scissor[0], 0)), u32(std::max(sy1 - d.scissor[1], 0))}};
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
    mark(cmd, "draws: drawing");
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
        if (zback) {
            barrier(cmd, zimage, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, T,
                    d.depth.stencil ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT);
            VkBufferImageCopy copy{};
            copy.bufferOffset = at_zback;
            copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            copy.imageExtent = {d.depth.width, d.depth.height, 1};
            vkCmdCopyImageToBuffer(cmd, zimage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, k.readback.buffer, 1, &copy);
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
    if (!kept) mark(cmd, "--gpu-check's copies back");
    if (!kept && !flush_batch("--gpu-check")) {
        why = "the GPU didn't finish the draw";
        return false;
    }
    if (zback) std::memcpy(d.depth.result, k.readback.p + at_zback, zbytes);
    if (zres) {
        bool swrites = d.depth.stencil_test && (d.depth.front.wmask || d.depth.back.wmask);
        Resident::State s = d.depth.write || swrites ? Resident::gpu : zupload ? Resident::same : zres->state;
        if (s != zres->state) {
            guard(*zres, s == Resident::gpu ? 0 : 1);
            zres->state = s;
        }
        if (zupload) k.target_uploads++;
        if (d.depth.write || swrites) written(*zres);
    }
    for (size_t i = 0; i < nt; i++) {
        if (!target[i]) continue;
        if (!kept) {
            std::memcpy(d.targets[i].result, k.readback.p + at_back[i], target_bytes[i]);
            continue;
        }
        if (upload[i]) k.target_uploads++;
        written(*res[i]);
        if (res[i]->state != Resident::gpu) {  // the memory is now the older: guarded from any touch
            guard(*res[i], 0);
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

bool compressed() {
    std::lock_guard lock(g_lock);
    return init() && g.bc;
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
    // arrays of kept memory: each part of one in an image kept there (its elements whole texels)
    // copied from it on the GPU, the rest from the memory (read back first if need be)
    d.array_from.resize(d.arrays.size());
    struct Piece {
        u64 at, bytes;  // from the array's start
        Resident* r;
        bool stencil;  // from its stencil (a byte a texel), not its depth or colour
    };
    std::vector<std::vector<Piece>> pieces(d.arrays.size());
    for (size_t i = 0; i < d.arrays.size(); i++) {
        auto [address, bytes] = d.array_from[i];
        if (!bytes) continue;
        d.arrays[i].assign((bytes + 3) / 4, 0);
        auto* out = reinterpret_cast<unsigned char*>(d.arrays[i].data());
        for (u64 pos = address, end = address + bytes; pos < end;) {
            Resident* in = nullptr;
            bool stencil = false;
            u64 next = end;  // where the next image kept there starts
            for (auto& [rb, r] : g_resident) {
                if (r.state == Resident::memory) continue;
                u64 sbytes = u64(r.width) * r.height;
                if (pos >= rb && pos < rb + r.bytes && (pos - rb) % r.bpp == 0) in = &r, stencil = false;
                else if (r.sbase && pos >= r.sbase && pos < r.sbase + sbytes) in = &r, stencil = true;
                else {
                    if (rb > pos && rb < next) next = rb;
                    if (r.sbase > pos && r.sbase < next) next = r.sbase;
                }
            }
            u64 in_end = !in ? 0 : stencil ? in->sbase + u64(in->width) * in->height : in->base + in->bytes;
            u32 bpp = !in ? 1 : stencil ? 1 : in->bpp;
            u64 n = in ? (std::min(end, in_end) - pos) / bpp * bpp : 0;
            if (n) {
                pieces[i].push_back({pos - address, n, in, stencil});
                pos += n;
                continue;
            }
            u64 to = in ? std::min(end, in_end) : next;
            read_back_pages(pos & ~u64(0xfff), (to + 0xfff) & ~u64(0xfff), "a compute shader's array, the part not in an image");
            std::memcpy(out + (pos - address), reinterpret_cast<const void*>(pos), to - pos);
            pos = to;
        }
        if (!pieces[i].empty()) k.arrays_kept++;
    }
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
    // the arrays' parts in images kept there, from them: the rest of the first row, the whole
    // rows, the start of the last
    bool copied = false;
    for (size_t i = 0; i < d.arrays.size(); i++)
        for (const Piece& p : pieces[i]) {
            Resident* r = p.r;
            VkImageAspectFlags from = p.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : r->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            u32 bpp = p.stencil ? 1 : r->bpp;
            u64 first = (d.array_from[i].first + p.at - (p.stencil ? r->sbase : r->base)) / bpp, n = p.bytes / bpp;
            barrier(cmd, r->image.image, 1, rest(*r), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rest_access(*r), VK_ACCESS_TRANSFER_READ_BIT,
                    rest_stage(*r), T, aspect(*r));
            std::vector<VkBufferImageCopy> parts;
            for (u64 at_texel = first, end = first + n; at_texel < end;) {
                u32 x = u32(at_texel % r->width), y = u32(at_texel / r->width);
                VkBufferImageCopy c{};
                c.bufferOffset = base + at_arrays[i] + p.at + (at_texel - first) * bpp;
                c.imageSubresource = {from, 0, 0, 1};
                c.imageOffset = {std::int32_t(x), std::int32_t(y), 0};
                if (x || end - at_texel < r->width) {  // part of a row
                    u32 w = u32(std::min<u64>(r->width - x, end - at_texel));
                    c.imageExtent = {w, 1, 1};
                    at_texel += w;
                } else {  // whole rows
                    u32 rows = u32((end - at_texel) / r->width);
                    c.bufferRowLength = r->width;
                    c.imageExtent = {r->width, rows, 1};
                    at_texel += u64(rows) * r->width;
                }
                parts.push_back(c);
            }
            vkCmdCopyImageToBuffer(cmd, r->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, k.upload.buffer, u32(parts.size()), parts.data());
            barrier(cmd, r->image.image, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rest(*r), VK_ACCESS_TRANSFER_READ_BIT, rest_access(*r), T,
                    rest_stage(*r), aspect(*r));
            copied = true;
        }
    if (copied) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, T, X, 0, 1, &mb, 0, nullptr, 0, nullptr);
        mark(cmd, "dispatches: arrays copied from kept memory");
    }
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
    mark(cmd, "dispatches: memory uploaded");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, d.groups[0], d.groups[1], d.groups[2]);
    for (Resident* r : used)
        barrier(cmd, r->image.image, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, RW, X, C);
    mark(cmd, "dispatches: dispatching");
    // the memory: older than what was written; the same as what was only taken up and read
    std::set<Resident*> stored(into.begin(), into.end());
    for (Resident* r : stored) written(*r);
    for (Resident* r : used) {
        Resident::State s = stored.count(r) ? Resident::gpu : Resident::same;
        if (r->state == s || (r->state == Resident::gpu && s == Resident::same)) continue;
        guard(*r, s == Resident::gpu ? 0 : 1);
        r->state = s;
    }
    k.dispatches++;
    k.dispatch_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    return true;
}

bool clear_depth(u64 address, u32 width, u32 height, float value, int stencil, bool depth, u32 x0, u32 y0, u32 x1, u32 y1) {
    std::lock_guard lock(g_lock);
    struct Inside {
        Inside() { t_inside = true; }
        ~Inside() { t_inside = false; }
    } inside;
    auto it = g_resident.find(address);
    if (it == g_resident.end() || !it->second.depth || it->second.width != width || it->second.height != height) return false;
    Resident& r = it->second;
    if (r.state == Resident::memory) return false;  // (the memory may be the newer, the stencil's too: cleared there instead)
    if (stencil >= 0 && !r.sbase) return false;
    VkImageAspectFlags which = (depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil >= 0 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (!which) return true;
    x1 = std::min(x1, width), y1 = std::min(y1, height);
    if (x0 >= x1 || y0 >= y1) return true;
    bool whole = x0 == 0 && y0 == 0 && x1 == width && y1 == height;
    // a part of it: the scratch buffer filled with the values, copied into that rectangle
    VkDeviceSize area = VkDeviceSize(x1 - x0) * (y1 - y0), zbytes = (area * r.bpp + 3) & ~VkDeviceSize(3);
    if (!whole && zbytes + area > k.scratch.size && (!flush_batch("the scratch buffer") || !k.scratch.reserve(zbytes + area))) return false;
    VkCommandBuffer cmd = batch();
    const VkPipelineStageFlags T = VK_PIPELINE_STAGE_TRANSFER_BIT;
    barrier(cmd, r.image.image, 1, rest(r), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rest_access(r) | VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, rest_stage(r) | T, T, aspect(r));
    if (whole) {
        VkClearDepthStencilValue v{value, u32(std::max(stencil, 0))};
        VkImageSubresourceRange range{which, 0, 1, 0, 1};  // the depth, the stencil, or both
        vkCmdClearDepthStencilImage(cmd, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
    } else {
        VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = k.scratch.buffer;
        bb.size = VK_WHOLE_SIZE;
        bb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, T, T, 0, 0, nullptr, 1, &bb, 0, nullptr);
        u32 zbits;
        if (r.bpp == 2) {
            u32 h = u32(std::nearbyint(std::clamp(value, 0.0f, 1.0f) * 65535.0f));
            zbits = h | (h << 16);
        } else {
            std::memcpy(&zbits, &value, 4);
        }
        if (depth) vkCmdFillBuffer(cmd, k.scratch.buffer, 0, zbytes, zbits);
        if (stencil >= 0) vkCmdFillBuffer(cmd, k.scratch.buffer, zbytes, (area + 3) & ~VkDeviceSize(3), u32(stencil) * 0x01010101u);
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, T, T, 0, 0, nullptr, 1, &bb, 0, nullptr);
        VkBufferImageCopy c[2]{};
        u32 n = 0;
        for (int s = 0; s < 2; s++) {
            if (s == 0 ? !depth : stencil < 0) continue;
            c[n].bufferOffset = s == 0 ? 0 : zbytes;
            c[n].imageSubresource = {VkImageAspectFlags(s == 0 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT), 0, 0, 1};
            c[n].imageOffset = {std::int32_t(x0), std::int32_t(y0), 0};
            c[n].imageExtent = {x1 - x0, y1 - y0, 1};
            n++;
        }
        vkCmdCopyBufferToImage(cmd, k.scratch.buffer, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, c);
    }
    barrier(cmd, r.image.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rest(r), VK_ACCESS_TRANSFER_WRITE_BIT, rest_access(r), T, rest_stage(r),
            aspect(r));
    mark(cmd, "depth buffers cleared");
    if (r.state != Resident::gpu) {  // the image is now the newer
        guard(r, 0);
        r.state = Resident::gpu;
    }
    k.depth_clears++;
    written(r);
    return true;
}

// A depth buffer's fast clear seen to on the GPU, its HTILE buffer kept there (a compute
// shader's store): a compute pass counts the tiles marked cleared, then a pass over the whole
// depth buffer writes the clear value only if that is all of them (as expand_htile() does on
// the CPU), with no wait and nothing read back
bool clear_depth_by_htile(u64 htile, u64 tiles, u64 address, u32 width, u32 height, float value) {
    std::lock_guard lock(g_lock);
    struct Inside {
        Inside() { t_inside = true; }
        ~Inside() { t_inside = false; }
    } inside;
    if (!g.ok || !tiles || tiles > (u64(1) << 26)) return false;
    auto zt = g_resident.find(address);
    if (zt == g_resident.end() || !zt->second.depth || zt->second.width != width || zt->second.height != height ||
        zt->second.state == Resident::memory)
        return false;
    Resident& z = zt->second;
    Resident* h = nullptr;
    for (auto& [base, r] : g_resident)
        if (htile >= base && htile + tiles * 4 <= base + r.bytes && (htile - base) % 4 == 0 && r.uints && r.state != Resident::memory &&
            (r.bpp == 4 || r.bpp == 8 || r.bpp == 16))
            h = &r;
    if (!h) return false;
    std::string why;
    // the count: each tile a thread, the dword it is (in the image's texel, as uints)
    std::string cs = std::format(R"(#version 450
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) buffer Values {{ uint U[]; }};
layout(set = 0, binding = 36, {}) uniform readonly uimage2D H;
void main() {{
    uint i = gl_GlobalInvocationID.x;
    if (i >= U[2]) return;
    uint o = U[0] + 4u * i, t = o / U[3], c = (o % U[3]) / 4u;
    uvec4 v = imageLoad(H, ivec2(int(t % U[1]), int(t / U[1])));
    uint w = c == 0u ? v.x : c == 1u ? v.y : c == 2u ? v.z : v.w;
    if ((w & 15u) == 0u) atomicAdd(U[4], 1u);
}}
)",
                                 h->bpp == 16 ? "rgba32ui" : h->bpp == 8 ? "rg32ui" : "r32ui");
    auto ct = k.compute.find(cs);
    VkPipeline count = ct == k.compute.end() ? VK_NULL_HANDLE : ct->second;
    if (!count) {
        VkShaderModule m = module(cs, shaderc_compute_shader, why);
        if (!m) return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = m;
        ci.stage.pName = "main";
        ci.layout = k.layout;
        if (vkCreateComputePipelines(g.device, VK_NULL_HANDLE, 1, &ci, nullptr, &count) != VK_SUCCESS) return false;
        k.compute[cs] = count;
    }
    // the fill: a triangle over the whole buffer, its depth the clear value, or nothing
    std::string key = std::format("htile:{}", z.format);
    auto pt = k.pipelines.find(key);
    VkPipeline fill = pt == k.pipelines.end() ? VK_NULL_HANDLE : pt->second;
    if (!fill) {
        VkShaderModule vsm = module(R"(#version 450
void main() { gl_Position = vec4(vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0, 0.0, 1.0); }
)",
                                    shaderc_vertex_shader, why);
        VkShaderModule psm = module(R"(#version 450
layout(set = 0, binding = 0, std430) readonly buffer Values { uint U[]; };
void main() {
    if (U[4] != U[2]) discard;
    gl_FragDepth = uintBitsToFloat(U[5]);
}
)",
                                    shaderc_fragment_shader, why);
        if (!vsm || !psm) return false;
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = vsm, stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = psm, stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.depthAttachmentFormat = VkFormat(z.format);
        VkPipelineDepthStencilStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dsi.depthTestEnable = VK_TRUE;
        dsi.depthWriteEnable = VK_TRUE;
        dsi.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &ri;
        gi.stageCount = 2;
        gi.pStages = stages;
        gi.pVertexInputState = &vi;
        gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vp;
        gi.pRasterizationState = &rs;
        gi.pMultisampleState = &ms;
        gi.pColorBlendState = &bs;
        gi.pDepthStencilState = &dsi;
        gi.pDynamicState = &ds;
        gi.layout = k.layout;
        if (vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &gi, nullptr, &fill) != VK_SUCCESS) return false;
        k.pipelines[key] = fill;
    }
    // U: where the tiles start in the image (bytes), its width, the tiles, its texel's bytes,
    // the count, the clear value
    u32 values[8] = {u32(htile - h->base), h->width, u32(tiles), h->bpp, 0, 0, 0, 0};
    std::memcpy(&values[5], &value, 4);
    VkDeviceSize base;
    VkDescriptorSet set;
    if (!reserve_batch(sizeof values, base, set, why)) return false;
    std::memcpy(k.upload.p + base, values, sizeof values);
    VkDescriptorBufferInfo bi{k.upload.buffer, base, sizeof values};
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, h->uints, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstSet = set, w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[0].pBufferInfo = &bi;
    w[1].dstSet = set, w[1].dstBinding = 36, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[1].pImageInfo = &ii;
    vkUpdateDescriptorSets(g.device, 2, w, 0, nullptr);
    VkCommandBuffer cmd = batch();
    const VkPipelineStageFlags T = VK_PIPELINE_STAGE_TRANSFER_BIT, C = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               X = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, F = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const VkAccessFlags RW = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier(cmd, h->image.image, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, C | T, X);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, count);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, u32((tiles + 63) / 64), 1, 1);
    barrier(cmd, h->image.image, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, RW, X, C);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, X, F, 0, 1, &mb, 0, nullptr, 0, nullptr);
    const VkPipelineStageFlags Z = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags ZRW = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkImageLayout ZL = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier(cmd, z.image.image, 1, ZL, ZL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            ZRW, Z | T, Z, aspect(z));
    VkRenderingAttachmentInfo zatt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    zatt.imageView = z.image.view;
    zatt.imageLayout = ZL;
    zatt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    zatt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rinfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rinfo.renderArea = {{0, 0}, {width, height}};
    rinfo.layerCount = 1;
    rinfo.pDepthAttachment = &zatt;
    vkCmdBeginRendering(cmd, &rinfo);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fill);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, k.layout, 0, 1, &set, 0, nullptr);
    VkViewport viewport{0, 0, float(width), float(height), 0, 1};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    mark(cmd, "depth buffers' fast clears (HTILE)");
    if (z.state != Resident::gpu) {  // the image is (or may be) now the newer
        guard(z, 0);
        z.state = Resident::gpu;
    }
    k.htile_checks++;
    written(z);
    return true;
}

u64 steady_version(u64 address, u64 bytes, u32 bpp) {
    std::lock_guard lock(g_lock);
    for (auto& [base, r] : g_resident)
        if (address >= base && address + bytes <= base + r.bytes && r.bpp == bpp && (address - base) % bpp == 0 && !r.depth &&
            !r.sbase)
            return r.state == Resident::memory ? 0 : r.version ? r.version : (r.version = ++g_writes);
    return 0;
}

u64 version(u64 address) {
    std::lock_guard lock(g_lock);
    for (auto& [base, r] : g_resident)
        if (address >= base && address < base + r.bytes) return r.version ? r.version : (r.version = ++g_writes);
    return 0;
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
    return flush_batch("a command buffer of the game's done", false);
}

bool touched(u64 addr, bool write, u64 rip) {
    if (t_inside) return false;  // this thread is in the middle of a draw
    std::lock_guard lock(g_lock);
    t_inside = true;
    bool ours = false;
    for (auto& [base, r] : g_resident) {
        if ((addr < r.lo || addr >= r.hi) && (!r.sbase || addr < r.slo || addr >= r.shi)) continue;
        ours = true;
        if (r.state == Resident::gpu &&
            !read_back(r, t_doing ? std::format("touched by the loader: {}", t_doing)
                          : rip >= 0x10000000 && current_thread() && current_thread()->in_call
                              ? std::format("touched by the loader, in the game's call to {}", M.imports[current_thread()->in_call - 1].name)
                              : std::format("touched by {} code at {:#x} ({}, on {})", rip < 0x10000000 ? "the game's" : "the loader's", rip,
                                            write ? "a write" : "a read",
                                            current_thread() ? std::format("game thread {}", current_thread()->name) : "a thread of the loader's"))) {
            ours = false;  // the GPU failed: the fault stands
        } else if (r.state == Resident::same && write) {  // the memory is now the newer
            guard(r, 2);
            r.state = Resident::memory;
            written(r);
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
    if (!g.int64 && (d.ps_text().find("uint64_t") != std::string::npos || d.vs_text().find("uint64_t") != std::string::npos)) {
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
    if (!g.validation) return s + (M.gpu_check ? " (the validation layer isn't installed)" : "");
    if (u64 n = g_messages.load()) return s + std::format(" (the validation layer reported {} problems, the first: {})", n, g_first_message);
    return s + " (the validation layer reported no problems)";
}

// A count as a table, the most first (up to 25 rows)
std::string tally(const char* head, const std::map<std::string, u64>& m) {
    std::vector<std::pair<u64, std::string>> by;
    for (auto& [what, n] : m) by.push_back({n, what});
    std::sort(by.rbegin(), by.rend());
    std::string t = head;
    for (size_t i = 0; i < by.size() && i < 25; i++) t += std::format("| {} | {} |\n", by[i].second, by[i].first);
    return t;
}

// The GPU's time, by what it was doing, the most first
// The CPU's time in the draws on the GPU, by part, the most first
std::string part_time() {
    if (g_part_ns.empty()) return {};
    std::vector<std::pair<u64, std::string>> by;
    u64 all = 0;
    for (auto& [what, ns] : g_part_ns) by.push_back({ns, what}), all += ns;
    std::sort(by.rbegin(), by.rend());
    std::string t = std::format("\nThe CPU's time in the draws on the GPU, {:.1f} s in all, by part (waits for the GPU included):\n\n| Part | Seconds |\n|---|---:|\n",
                                double(all) / 1e9);
    for (auto& [ns, what] : by) t += std::format("| {} | {:.2f} |\n", what, double(ns) / 1e9);
    return t + std::format("\nOf which: {} shaders' GLSL compiled into SPIR-V (shaderc), {:.1f} s.\n", g_compiled, double(g_compile_ns) / 1e9);
}

std::string gpu_time() {
    if (g_time.ns.empty()) return {};
    std::vector<std::pair<u64, std::string>> by;
    u64 all = 0;
    for (auto& [what, ns] : g_time.ns) by.push_back({ns, what}), all += ns;
    std::sort(by.rbegin(), by.rend());
    std::string t = std::format("\nThe GPU's time (from timestamps), {:.1f} s in all, by what it was doing:\n\n| What | Seconds |\n|---|---:|\n",
                                double(all) / 1e9);
    for (auto& [ns, what] : by) t += std::format("| {} | {:.2f} |\n", what, double(ns) / 1e9);
    return t;
}

// The read-backs by why, the most time first
std::string readback_time() {
    std::vector<std::pair<u64, std::string>> by;
    for (auto& [what, e] : g_readback_ns) by.push_back({e.second, what});
    std::sort(by.rbegin(), by.rend());
    std::string t = "\nThe time reading back, by why:\n\n| Why | Times | Seconds |\n|---|---:|---:|\n";
    for (size_t i = 0; i < by.size() && i < 25; i++)
        t += std::format("| {} | {} | {:.2f} |\n", by[i].second, g_readback_ns[by[i].second].first, double(by[i].first) / 1e9);
    return t;
}

std::string report() {
    std::lock_guard lock(g_lock);
    if (!k.draws) return {};
    return std::format("{} draws on the GPU took {:.1f} s, uploads included; {} pipelines; {} textures uploaded, {} kept at "
                       "the end ({} MiB), {} let go. Render targets kept there: {} at the end; taken up from memory {} times, "
                       "read back into it {} times (when touched), written there {} times, {} let go; read as textures {} times "
                       "(copied again {} of them; the copy still the same {}; textures' layers copied from there {})",
                       k.draws, double(k.ns) / 1e9, k.pipelines.size(), k.texture_uploads, k.textures.size(),
                       k.texture_bytes >> 20, k.evicted, g_resident.size(), k.target_uploads, k.readbacks, k.writes, k.let_go,
                       k.target_textures + k.target_textures_same, k.target_textures, k.target_textures_same, k.layers_from_kept) +
           (k.dispatches ? std::format(". {} compute dispatches there took {:.1f} s ({} of their arrays copied there from kept memory)", k.dispatches,
                                       double(k.dispatch_ns) / 1e9, k.arrays_kept) : "") +
           std::format(". Kept there because compute shaders stored into it: {} pieces of memory; given another shape there {} times. "
                       "Depth buffers' fast clears seen to there: {}. Sent to the GPU in {} batches (each waited for, then or before the next)", k.regions, k.reshapes,
                       k.htile_checks, k.batches) +
           std::format(". The draws' larger buffers kept there ({} MiB for them): {} sent ({} MiB), {} found there, {} let go",
                       g_pool.size >> 20, g_pool.sent, g_pool.sent_bytes >> 20, g_pool.found, g_pool.let_go) +
           std::format(". Of the time: {:.1f} s submitting batches and waiting for them, {:.1f} s reading targets back into "
                       "memory (waits included), {:.1f} s copying draws' data to upload ({} MiB of textures; and {:.1f} s of it "
                       "the draws' buffers, {} MiB)",
                       double(k.wait_ns) / 1e9, double(k.readback_ns) / 1e9, double(k.upload_ns) / 1e9, k.texture_upload_bytes >> 20,
                       double(k.buffer_upload_ns) / 1e9, k.buffer_upload_bytes >> 20) +
           tally(".\n\nWhy batches went to the GPU:\n\n| Why | Times |\n|---|---:|\n", g_batch_why) +
           tally("\nTargets read back into memory:\n\n| Why (target) | Times |\n|---|---:|\n", g_readback_why) + readback_time() + gpu_time() + part_time();
}

}  // namespace mensis::vkdraw

#else  // no Vulkan SDK where this was built

namespace mensis::vkdraw {
thread_local const char* t_doing = nullptr;
bool has_texture(const std::string&) { return false; }
bool compressed() { return false; }
bool barycentric() { return false; }
bool flush() { return true; }
bool kept(u64, u64, u32) { return false; }
bool clear_depth(u64, u32, u32, float, int, bool, u32, u32, u32, u32) { return false; }
bool clear_depth_by_htile(u64, u64, u64, u32, u32, float) { return false; }
u64 version(u64) { return 0; }
u64 steady_version(u64, u64, u32) { return 0; }
bool touched(u64, bool, u64) { return false; }
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
