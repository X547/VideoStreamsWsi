#pragma once

#include "Layer.h"

// Surface
VkResult VKAPI_CALL Layer_CreateHeadlessSurfaceEXT(VkInstance instance, const VkHeadlessSurfaceCreateInfoEXT *createInfo, const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface);
void     VKAPI_CALL Layer_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *allocator);
VkResult VKAPI_CALL Layer_GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physDev, const VkPhysicalDeviceSurfaceInfo2KHR *surface_info, VkSurfaceCapabilities2KHR *capabilities);
VkResult VKAPI_CALL Layer_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physDev, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *capabilities);
VkResult VKAPI_CALL Layer_GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physDev, const VkPhysicalDeviceSurfaceInfo2KHR *surface_info, uint32_t *count, VkSurfaceFormat2KHR *formats);
VkResult VKAPI_CALL Layer_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physDev, VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *formats);
VkResult VKAPI_CALL Layer_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physDev, VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *modes);
VkResult VKAPI_CALL Layer_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physDev, uint32_t index, VkSurfaceKHR surface, VkBool32 *supported);
VkResult VKAPI_CALL Layer_GetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pRectCount, VkRect2D* pRects);
VkResult VKAPI_CALL Layer_GetDeviceGroupSurfacePresentModesKHR(VkDevice device, VkSurfaceKHR surface, VkDeviceGroupPresentModeFlagsKHR *pModes);

// Swapchain
VkResult VKAPI_CALL Layer_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *createInfo, const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain);
void     VKAPI_CALL Layer_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator);
VkResult VKAPI_CALL Layer_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images);
VkResult VKAPI_CALL Layer_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex);
VkResult VKAPI_CALL Layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);
