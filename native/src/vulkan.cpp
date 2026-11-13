// SPDX-License-Identifier: GPL-3.0-or-later
//
// The window's picture through Vulkan. Each time the window is redrawn, the display's buffer is
// copied into a buffer the GPU can read, from there into an image on the GPU, and that image
// is scaled ("blitted", with linear filtering) onto the window's swapchain image, which is then
// presented. No shaders yet: this is the groundwork the game's own shaders, translated for the
// GPU, will draw through.
//
// The GPU chosen is the first discrete one that can present to the window, else the first
// that can. Khronos's validation layer, which reports any wrong use of Vulkan, is switched on
// when it is installed (with the Vulkan SDK); what it reports goes to stderr and is counted
// in the summary.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>

#include "vulkan.hpp"

namespace mensis::vk {

namespace {

struct State {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    u32 family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;  // the last frame's commands have finished
    VkSemaphore acquired = VK_NULL_HANDLE;

    // the swapchain, remade when the window's size changes
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkSemaphore> drawn;  // one for each swapchain image: its commands are done

    // the picture: a buffer the CPU writes and an image on the GPU, remade when its size changes
    u32 width = 0, height = 0;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkImage picture = VK_NULL_HANDLE;
    VkDeviceMemory picture_memory = VK_NULL_HANDLE;

    std::string gpu_name, error;
    bool validation = false;
};

State g;
std::atomic<u64> g_messages{0}, g_presented{0};
std::mutex g_report_lock;
std::string g_first_message;

VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                          VkDebugUtilsMessageTypeFlagsEXT,
                                          const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    u64 n = g_messages++;
    const char* kind = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning";
    if (n < 20) std::fprintf(stderr, "Vulkan validation %s: %s\n", kind, data->pMessage);
    if (n == 0) {
        std::lock_guard lock(g_report_lock);
        g_first_message = data->pMessage;
    }
    return VK_FALSE;
}

bool check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    g.error = std::format("{} failed ({})", what, int(r));
    return false;
}

u32 memory_type(u32 bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g.gpu, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}

bool has_layer(const char* name) {
    u32 n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    for (const auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

bool make_instance(HINSTANCE inst, HWND window) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Mensis";
    app.apiVersion = VK_API_VERSION_1_3;
    std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    const char* layer = "VK_LAYER_KHRONOS_validation";
    g.validation = has_layer(layer);
    if (g.validation) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mi.pfnUserCallback = on_message;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = u32(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    if (g.validation) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
        ci.pNext = &mi;  // so creating the instance is checked too
    }
    if (!check(vkCreateInstance(&ci, nullptr, &g.instance), "vkCreateInstance")) return false;
    if (g.validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(g.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create) create(g.instance, &mi, nullptr, &g.messenger);
    }
    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    si.hinstance = inst;
    si.hwnd = window;
    return check(vkCreateWin32SurfaceKHR(g.instance, &si, nullptr, &g.surface), "vkCreateWin32SurfaceKHR");
}

bool pick_gpu() {
    u32 n = 0;
    vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(g.instance, &n, gpus.data());
    int best = -1;
    for (VkPhysicalDevice p : gpus) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(p, &pr);
        u32 qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, qs.data());
        for (u32 i = 0; i < qn; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(p, i, g.surface, &present);
            if (!present || !(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best) {
                best = score;
                g.gpu = p;
                g.family = i;
                g.gpu_name = pr.deviceName;
            }
            break;
        }
    }
    if (!g.gpu) {
        g.error = "no GPU can draw to the window through Vulkan";
        return false;
    }
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = g.family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    const char* extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = &extension;
    if (!check(vkCreateDevice(g.gpu, &di, nullptr, &g.device), "vkCreateDevice")) return false;
    vkGetDeviceQueue(g.device, g.family, 0, &g.queue);

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = g.family;
    if (!check(vkCreateCommandPool(g.device, &pi, nullptr, &g.pool), "vkCreateCommandPool")) return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (!check(vkAllocateCommandBuffers(g.device, &ai, &g.cmd), "vkAllocateCommandBuffers")) return false;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    return check(vkCreateFence(g.device, &fi, nullptr, &g.done), "vkCreateFence") &&
           check(vkCreateSemaphore(g.device, &sem, nullptr, &g.acquired), "vkCreateSemaphore");
}

void drop_swapchain() {
    for (VkSemaphore s : g.drawn) vkDestroySemaphore(g.device, s, nullptr);
    g.drawn.clear();
    g.images.clear();
    if (g.swapchain) vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
    g.swapchain = VK_NULL_HANDLE;
}

// The swapchain for a client area of w x h; false if there is nothing to draw to (minimised).
bool make_swapchain(u32 w, u32 h) {
    vkDeviceWaitIdle(g.device);
    drop_swapchain();
    VkSurfaceCapabilitiesKHR caps;
    if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.gpu, g.surface, &caps), "surface capabilities"))
        return false;
    if (caps.currentExtent.width != 0xffffffffu) {
        w = caps.currentExtent.width;
        h = caps.currentExtent.height;
    }
    if (!w || !h) return false;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        g.error = "the window's images can't be copied to";
        return false;
    }
    u32 n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.gpu, g.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.gpu, g.surface, &n, formats.data());
    // Not an sRGB format: the game's buffer already holds the colours as they are to be shown.
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{} : formats[0];
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) chosen = f;
    g.format = chosen.format;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g.surface;
    ci.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = {w, h};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always there: shown at the screen's refresh
    ci.clipped = VK_TRUE;
    if (!check(vkCreateSwapchainKHR(g.device, &ci, nullptr, &g.swapchain), "vkCreateSwapchainKHR")) return false;
    g.extent = {w, h};
    vkGetSwapchainImagesKHR(g.device, g.swapchain, &n, nullptr);
    g.images.resize(n);
    vkGetSwapchainImagesKHR(g.device, g.swapchain, &n, g.images.data());
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    g.drawn.resize(n);
    for (auto& s : g.drawn)
        if (!check(vkCreateSemaphore(g.device, &sem, nullptr, &s), "vkCreateSemaphore")) return false;
    return true;
}

void drop_picture() {
    if (g.staging_memory) vkFreeMemory(g.device, g.staging_memory, nullptr);
    if (g.staging) vkDestroyBuffer(g.device, g.staging, nullptr);
    if (g.picture_memory) vkFreeMemory(g.device, g.picture_memory, nullptr);
    if (g.picture) vkDestroyImage(g.device, g.picture, nullptr);
    g.staging = VK_NULL_HANDLE;
    g.staging_memory = VK_NULL_HANDLE;
    g.picture = VK_NULL_HANDLE;
    g.picture_memory = VK_NULL_HANDLE;
    g.mapped = nullptr;
    g.width = g.height = 0;
}

bool make_picture(u32 w, u32 h) {
    vkDeviceWaitIdle(g.device);
    drop_picture();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(w) * h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!check(vkCreateBuffer(g.device, &bi, nullptr, &g.staging), "vkCreateBuffer")) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g.device, g.staging, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!check(vkAllocateMemory(g.device, &ai, nullptr, &g.staging_memory), "vkAllocateMemory")) return false;
    vkBindBufferMemory(g.device, g.staging, g.staging_memory, 0);
    if (!check(vkMapMemory(g.device, g.staging_memory, 0, VK_WHOLE_SIZE, 0, &g.mapped), "vkMapMemory")) return false;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;  // the PS4's A8R8G8B8: B, G, R, A in memory
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!check(vkCreateImage(g.device, &ii, nullptr, &g.picture), "vkCreateImage")) return false;
    vkGetImageMemoryRequirements(g.device, g.picture, &mr);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!check(vkAllocateMemory(g.device, &ai, nullptr, &g.picture_memory), "vkAllocateMemory")) return false;
    vkBindImageMemory(g.device, g.picture, g.picture_memory, 0);
    g.width = w;
    g.height = h;
    return true;
}

void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
             VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(g.cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

bool open(void* instance, void* window, std::string& error) {
    bool ok = make_instance(static_cast<HINSTANCE>(instance), static_cast<HWND>(window)) && pick_gpu();
    if (ok) {
        // the blit scales with linear filtering, which the swapchain's format must allow
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g.gpu, VK_FORMAT_B8G8R8A8_UNORM, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
            g.error = "the GPU can't scale the picture with filtering";
            ok = false;
        }
    }
    error = g.error;
    if (!ok) g.device = VK_NULL_HANDLE;  // present() does nothing; the window uses GDI
    return ok;
}

void present(const u32* pixels, u32 width, u32 height, u32 window_width, u32 window_height) {
    if (!g.device) return;
    vkWaitForFences(g.device, 1, &g.done, VK_TRUE, UINT64_MAX);
    if (!window_width || !window_height) return;  // minimised
    if (!g.swapchain || g.extent.width != window_width || g.extent.height != window_height)
        if (!make_swapchain(window_width, window_height)) return;
    if (width && (width != g.width || height != g.height))
        if (!make_picture(width, height)) return;

    u32 index = 0;
    VkResult r = vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX, g.acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g.extent = {};  // remade next time
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
    vkResetFences(g.device, 1, &g.done);

    VkImage target = g.images[index];
    vkResetCommandBuffer(g.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g.cmd, &bi);
    barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (width && g.picture) {
        std::memcpy(g.mapped, pixels, size_t(width) * height * 4);
        barrier(g.picture, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(g.cmd, g.staging, g.picture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(g.picture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int(width), int(height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {int(g.extent.width), int(g.extent.height), 1};
        vkCmdBlitImage(g.cmd, g.picture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    } else {
        VkClearColorValue black{};  // before the first flip
        VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(g.cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    }
    barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(g.cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g.acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.drawn[index];
    if (vkQueueSubmit(g.queue, 1, &si, g.done) != VK_SUCCESS) return;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.drawn[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swapchain;
    pi.pImageIndices = &index;
    r = vkQueuePresentKHR(g.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g.extent = {};
    g_presented++;
}

std::vector<std::string> report() {
    if (g.gpu_name.empty() && g.error.empty()) return {};
    if (!g.device)
        return {std::format("Window: drawn through GDI, Vulkan couldn't be used: {}.\n", g.error)};
    std::string s = std::format("Window: drawn through Vulkan on the {}, {} pictures presented", g.gpu_name,
                                g_presented.load());
    if (!g.validation)
        s += "; the validation layer isn't installed (it comes with the Vulkan SDK)";
    else if (u64 n = g_messages.load())
        s += std::format("; the validation layer reported {} problems, the first: {}", n, [] {
                 std::lock_guard lock(g_report_lock);
                 return g_first_message;
             }());
    else
        s += "; the validation layer reported no problems";
    return {s + ".\n"};
}

}  // namespace mensis::vk
