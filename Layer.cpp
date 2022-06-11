#include "Layer.h"
#include "Wsi.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <map>

#include <OS.h>
#include <private/shared/AutoDeleter.h>
#include <private/shared/PthreadMutexLocker.h>


pthread_mutex_t sInstanceMapLock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
std::map<VkInstance, ObjectDeleter<LayerInstance>> sInstanceMap;
pthread_mutex_t sDeviceMapLock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
std::map<VkDevice, ObjectDeleter<LayerDevice>> sDeviceMap;


VkResult ExtensionProperties(const uint32_t count, const VkExtensionProperties *properties, uint32_t *pCount, VkExtensionProperties *pProperties)
{
	if (pProperties == NULL) {
		*pCount = count;
		return VK_SUCCESS;
	}
	memcpy(pProperties, properties, std::min<uint32_t>(count, *pCount));
	if (*pCount < count)
		return VK_INCOMPLETE;
	return VK_SUCCESS;
}


//#pragma mark - LayerInstance

LayerInstance::LayerInstance():
	fBaseInstance(VK_NULL_HANDLE)
{
}

LayerInstance::~LayerInstance()
{
}

VkResult LayerInstance::Init(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
{
  VkLayerInstanceCreateInfo *layerCreateInfo = (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;
  while(layerCreateInfo && !(layerCreateInfo->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && layerCreateInfo->function == VK_LAYER_LINK_INFO)) {
    layerCreateInfo = (VkLayerInstanceCreateInfo *)layerCreateInfo->pNext;
  }
	if (layerCreateInfo == NULL) return VK_ERROR_INITIALIZATION_FAILED;


	fHooks.GetInstanceProcAddr = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;
	fHooks.CreateInstance = (PFN_vkCreateInstance)fHooks.GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");

	VkCheckRet(fHooks.CreateInstance(pCreateInfo, pAllocator, pInstance));
	fBaseInstance = *pInstance;

#define REQUIRED(x) fHooks.x = (PFN_vk##x)fHooks.GetInstanceProcAddr(fBaseInstance, "vk" #x);
#define OPTIONAL(x)
	INSTANCE_HOOK_LIST(REQUIRED, OPTIONAL);
#undef REQUIRED
#undef OPTIONAL

	return VK_SUCCESS;
}

PFN_vkVoidFunction LayerInstance::GetInstanceProcAddr(const char* pName)
{
	return fHooks.GetInstanceProcAddr(fBaseInstance, pName);
}

LayerInstance *LayerInstance::FromHandle(VkInstance instance)
{
	PthreadMutexLocker lock(&sInstanceMapLock);
	auto it = sInstanceMap.find(instance);
	if (it == sInstanceMap.end()) return NULL;
	return it->second.Get();
}

LayerInstance *LayerInstance::FromPhysDev(VkPhysicalDevice physDev)
{
	(void)physDev;
	PthreadMutexLocker lock(&sInstanceMapLock);
	// !!!
	return sInstanceMap.begin()->second.Get();
}


//#pragma mark - LayerDevice

LayerDevice::LayerDevice(LayerInstance *instance): fInstance(instance), fBaseDevice(VK_NULL_HANDLE), fPhysDev(VK_NULL_HANDLE)
{}

LayerDevice::~LayerDevice()
{}

VkResult LayerDevice::Init(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
	fPhysDev = physicalDevice;
	
	VkLayerDeviceCreateInfo *layerCreateInfo = (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;
	while (layerCreateInfo && !(layerCreateInfo->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && layerCreateInfo->function == VK_LAYER_LINK_INFO)) {
		layerCreateInfo = (VkLayerDeviceCreateInfo *)layerCreateInfo->pNext;
	}
	
	if (layerCreateInfo == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	
	fHooks.GetInstanceProcAddr = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	fHooks.GetDeviceProcAddr = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	// move chain on for next layer
	layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

  fHooks.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)fHooks.GetDeviceProcAddr(fBaseDevice, "vkGetDeviceProcAddr");
  fHooks.CreateDevice = (PFN_vkCreateDevice)fHooks.GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice");

	const char *layerExtensions[] {
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
	};

	VkDeviceCreateInfo createInfoOverride;
	memcpy(&createInfoOverride, pCreateInfo, sizeof(createInfoOverride));
	createInfoOverride.enabledExtensionCount += B_COUNT_OF(layerExtensions);
	ArrayDeleter<const char*> extensionsOverride(new const char*[B_COUNT_OF(layerExtensions)]);
	createInfoOverride.ppEnabledExtensionNames = &extensionsOverride[0];
	memcpy(&extensionsOverride[0], pCreateInfo->ppEnabledExtensionNames, pCreateInfo->enabledExtensionCount * sizeof(const char*));
	memcpy(&extensionsOverride[pCreateInfo->enabledExtensionCount], layerExtensions, B_COUNT_OF(layerExtensions) * sizeof(const char*));

	VkCheckRet(fHooks.CreateDevice(physicalDevice, &createInfoOverride, pAllocator, pDevice));
	fBaseDevice = *pDevice;

#define REQUIRED(x) fHooks.x = (PFN_vk##x)fHooks.GetDeviceProcAddr(fBaseDevice, "vk" #x); if (fHooks.x == NULL) {fprintf(stderr, "[!] no function %s\n", "vk" #x); return VK_ERROR_INITIALIZATION_FAILED;}
#define OPTIONAL(x)
	DEVICE_HOOK_LIST(REQUIRED, OPTIONAL);
#undef REQUIRED
#undef OPTIONAL

	return VK_SUCCESS;
}

PFN_vkVoidFunction LayerDevice::GetDeviceProcAddr(const char* pName)
{
	return fHooks.GetDeviceProcAddr(fBaseDevice, pName);
}

LayerDevice *LayerDevice::FromHandle(VkDevice device)
{
	PthreadMutexLocker lock(&sDeviceMapLock);
	auto it = sDeviceMap.find(device);
	if (it == sDeviceMap.end()) return NULL;
	return it->second.Get();
}


//#pragma mark - hooks

static VkResult VKAPI_CALL Layer_CreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
{
	printf("VideoStreamsWsi: vkCreateInstance(%p)\n", (void*)*pInstance);

	ObjectDeleter<LayerInstance> layerInst(new LayerInstance());
	VkCheckRet(layerInst->Init(pCreateInfo, pAllocator, pInstance));

	PthreadMutexLocker lock(&sInstanceMapLock);
	sInstanceMap.emplace(layerInst->ToHandle(), layerInst.Get());
	layerInst.Detach();

	return VK_SUCCESS;
}

static void VKAPI_CALL Layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
	(void)pAllocator;
	printf("VideoStreamsWsi: vkDestroyInstance\n");
	PthreadMutexLocker lock(&sInstanceMapLock);
	sInstanceMap.erase(sInstanceMap.find(instance));
}

static VkResult VKAPI_CALL Layer_EnumerateInstanceLayerProperties(uint32_t *pCount, VkLayerProperties *pProperties)
{
	static const VkLayerProperties property = {"VK_LAYER_window_system_integration", VK_MAKE_VERSION(1, 0, VK_HEADER_VERSION), 1, "Window system integration layer"};
	if (pProperties == NULL) {
		*pCount = 1;
		return VK_SUCCESS;
	}
	memcpy(pProperties, &property, std::min<uint32_t>(1, *pCount));
	if (*pCount < 1)
		return VK_INCOMPLETE;
	return VK_SUCCESS;
}


static VkResult VKAPI_CALL Layer_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
	printf("VideoStreamsWsi: vkCreateDevice\n");
	
	printf("instance: %p\n", *(void**)physicalDevice);
	
	ObjectDeleter<LayerDevice> layerDev(new LayerDevice(LayerInstance::FromPhysDev(physicalDevice)));
	VkCheckRet(layerDev->Init(physicalDevice, pCreateInfo, pAllocator, pDevice));
	
	PthreadMutexLocker lock(&sDeviceMapLock);
	sDeviceMap.emplace(layerDev->ToHandle(), layerDev.Get());
	layerDev.Detach();
	
	return VK_SUCCESS;
}

static void VKAPI_CALL Layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
	(void)pAllocator;
	printf("VideoStreamsWsi: vkDestroyDevice\n");
	PthreadMutexLocker lock(&sDeviceMapLock);
	sDeviceMap.erase(sDeviceMap.find(device));
}

static VkResult VKAPI_CALL Layer_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char *pLayerName, uint32_t *pCount, VkExtensionProperties *pProperties)
{
	printf("VideoStreamsWsi: vkEnumerateDeviceExtensionProperties\n");
	if (pLayerName && !strcmp(pLayerName, "VK_LAYER_window_system_integration")) {
		static const VkExtensionProperties extensions[] = {
			{VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION}
		};
		return ExtensionProperties(B_COUNT_OF(extensions), extensions, pCount, pProperties);
	}

	return LayerInstance::FromPhysDev(physicalDevice)->Hooks().EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pCount, pProperties);
}


//#pragma mark - exports

#define GET_PROC_ADDR(func) if (!strcmp(pName, "vk" #func)) return (PFN_vkVoidFunction)&Layer_##func

extern "C" _EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName)
{
	//printf("VideoStreamsWsi: vkGetInstanceProcAddr(%p, \"%s\")\n", (void*)instance, pName);

	GET_PROC_ADDR(CreateInstance);
	GET_PROC_ADDR(DestroyInstance);
	GET_PROC_ADDR(CreateDevice);
	GET_PROC_ADDR(EnumerateInstanceLayerProperties);
	GET_PROC_ADDR(EnumerateDeviceExtensionProperties);

	// WSI surface
	GET_PROC_ADDR(CreateHeadlessSurfaceEXT);
	GET_PROC_ADDR(DestroySurfaceKHR);
	GET_PROC_ADDR(GetPhysicalDevicePresentRectanglesKHR);
	GET_PROC_ADDR(GetPhysicalDeviceSurfaceCapabilities2KHR);
	GET_PROC_ADDR(GetPhysicalDeviceSurfaceCapabilitiesKHR);
	GET_PROC_ADDR(GetPhysicalDeviceSurfaceFormats2KHR);
	GET_PROC_ADDR(GetPhysicalDeviceSurfaceFormatsKHR);
	GET_PROC_ADDR(GetPhysicalDeviceSurfacePresentModesKHR);
	GET_PROC_ADDR(GetPhysicalDeviceSurfaceSupportKHR);

	LayerInstance *layerInst = LayerInstance::FromHandle(instance);
	if (layerInst == NULL) return NULL;
	return layerInst->GetInstanceProcAddr(pName);
}

extern "C" _EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
	//printf("VideoStreamsWsi: vkGetDeviceProcAddr(%p, \"%s\")\n", (void*)device, pName);

	GET_PROC_ADDR(DestroyDevice);

	// WSI surface
	GET_PROC_ADDR(GetDeviceGroupSurfacePresentModesKHR);
	// WSI swapchain
	GET_PROC_ADDR(CreateSwapchainKHR);
	GET_PROC_ADDR(DestroySwapchainKHR);
	GET_PROC_ADDR(GetSwapchainImagesKHR);
	GET_PROC_ADDR(AcquireNextImageKHR);
	GET_PROC_ADDR(QueuePresentKHR);

	LayerDevice *layerDev = LayerDevice::FromHandle(device);
	if (layerDev == NULL) return NULL;
	return layerDev->GetDeviceProcAddr(pName);
}

extern "C" _EXPORT VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const VkEnumerateInstanceExtensionPropertiesChain *chain, const char *pLayerName, uint32_t *pCount, VkExtensionProperties *pProperties)
{
	printf("VideoStreamsWsi: vkEnumerateInstanceExtensionProperties\n");

	if (pLayerName && !strcmp(pLayerName, "VK_LAYER_window_system_integration")) {
		static const VkExtensionProperties extensions[] = {
			{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION}
		};
		return ExtensionProperties(B_COUNT_OF(extensions), extensions, pCount, pProperties);
	}

	return chain->CallDown(pLayerName, pCount, pProperties);
}

#undef GET_PROC_ADDR
