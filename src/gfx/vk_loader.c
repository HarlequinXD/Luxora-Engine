#include "../lx_internal.h"
#include "vk.h"

#define LX_VK_DEFINE(name) PFN_##name name;
LX_VK_GLOBAL_FUNCS(LX_VK_DEFINE)
LX_VK_INSTANCE_FUNCS(LX_VK_DEFINE)
LX_VK_DEVICE_FUNCS(LX_VK_DEFINE)
#undef LX_VK_DEFINE

static PFN_vkGetInstanceProcAddr g_gipa;

bool lx_vk_load_global(PFN_vkGetInstanceProcAddr gipa) {
    g_gipa = gipa;
    if (!gipa) return false;
#define LX_VK_LOAD(name) name = (PFN_##name)gipa(NULL, #name);
    LX_VK_GLOBAL_FUNCS(LX_VK_LOAD)
#undef LX_VK_LOAD
    return vkCreateInstance != NULL;
}

void lx_vk_load_instance(VkInstance instance) {
#define LX_VK_LOAD(name) name = (PFN_##name)g_gipa(instance, #name);
    LX_VK_INSTANCE_FUNCS(LX_VK_LOAD)
#undef LX_VK_LOAD
}

bool lx_vk_load_device(VkDevice device) {
    bool ok = true;
#define LX_VK_LOAD(name)                                                       \
    name = (PFN_##name)vkGetDeviceProcAddr(device, #name);                     \
    if (!name) name = (PFN_##name)g_gipa(VK_NULL_HANDLE, #name);
    LX_VK_DEVICE_FUNCS(LX_VK_LOAD)
#undef LX_VK_LOAD
    /* dynamic rendering may only be exposed through the KHR extension on 1.2 drivers */
    if (!vkCmdBeginRendering) vkCmdBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR");
    if (!vkCmdEndRendering) vkCmdEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR");
#define LX_VK_CHECK(name) if (!name) { LX_ERROR("Vulkan function missing: %s", #name); ok = false; }
    LX_VK_DEVICE_FUNCS(LX_VK_CHECK)
#undef LX_VK_CHECK
    return ok;
}

const char *lx_vk_result(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VK_ERROR (other)";
    }
}
