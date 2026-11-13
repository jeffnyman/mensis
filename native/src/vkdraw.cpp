// SPDX-License-Identifier: GPL-3.0-or-later
//
// One draw on the PC's GPU, through Vulkan, with nothing kept between draws: it is for
// checking the translation (--gpu-check), a draw at a time, not for speed. The GLSL is
// compiled to SPIR-V by the Vulkan SDK's shaderc. Textures are uploaded as 32-bit floats,
// decoded on the CPU exactly as the CPU's interpreter reads them, so that only the shader
// and the GPU's filtering and blending differ from the CPU's draw.
//
// Built where the Vulkan SDK is (Windows, so far: MENSIS_VULKAN); elsewhere run() says so.

#include "vkdraw.hpp"

#ifdef MENSIS_VULKAN

#include <shaderc/shaderc.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
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
    bool validation = false, int64 = false;
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
    if (g_messages <= 10) std::fprintf(stderr, "Vulkan validation (GPU check): %s\n", data->pMessage);
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
    app.pApplicationName = "Mensis GPU check";
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
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.dynamicRendering = VK_TRUE;
    v13.shaderDemoteToHelperInvocation = VK_TRUE;  // what discard compiles to for Vulkan 1.3
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

// Everything one draw makes, destroyed at the end
struct Objects {
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> buffers;
    std::vector<std::pair<VkImage, VkDeviceMemory>> images;
    std::vector<VkImageView> views;
    std::vector<VkSampler> samplers;
    std::vector<VkShaderModule> modules;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    ~Objects() {
        VkDevice d = g.device;
        if (fence) vkDestroyFence(d, fence, nullptr);
        if (cmd) vkFreeCommandBuffers(d, g.pool, 1, &cmd);
        if (dpool) vkDestroyDescriptorPool(d, dpool, nullptr);
        if (pipeline) vkDestroyPipeline(d, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(d, layout, nullptr);
        if (set_layout) vkDestroyDescriptorSetLayout(d, set_layout, nullptr);
        for (auto m : modules) vkDestroyShaderModule(d, m, nullptr);
        for (auto s : samplers) vkDestroySampler(d, s, nullptr);
        for (auto v : views) vkDestroyImageView(d, v, nullptr);
        for (auto [i, m] : images) vkDestroyImage(d, i, nullptr), vkFreeMemory(d, m, nullptr);
        for (auto [b, m] : buffers) vkDestroyBuffer(d, b, nullptr), vkFreeMemory(d, m, nullptr);
    }

    // A buffer the CPU can write and read, holding `bytes` (copied in if given)
    VkBuffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* bytes, void** mapped = nullptr) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = std::max<VkDeviceSize>(size, 16);
        bi.usage = usage;
        VkBuffer b;
        if (vkCreateBuffer(g.device, &bi, nullptr, &b) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(g.device, b, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory m;
        if (vkAllocateMemory(g.device, &ai, nullptr, &m) != VK_SUCCESS) {
            vkDestroyBuffer(g.device, b, nullptr);
            return VK_NULL_HANDLE;
        }
        vkBindBufferMemory(g.device, b, m, 0);
        buffers.emplace_back(b, m);
        void* p;
        vkMapMemory(g.device, m, 0, VK_WHOLE_SIZE, 0, &p);
        if (bytes) std::memcpy(p, bytes, size);
        if (mapped) *mapped = p;
        return b;
    }

    VkImage image(VkImageType type, VkFormat format, u32 w, u32 h, u32 depth, u32 layers, VkImageUsageFlags usage) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = type;
        ii.format = format;
        ii.extent = {w, h, depth};
        ii.mipLevels = 1;
        ii.arrayLayers = layers;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        VkImage i;
        if (vkCreateImage(g.device, &ii, nullptr, &i) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(g.device, i, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory m;
        if (vkAllocateMemory(g.device, &ai, nullptr, &m) != VK_SUCCESS) {
            vkDestroyImage(g.device, i, nullptr);
            return VK_NULL_HANDLE;
        }
        vkBindImageMemory(g.device, i, m, 0);
        images.emplace_back(i, m);
        return i;
    }
};

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

bool draw(Draw& d, std::string& why) {
    Objects o;
    std::vector<u32> vs_spv, ps_spv;
    if (!compile(d.vs, shaderc_vertex_shader, vs_spv, why) || !compile(d.ps, shaderc_fragment_shader, ps_spv, why)) return false;
    auto module = [&](const std::vector<u32>& code) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = code.size() * 4;
        mi.pCode = code.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(g.device, &mi, nullptr, &m);
        o.modules.push_back(m);
        return m;
    };
    VkShaderModule vsm = module(vs_spv), psm = module(ps_spv), gsm = VK_NULL_HANDLE;
    if (!d.gs.empty()) {
        std::vector<u32> gs_spv;
        if (!compile(d.gs, shaderc_geometry_shader, gs_spv, why)) return false;
        gsm = module(gs_spv);
    }
    bool gpu_vertices = !d.indices.empty();
    VkFormat format = VkFormat(d.format);

    // the descriptors: the values, then a texture and sampler for each image instruction
    // and the vertex shader's: its values at 16, its buffers from 17
    if (d.textures.size() > 15) {
        why = "more than 15 image instructions";
        return false;
    }
    const VkShaderStageFlags all = VK_SHADER_STAGE_ALL_GRAPHICS;
    std::vector<VkDescriptorSetLayoutBinding> bindings(1 + d.textures.size());
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all, nullptr};
    for (u32 k = 0; k < d.textures.size(); k++)
        bindings[k + 1] = {k + 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, all, nullptr};
    if (gpu_vertices) {
        bindings.push_back({16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all, nullptr});
        for (u32 k = 0; k < d.vs_buffers.size(); k++) bindings.push_back({17 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all, nullptr});
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = u32(bindings.size());
    li.pBindings = bindings.data();
    vkCreateDescriptorSetLayout(g.device, &li, nullptr, &o.set_layout);
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof d.viewport};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &o.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    vkCreatePipelineLayout(g.device, &pli, nullptr, &o.layout);

    // the pipeline
    VkPipelineShaderStageCreateInfo stages[3] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = psm;
    stages[1].pName = "main";
    stages[2].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    stages[2].module = gsm;
    stages[2].pName = "main";
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
    VkViewport viewport{0, 0, float(d.width), float(d.height), 0, 1};
    VkRect2D scissor{{d.scissor[0], d.scissor[1]}, {u32(d.scissor[2] - d.scissor[0]), u32(d.scissor[3] - d.scissor[1])}};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    vp.pScissors = &scissor;
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
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = d.mask & 15;
    if ((d.blend >> 30) & 1) {
        u32 bc = d.blend;
        bool separate = (bc >> 29) & 1;
        u32 csrc = bc & 31, cfn = (bc >> 5) & 7, cdst = (bc >> 8) & 31;
        u32 asrc = separate ? (bc >> 16) & 31 : csrc, afn = separate ? (bc >> 21) & 7 : cfn, adst = separate ? (bc >> 24) & 31 : cdst;
        cb.blendEnable = VK_TRUE;
        if (!blend_factor(csrc, cb.srcColorBlendFactor) || !blend_factor(cdst, cb.dstColorBlendFactor) ||
            !blend_factor(asrc, cb.srcAlphaBlendFactor) || !blend_factor(adst, cb.dstAlphaBlendFactor) ||
            !blend_op(cfn, cb.colorBlendOp) || !blend_op(afn, cb.alphaBlendOp)) {
            why = std::format("blending {:#x}", bc);
            return false;
        }
    }
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bs.attachmentCount = 1;
    bs.pAttachments = &cb;
    for (int c = 0; c < 4; c++) bs.blendConstants[c] = d.blend_color[c];
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &format;
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
    gi.layout = o.layout;
    if (vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &gi, nullptr, &o.pipeline) != VK_SUCCESS) {
        why = "vkCreateGraphicsPipelines failed";
        return false;
    }

    // the target, the textures and the buffers
    VkDeviceSize target_bytes = VkDeviceSize(d.width) * d.height * 4;
    VkBuffer staging = o.buffer(target_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, d.target.data());
    void* readback = nullptr;
    VkBuffer back = o.buffer(target_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, nullptr, &readback);
    VkImage target = o.image(VK_IMAGE_TYPE_2D, format, d.width, d.height, 1, 1,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkBuffer values = o.buffer(d.values.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, d.values.data());
    VkBuffer vertices = o.buffer(d.vertices.size() * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, d.vertices.data());
    VkBuffer indices = o.buffer(d.indices.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, d.indices.data());
    VkBuffer vs_values = o.buffer(d.vs_values.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, d.vs_values.data());
    std::vector<VkBuffer> vs_buffers;
    for (auto& b : d.vs_buffers) {
        vs_buffers.push_back(o.buffer(b.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, b.data()));
        if (!vs_buffers.back()) vertices = VK_NULL_HANDLE;
    }
    if (!staging || !back || !target || !values || !vertices || !indices || !vs_values) {
        why = "out of GPU memory";
        return false;
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = target;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView target_view;
    vkCreateImageView(g.device, &vci, nullptr, &target_view);
    o.views.push_back(target_view);

    struct Tex {
        VkImage image;
        VkBuffer staging;
        u32 layers;
    };
    std::vector<Tex> texs;
    std::vector<VkDescriptorImageInfo> infos(d.textures.size());
    for (size_t k = 0; k < d.textures.size(); k++) {
        const Texture& t = d.textures[k];
        bool three = t.type == 10;
        u32 layers = three ? 1 : t.layers;
        VkImage im = o.image(three ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, t.width, t.height,
                             three ? t.layers : 1, layers, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        VkBuffer st = o.buffer(t.rgba.size() * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, t.rgba.data());
        if (!im || !st) {
            why = "out of GPU memory";
            return false;
        }
        texs.push_back({im, st, layers});
        VkImageViewCreateInfo tv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        tv.image = im;
        tv.viewType = three ? VK_IMAGE_VIEW_TYPE_3D : t.type == 12 || t.type == 13 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        tv.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        tv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        VkImageView view;
        vkCreateImageView(g.device, &tv, nullptr, &view);
        o.views.push_back(view);
        const u32* s = t.sampler;
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = ((s[2] >> 20) & 3) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.minFilter = ((s[2] >> 22) & 3) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = address_mode(s[0] & 7);
        si.addressModeV = address_mode((s[0] >> 3) & 7);
        si.addressModeW = address_mode((s[0] >> 6) & 7);
        u32 border = (s[3] >> 30) & 3;
        si.borderColor = border == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        si.maxLod = 0;
        VkSampler sampler;
        vkCreateSampler(g.device, &si, nullptr, &sampler);
        o.samplers.push_back(sampler);
        infos[k] = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }

    // the descriptor set
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 + u32(d.vs_buffers.size())},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::max<u32>(1, u32(d.textures.size()))}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    vkCreateDescriptorPool(g.device, &dpi, nullptr, &o.dpool);
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = o.dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &o.set_layout;
    VkDescriptorSet set;
    vkAllocateDescriptorSets(g.device, &dai, &set);
    VkDescriptorBufferInfo vinfo{values, 0, VK_WHOLE_SIZE};
    std::vector<VkWriteDescriptorSet> writes(1 + d.textures.size(), {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET});
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &vinfo;
    for (u32 k = 0; k < d.textures.size(); k++) {
        writes[k + 1].dstSet = set;
        writes[k + 1].dstBinding = k + 1;
        writes[k + 1].descriptorCount = 1;
        writes[k + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[k + 1].pImageInfo = &infos[k];
    }
    std::vector<VkDescriptorBufferInfo> vs_infos;
    vs_infos.reserve(1 + vs_buffers.size());
    if (gpu_vertices) {
        vs_infos.push_back({vs_values, 0, VK_WHOLE_SIZE});
        for (VkBuffer b : vs_buffers) vs_infos.push_back({b, 0, VK_WHOLE_SIZE});
        for (u32 k = 0; k < vs_infos.size(); k++) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = 16 + k;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &vs_infos[k];
            writes.push_back(w);
        }
    }
    vkUpdateDescriptorSets(g.device, u32(writes.size()), writes.data(), 0, nullptr);

    // the commands
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(g.device, &cai, &o.cmd);
    VkCommandBuffer cmd = o.cmd;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    const VkPipelineStageFlags T = VK_PIPELINE_STAGE_TRANSFER_BIT;
    barrier(cmd, target, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, T, T);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {d.width, d.height, 1};
    vkCmdCopyBufferToImage(cmd, staging, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(cmd, target, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, T, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    for (size_t k = 0; k < texs.size(); k++) {
        const Texture& t = d.textures[k];
        barrier(cmd, texs[k].image, texs[k].layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, T, T);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, texs[k].layers};
        c.imageExtent = {t.width, t.height, t.type == 10 ? t.layers : 1};
        vkCmdCopyBufferToImage(cmd, texs[k].staging, texs[k].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cmd, texs[k].image, texs[k].layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, T, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = target_view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rinfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rinfo.renderArea = {{0, 0}, {d.width, d.height}};
    rinfo.layerCount = 1;
    rinfo.colorAttachmentCount = 1;
    rinfo.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &rinfo);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, o.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, o.layout, 0, 1, &set, 0, nullptr);
    if (gpu_vertices) {
        vkCmdPushConstants(cmd, o.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof d.viewport, d.viewport);
        vkCmdBindIndexBuffer(cmd, indices, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, u32(d.indices.size()), 1, 0, 0, 0);
    } else {
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertices, &zero);
        vkCmdDraw(cmd, u32(d.vertices.size() / ((1 + d.attributes) * 4)), 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
    barrier(cmd, target, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, T);
    vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, back, 1, &copy);
    VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hb.buffer = back;
    hb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, T, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
    vkEndCommandBuffer(cmd);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(g.device, &fi, nullptr, &o.fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(g.queue, 1, &si, o.fence) != VK_SUCCESS || vkWaitForFences(g.device, 1, &o.fence, VK_TRUE, 10'000'000'000ull) != VK_SUCCESS) {
        why = "the GPU didn't finish the draw";
        return false;
    }
    std::memcpy(d.target.data(), readback, target_bytes);
    return true;
}

}  // namespace

bool run(Draw& d, std::string& why) {
    std::lock_guard lock(g_lock);
    if (!init()) {
        why = g.why;
        return false;
    }
    if (d.ps.find("uint64_t") != std::string::npos && !g.int64) {
        why = "the GPU has no 64-bit integers";
        return false;
    }
    return draw(d, why);
}

std::string device() {
    std::lock_guard lock(g_lock);
    if (!g.ok) return g.tried ? "Vulkan couldn't be used: " + g.why : "";
    std::string s = "on the " + g.name;
    if (!g.validation) return s + " (the validation layer isn't installed)";
    if (u64 n = g_messages.load()) return s + std::format(" (the validation layer reported {} problems, the first: {})", n, g_first_message);
    return s + " (the validation layer reported no problems)";
}

}  // namespace mensis::vkdraw

#else  // no Vulkan SDK where this was built

namespace mensis::vkdraw {
bool run(Draw&, std::string& why) {
    why = "no Vulkan in this build (the Windows build has it)";
    return false;
}
std::string device() { return {}; }
}  // namespace mensis::vkdraw

#endif
