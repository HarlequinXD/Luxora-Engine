/* Vulkan function loader (volk-style). The Vulkan loader library is opened at runtime by the
 * platform layer, so nothing links against libvulkan / vulkan-1.lib at build time. */
#ifndef LX_VK_H
#define LX_VK_H
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdbool.h>

#define LX_VK_GLOBAL_FUNCS(X)                  \
    X(vkCreateInstance)                        \
    X(vkEnumerateInstanceExtensionProperties)  \
    X(vkEnumerateInstanceLayerProperties)      \
    X(vkEnumerateInstanceVersion)

#define LX_VK_INSTANCE_FUNCS(X)                      \
    X(vkDestroyInstance)                             \
    X(vkEnumeratePhysicalDevices)                    \
    X(vkGetPhysicalDeviceProperties)                 \
    X(vkGetPhysicalDeviceFeatures)                   \
    X(vkGetPhysicalDeviceQueueFamilyProperties)      \
    X(vkGetPhysicalDeviceMemoryProperties)           \
    X(vkGetPhysicalDeviceFormatProperties)           \
    X(vkCreateDevice)                                \
    X(vkGetDeviceProcAddr)                           \
    X(vkEnumerateDeviceExtensionProperties)          \
    X(vkDestroySurfaceKHR)                           \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)          \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)     \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)          \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)     \
    X(vkCreateDebugUtilsMessengerEXT)                \
    X(vkDestroyDebugUtilsMessengerEXT)

#define LX_VK_DEVICE_FUNCS(X)          \
    X(vkDestroyDevice)                 \
    X(vkGetDeviceQueue)                \
    X(vkDeviceWaitIdle)                \
    X(vkQueueSubmit)                   \
    X(vkQueueWaitIdle)                 \
    X(vkQueuePresentKHR)               \
    X(vkCreateSwapchainKHR)            \
    X(vkDestroySwapchainKHR)           \
    X(vkGetSwapchainImagesKHR)         \
    X(vkAcquireNextImageKHR)           \
    X(vkCreateImageView)               \
    X(vkDestroyImageView)              \
    X(vkCreateImage)                   \
    X(vkDestroyImage)                  \
    X(vkGetImageMemoryRequirements)    \
    X(vkBindImageMemory)               \
    X(vkCreateBuffer)                  \
    X(vkDestroyBuffer)                 \
    X(vkGetBufferMemoryRequirements)   \
    X(vkBindBufferMemory)              \
    X(vkAllocateMemory)                \
    X(vkFreeMemory)                    \
    X(vkMapMemory)                     \
    X(vkUnmapMemory)                   \
    X(vkCreateSampler)                 \
    X(vkDestroySampler)                \
    X(vkCreateShaderModule)            \
    X(vkDestroyShaderModule)           \
    X(vkCreatePipelineLayout)          \
    X(vkDestroyPipelineLayout)         \
    X(vkCreateGraphicsPipelines)       \
    X(vkDestroyPipeline)               \
    X(vkCreateDescriptorSetLayout)     \
    X(vkDestroyDescriptorSetLayout)    \
    X(vkCreateDescriptorPool)          \
    X(vkDestroyDescriptorPool)         \
    X(vkAllocateDescriptorSets)        \
    X(vkFreeDescriptorSets)            \
    X(vkUpdateDescriptorSets)          \
    X(vkCreateCommandPool)             \
    X(vkDestroyCommandPool)            \
    X(vkAllocateCommandBuffers)        \
    X(vkFreeCommandBuffers)            \
    X(vkBeginCommandBuffer)            \
    X(vkEndCommandBuffer)              \
    X(vkResetCommandBuffer)            \
    X(vkCreateFence)                   \
    X(vkDestroyFence)                  \
    X(vkWaitForFences)                 \
    X(vkResetFences)                   \
    X(vkCreateSemaphore)               \
    X(vkDestroySemaphore)              \
    X(vkCmdBeginRendering)             \
    X(vkCmdEndRendering)               \
    X(vkCmdPipelineBarrier)            \
    X(vkCmdBindPipeline)               \
    X(vkCmdBindDescriptorSets)         \
    X(vkCmdBindVertexBuffers)          \
    X(vkCmdBindIndexBuffer)            \
    X(vkCmdDraw)                       \
    X(vkCmdDrawIndexed)                \
    X(vkCmdSetViewport)                \
    X(vkCmdSetScissor)                 \
    X(vkCmdPushConstants)              \
    X(vkCmdCopyBuffer)                 \
    X(vkCmdCopyBufferToImage)          \
    X(vkCmdClearColorImage)            \
    X(vkCmdCopyImageToBuffer)          \
    X(vkCmdBlitImage)

#define LX_VK_DECLARE(name) extern PFN_##name name;
LX_VK_GLOBAL_FUNCS(LX_VK_DECLARE)
LX_VK_INSTANCE_FUNCS(LX_VK_DECLARE)
LX_VK_DEVICE_FUNCS(LX_VK_DECLARE)
#undef LX_VK_DECLARE

bool lx_vk_load_global(PFN_vkGetInstanceProcAddr gipa);
void lx_vk_load_instance(VkInstance instance);
bool lx_vk_load_device(VkDevice device);
const char *lx_vk_result(VkResult r);

#endif
