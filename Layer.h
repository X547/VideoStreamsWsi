#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#define VkCheckRet(err) {VkResult _err = (err); if (_err != VK_SUCCESS) return _err;}


#define INSTANCE_HOOK_LIST(REQUIRED, OPTIONAL) \
	REQUIRED(DestroyInstance) \
	REQUIRED(EnumerateDeviceExtensionProperties) \
	REQUIRED(GetPhysicalDeviceImageFormatProperties) \
	REQUIRED(GetPhysicalDeviceMemoryProperties) \
	REQUIRED(GetPhysicalDeviceProperties)

#define DEVICE_HOOK_LIST(REQUIRED, OPTIONAL) \
	REQUIRED(DestroyDevice) \
	REQUIRED(AllocateCommandBuffers) \
	REQUIRED(AllocateMemory) \
	REQUIRED(BindImageMemory) \
	REQUIRED(CreateCommandPool) \
	REQUIRED(CreateImage) \
	REQUIRED(DestroyCommandPool) \
	REQUIRED(DestroyImage) \
	REQUIRED(FreeCommandBuffers) \
	REQUIRED(FreeMemory) \
	REQUIRED(GetDeviceQueue) \
	REQUIRED(GetImageMemoryRequirements) \
	REQUIRED(GetImageSubresourceLayout) \
	REQUIRED(MapMemory) \
	REQUIRED(ResetFences) \
	REQUIRED(UnmapMemory) \
	REQUIRED(CreateFence) \
	REQUIRED(DestroyFence) \
	REQUIRED(WaitForFences) \
	REQUIRED(BeginCommandBuffer) \
	REQUIRED(CmdCopyImage) \
	REQUIRED(CmdBlitImage) \
	REQUIRED(CmdPipelineBarrier) \
	REQUIRED(EndCommandBuffer) \
	REQUIRED(QueueSubmit) \
	REQUIRED(QueueWaitIdle)


struct InstanceHooks {
		PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
		PFN_vkCreateInstance CreateInstance;

#define DISPATCH_TABLE_ENTRY(x) PFN_vk##x x{};
   INSTANCE_HOOK_LIST(DISPATCH_TABLE_ENTRY, DISPATCH_TABLE_ENTRY)
#undef DISPATCH_TABLE_ENTRY
};

struct DeviceHooks {
		PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
		PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
		PFN_vkCreateDevice CreateDevice;

#define DISPATCH_TABLE_ENTRY(x) PFN_vk##x x{};
   DEVICE_HOOK_LIST(DISPATCH_TABLE_ENTRY, DISPATCH_TABLE_ENTRY)
#undef DISPATCH_TABLE_ENTRY
};


class LayerInstance {
private:
	VkInstance fBaseInstance;
	InstanceHooks fHooks;

public:
	LayerInstance();
	~LayerInstance();
	VkResult Init(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance);

	PFN_vkVoidFunction GetInstanceProcAddr(const char* pName);

	static LayerInstance *FromHandle(VkInstance instance);
	VkInstance ToHandle() {return fBaseInstance;}
	static LayerInstance *FromPhysDev(VkPhysicalDevice physDev);
	InstanceHooks &Hooks() {return fHooks;}
};


class LayerDevice {
private:
	LayerInstance *fInstance;
	VkDevice fBaseDevice;
	VkPhysicalDevice fPhysDev;
	DeviceHooks fHooks;

public:
	LayerDevice(LayerInstance *instance);
	~LayerDevice();
	VkResult Init(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice);

	PFN_vkVoidFunction GetDeviceProcAddr(const char* pName);

	static LayerDevice *FromHandle(VkDevice device);
	VkDevice ToHandle() {return fBaseDevice;}
	LayerInstance *GetInstance() {return fInstance;}
	VkPhysicalDevice GetPhysDev() {return fPhysDev;}
	DeviceHooks &Hooks() {return fHooks;}
};
