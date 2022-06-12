#include "Wsi.h"

#include <OS.h>

#include <VideoStreams/VideoProducer.h>

#include <private/shared/AutoDeleter.h>
#include <private/shared/AutoDeleterOS.h>
#include <private/shared/PthreadMutexLocker.h>

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <new>
#include <algorithm>
#include <cassert>

#include <Looper.h>
#include <Bitmap.h>

#include <xf86drm.h>

//#define AUTO_CONNECT_TO_SCREEN 1


#ifdef AUTO_CONNECT_TO_SCREEN
static bool FindConsumerGfx(BMessenger& consumer)
{
	BMessenger consumerApp("application/x-vnd.X512-RadeonGfx");
	if (!consumerApp.IsValid()) {
		printf("[!] No TestConsumer\n");
		return false;
	}
	for (int32 i = 0; ; i++) {
		BMessage reply;
		{
			BMessage scriptMsg(B_GET_PROPERTY);
			scriptMsg.AddSpecifier("Handler", i);
			consumerApp.SendMessage(&scriptMsg, &reply);
		}
		int32 error;
		if (reply.FindInt32("error", &error) >= B_OK && error < B_OK)
			return false;
		if (reply.FindMessenger("result", &consumer) >= B_OK) {
			BMessage scriptMsg(B_GET_PROPERTY);
			scriptMsg.AddSpecifier("InternalName");
			consumer.SendMessage(&scriptMsg, &reply);
			const char* name;
			if (reply.FindString("result", &name) >= B_OK && strcmp(name, "RadeonGfxConsumer") == 0)
				return true;
		}
	}
}
#endif

static uint32_t getMemoryTypeIndex(LayerDevice *lrDev, uint32_t typeBits, VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties deviceMemoryProperties;
	lrDev->GetInstance()->Hooks().GetPhysicalDeviceMemoryProperties(lrDev->GetPhysDev(), &deviceMemoryProperties);
	for (uint32_t i = 0; i < deviceMemoryProperties.memoryTypeCount; i++) {
		if ((typeBits & 1) == 1) {
			if ((deviceMemoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
				return i;
			}
		}
		typeBits >>= 1;
	}
	return 0;
}

static void insertImageMemoryBarrier(
	LayerDevice *lrDev,
	VkCommandBuffer cmdbuffer,
	VkImage image,
	VkAccessFlags srcAccessMask,
	VkAccessFlags dstAccessMask,
	VkImageLayout oldImageLayout,
	VkImageLayout newImageLayout,
	VkPipelineStageFlags srcStageMask,
	VkPipelineStageFlags dstStageMask,
	VkImageSubresourceRange subresourceRange
) {
	VkImageMemoryBarrier imageMemoryBarrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = srcAccessMask,
		.dstAccessMask = dstAccessMask,
		.oldLayout = oldImageLayout,
		.newLayout = newImageLayout,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = subresourceRange
	};

	lrDev->Hooks().CmdPipelineBarrier(
		cmdbuffer,
		srcStageMask,
		dstStageMask,
		0,
		0, nullptr,
		0, nullptr,
		1, &imageMemoryBarrier
	);
}

static VkResult submitWork(LayerDevice *lrDev, VkCommandBuffer cmdBuffer, VkQueue queue)
{
	VkSubmitInfo submitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmdBuffer;
	VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence;
	VkCheckRet(lrDev->Hooks().CreateFence(lrDev->ToHandle(), &fenceInfo, nullptr, &fence));
	VkCheckRet(lrDev->Hooks().QueueSubmit(queue, 1, &submitInfo, fence));
	VkCheckRet(lrDev->Hooks().WaitForFences(lrDev->ToHandle(), 1, &fence, VK_TRUE, UINT64_MAX));
	lrDev->Hooks().DestroyFence(lrDev->ToHandle(), fence, nullptr);
	return VK_SUCCESS;
}


//#pragma mark -

class VKLayerSwapchain;

class VKLayerImage {
private:
	LayerDevice *fDevice;
	VkImage fImage;
	VkDeviceMemory fMemory;

public:
	VKLayerImage();
	~VKLayerImage();
	VkResult Init(LayerDevice *device, const VkImageCreateInfo &createInfo, bool cpuMem = false, area_id *area = NULL);

	VkResult ToVideoBuffer(VideoBuffer &vidBuf, VkImageCreateInfo &imgInfo);

	VkImage ToHandle() {return fImage;}
	VkDeviceMemory GetMemoryHandle() {return fMemory;}
};

class BitmapHook;

class VKLayerSurfaceBase {
public:
	virtual ~VKLayerSurfaceBase() {};
	virtual void SetBitmapHook(BitmapHook *hook) = 0;
};

class VKLayerSurface: public VKLayerSurfaceBase, public VideoProducer {
private:
	LayerInstance *fInstance = NULL;
	VKLayerSwapchain *fSwapchain = NULL;
	BLooper *fLooper = NULL;

	friend class VKLayerSwapchain;

public:
	VKLayerSurface();
	virtual ~VKLayerSurface();
	VkResult Init(LayerInstance *instance, const VkHeadlessSurfaceCreateInfoEXT &createInfo);

	VkResult GetCapabilities(VkPhysicalDevice physDev, VkSurfaceCapabilitiesKHR *capabilities);
	VkResult GetFormats(VkPhysicalDevice physDev, uint32_t *count, VkSurfaceFormatKHR *formats);
	VkResult GetPresentModes(VkPhysicalDevice physDev, uint32_t *count, VkPresentModeKHR *modes);
	VkResult GetPresentRectangles(VkPhysicalDevice physDev, uint32_t* pRectCount, VkRect2D* pRects);

	static VKLayerSurface *FromHandle(VkSurfaceKHR surface) {return (VKLayerSurface*)surface;}
	VkSurfaceKHR ToHandle() {return (VkSurfaceKHR)this;}

	// VKLayerSurfaceBase
	void SetBitmapHook(BitmapHook *hook) override;

	// VideoProducer
	void Connected(bool isActive) final;
	void SwapChainChanged(bool isValid) final;
	void Presented() final;
};

class VKLayerSwapchain {
private:
	LayerDevice *fDevice;
	VKLayerSurface *fSurface;
	VkExtent2D fImageExtent;
	uint32 fImageCnt;
	ArrayDeleter<VKLayerImage> fImages;
	VkQueue fQueue = VK_NULL_HANDLE;
	VkFence fFence = VK_NULL_HANDLE;
	bool fRetired = false;

	ObjectDeleter<BBitmap> fBitmap;
	AreaDeleter fBitmapArea;
	BBitmap *fCurBitmap;

	VkImageCreateInfo ImageFromCreateInfo(const VkSwapchainCreateInfoKHR &createInfo);
	VkResult CheckSuboptimal();

public:
	VKLayerSwapchain();
	~VKLayerSwapchain();
	VkResult Init(LayerDevice *device, const VkSwapchainCreateInfoKHR &createInfo);

	VkResult GetSwapchainImages(uint32_t *count, VkImage *images);
	VkResult AcquireNextImage(const VkAcquireNextImageInfoKHR *pAcquireInfo, uint32_t *pImageIndex);
	VkResult QueuePresent(VkQueue queue, const VkPresentInfoKHR *present_info, uint32_t idx);

	static VKLayerSwapchain *FromHandle(VkSwapchainKHR surface) {return (VKLayerSwapchain*)surface;}
	VkSwapchainKHR ToHandle() {return (VkSwapchainKHR)this;}
};


//#pragma mark - VKLayerImage

VKLayerImage::VKLayerImage():
	fDevice(NULL), fImage(0), fMemory(0)
{}

VKLayerImage::~VKLayerImage()
{
	fDevice->Hooks().DestroyImage(fDevice->ToHandle(), fImage, NULL);
	fDevice->Hooks().FreeMemory(fDevice->ToHandle(), fMemory, NULL);
}

VkResult VKLayerImage::Init(LayerDevice *device, const VkImageCreateInfo &createInfo, bool cpuMem, area_id *area)
{
	fDevice = device;

	VkCheckRet(fDevice->Hooks().CreateImage(fDevice->ToHandle(), &createInfo, NULL, &fImage));

	VkMemoryRequirements memRequirements;
	fDevice->Hooks().GetImageMemoryRequirements(fDevice->ToHandle(), fImage, &memRequirements);
	size_t memTypeIdx = 0;
	if (cpuMem) {
		memTypeIdx = getMemoryTypeIndex(fDevice, memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	} else {
		for (; memTypeIdx < 8 * sizeof(memRequirements.memoryTypeBits); ++memTypeIdx) {
			if (memRequirements.memoryTypeBits & (1u << memTypeIdx))
				break;
		}
		assert(memTypeIdx <= 8 * sizeof(memRequirements.memoryTypeBits) - 1);
	}

	VkMemoryDedicatedAllocateInfo dedicateInfo{
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		.image = fImage,
	};

	VkMemoryAllocateInfo memAllocInfo{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &dedicateInfo,
		.allocationSize = memRequirements.size,
		.memoryTypeIndex = (uint32_t)memTypeIdx
	};

	VkImportMemoryHostPointerInfoEXT hostPtrInfo;
	AreaDeleter memArea;
	if (area != NULL) {
		void *memAreaAdr = NULL;
		memArea.SetTo(create_area("WSI image", &memAreaAdr, B_ANY_ADDRESS, memRequirements.size, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA));
		if (!memArea.IsSet())
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		hostPtrInfo = {
			.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
			.pHostPointer = memAreaAdr
		};
		memAllocInfo.pNext = &hostPtrInfo;
	}

	VkCheckRet(fDevice->Hooks().AllocateMemory(fDevice->ToHandle(), &memAllocInfo, nullptr, &fMemory));
	VkCheckRet(fDevice->Hooks().BindImageMemory(fDevice->ToHandle(), fImage, fMemory, 0));

	if (area != NULL) {*area = memArea.Detach();}
	return VK_SUCCESS;
}

VkResult VKLayerImage::ToVideoBuffer(VideoBuffer &vidBuf, VkImageCreateInfo &imgInfo)
{
	VkImageSubresource subresource {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
	VkSubresourceLayout subresourceLayout {};
	fDevice->Hooks().GetImageSubresourceLayout(fDevice->ToHandle(), ToHandle(), &subresource, &subresourceLayout);

	int memFd = -1;
	VkMemoryGetFdInfoKHR getFdInfo {
		.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
		.memory = GetMemoryHandle(),
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
	};
	VkResult res = fDevice->Hooks().GetMemoryFdKHR(fDevice->ToHandle(), &getFdInfo, &memFd);

	uint32_t boHandle = UINT32_MAX;
	int devFd = open("/dev/null", O_RDWR | O_CLOEXEC);
	drmPrimeFDToHandle(devFd, memFd, &boHandle);
	close(devFd); devFd = -1;

	vidBuf.ref.offset = subresourceLayout.offset;
	vidBuf.ref.size = subresourceLayout.size;
	vidBuf.ref.kind = bufferRefGpu;
	vidBuf.ref.gpu.id = boHandle;
	vidBuf.ref.gpu.team = getpid();
	vidBuf.format.bytesPerRow = subresourceLayout.rowPitch;
	vidBuf.format.width = imgInfo.extent.width;
	vidBuf.format.height = imgInfo.extent.height;
	vidBuf.format.colorSpace = B_RGBA32;

	return VK_SUCCESS;
}


//#pragma mark - VKLayerSurface

VKLayerSurface::VKLayerSurface()
{}

VKLayerSurface::~VKLayerSurface()
{
	fLooper->Quit();
}

VkResult VKLayerSurface::Init(LayerInstance *instance, const VkHeadlessSurfaceCreateInfoEXT &createInfo)
{
	(void)createInfo;
	fInstance = instance;

	fLooper = new BLooper("VKLayerSurface");
	fLooper->AddHandler(this);
	fLooper->Run();

#ifdef AUTO_CONNECT_TO_SCREEN
	BMessenger consumer;
	while (!FindConsumerGfx(consumer)) {
		snooze(100000);
	}
	printf("consumer: "); WriteMessenger(consumer); printf("\n");

	LockLooper();
	if (ConnectTo(consumer) < B_OK) {
		UnlockLooper();
		printf("[!] can't connect to consumer\n");
		return VK_ERROR_UNKNOWN;
	}
	UnlockLooper();
#endif

	return VK_SUCCESS;
}

VkResult VKLayerSurface::GetCapabilities(VkPhysicalDevice physDev, VkSurfaceCapabilitiesKHR *surfaceCapabilities)
{
	/* Image count limits */
	surfaceCapabilities->minImageCount = 1;
	surfaceCapabilities->maxImageCount = 3;

	/* Surface extents */
	if (true)
		surfaceCapabilities->currentExtent = {1920, 1080};
	else
		surfaceCapabilities->currentExtent = {(uint32_t)-1, (uint32_t)-1};

	surfaceCapabilities->minImageExtent = {1, 1};
	/* Ask the device for max */
	VkPhysicalDeviceProperties devProps;
	fInstance->Hooks().GetPhysicalDeviceProperties(physDev, &devProps);

	surfaceCapabilities->maxImageExtent = {
		devProps.limits.maxImageDimension2D, devProps.limits.maxImageDimension2D
	};
	surfaceCapabilities->maxImageArrayLayers = 1;

	/* Surface transforms */
	surfaceCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	surfaceCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;

	/* Composite alpha */
	surfaceCapabilities->supportedCompositeAlpha = (VkCompositeAlphaFlagBitsKHR)(
		VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR | VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR |
		VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR | VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR
	);

	/* Image usage flags */
	surfaceCapabilities->supportedUsageFlags =
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
	
	return VK_SUCCESS;
}

VkResult VKLayerSurface::GetFormats(VkPhysicalDevice physDev, uint32_t *count, VkSurfaceFormatKHR *surfaceFormats)
{
/*
	VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM};
	uint32_t formatCnt = 1;
*/
	constexpr int max_core_1_0_formats = VK_FORMAT_ASTC_12x12_SRGB_BLOCK + 1;
	VkFormat formats[max_core_1_0_formats];
	uint32_t formatCnt = 0;
	
	for (int format = 0; format < max_core_1_0_formats; format++) {
		VkImageFormatProperties formatProps;
		VkResult res = fInstance->Hooks().GetPhysicalDeviceImageFormatProperties(
			physDev, (VkFormat)format, VK_IMAGE_TYPE_2D,
			VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
			&formatProps
		);
		if (res != VK_ERROR_FORMAT_NOT_SUPPORTED) {
			formats[formatCnt++] = (VkFormat)format;
		}
	}

	if (surfaceFormats == NULL) {
		*count = formatCnt;
		return VK_SUCCESS;
	}
	memcpy(surfaceFormats, formats, sizeof(VkFormat)*std::min<uint32_t>(*count, formatCnt));
	if (*count < formatCnt)
		return VK_INCOMPLETE;
	return VK_SUCCESS;
}

VkResult VKLayerSurface::GetPresentModes(VkPhysicalDevice physDev, uint32_t *count, VkPresentModeKHR *presentModes)
{
	(void)physDev;
	static const VkPresentModeKHR modes[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR};
	if (presentModes == NULL) {
		*count = B_COUNT_OF(modes);
		return VK_SUCCESS;
	}
	memcpy(presentModes, modes, sizeof(VkPresentModeKHR)*std::min<uint32_t>(*count, B_COUNT_OF(modes)));
	if (*count < B_COUNT_OF(modes))
		return VK_INCOMPLETE;
	return VK_SUCCESS;
}

VkResult VKLayerSurface::GetPresentRectangles(VkPhysicalDevice physDev, uint32_t* pRectCount, VkRect2D* pRects)
{
	if (pRects == NULL) {
		*pRectCount = 1;
		return VK_SUCCESS;
	}
	if (*pRectCount < 1) {
		return VK_INCOMPLETE;
	}
	VkSurfaceCapabilitiesKHR caps;
	VkCheckRet(GetCapabilities(physDev, &caps));
	pRects[0].offset.x = 0;
	pRects[0].offset.y = 0;
	pRects[0].extent = caps.currentExtent;
	return VK_SUCCESS;
}

void VKLayerSurface::SetBitmapHook(BitmapHook *hook)
{
}


//#pragma mark - VideoProducer interface

void VKLayerSurface::Connected(bool isActive)
{
	VideoProducer::Connected(isActive);
}

void VKLayerSurface::SwapChainChanged(bool isValid)
{
	VideoProducer::SwapChainChanged(isValid);
}

void VKLayerSurface::Presented()
{
	VideoProducer::Presented();
}


//#pragma mark - VKLayerSwapchain

VKLayerSwapchain::VKLayerSwapchain()
{}

VKLayerSwapchain::~VKLayerSwapchain()
{
	fDevice->Hooks().DestroyFence(fDevice->ToHandle(), fFence, NULL);

	if (!fRetired) {
		fSurface->fSwapchain = NULL;
	}
}

VkImageCreateInfo VKLayerSwapchain::ImageFromCreateInfo(const VkSwapchainCreateInfoKHR &createInfo)
{
	return VkImageCreateInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = nullptr,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = createInfo.imageFormat,
		.extent = {
			createInfo.imageExtent.width,
			createInfo.imageExtent.height,
			1
		},
		.mipLevels = 1,
		.arrayLayers = createInfo.imageArrayLayers,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_LINEAR,
		.usage = createInfo.imageUsage,
		.sharingMode = createInfo.imageSharingMode,
		.queueFamilyIndexCount = createInfo.queueFamilyIndexCount,
		.pQueueFamilyIndices = createInfo.pQueueFamilyIndices,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
	};
}

VkResult VKLayerSwapchain::CheckSuboptimal()
{
#if 0
	auto bitmapHook = fSurface->GetBitmapHook();
	if (bitmapHook == NULL)
		return VK_SUCCESS;
	uint32_t width, height;
	bitmapHook->GetSize(width, height);

	if (!(fImageExtent.width == width && fImageExtent.height == height))
		return VK_SUBOPTIMAL_KHR;
#endif
	return VK_SUCCESS;
}

VkResult VKLayerSwapchain::Init(LayerDevice *device, const VkSwapchainCreateInfoKHR &createInfo)
{
	fDevice = device;
	fSurface = VKLayerSurface::FromHandle(createInfo.surface);

	VKLayerSwapchain *oldSwapchain = NULL;
	if (createInfo.oldSwapchain != NULL) {
		if (fSurface->fSwapchain == NULL || createInfo.oldSwapchain != fSurface->fSwapchain->ToHandle()) {
			return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
		}
		oldSwapchain = VKLayerSwapchain::FromHandle(createInfo.oldSwapchain);
	}

	VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
	VkCheckRet(fDevice->Hooks().CreateFence(fDevice->ToHandle(), &fence_info, NULL, &fFence));

	fImageExtent = createInfo.imageExtent;

	VkImageCreateInfo imageCreateInfo = ImageFromCreateInfo(createInfo);

	fImageCnt = createInfo.minImageCount;
	fImages.SetTo(new(std::nothrow) VKLayerImage[fImageCnt]);
	if (!fImages.IsSet())
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	for (uint32_t i = 0; i < fImageCnt; i++) {
		VkCheckRet(fImages[i].Init(device, imageCreateInfo));
	}

	fDevice->Hooks().GetDeviceQueue(fDevice->ToHandle(), 0, 0, &fQueue);

	ArrayDeleter<VideoBuffer> vidBufs(new VideoBuffer[fImageCnt]);
	for (uint32 i = 0; i < fImageCnt; i++) {
		memset(&vidBufs[i], 0, sizeof(VideoBuffer));
		vidBufs[i].id = i;
		VkCheckRet(fImages[i].ToVideoBuffer(vidBufs[i], imageCreateInfo));
	}
	SwapChain swapChain {
		.size = sizeof(SwapChain),
		.presentEffect = presentEffectSwap,
		.bufferCnt = fImageCnt,
		.buffers = &vidBufs[0]
	};

	fSurface->LockLooper();
	fSurface->SetSwapChain(&swapChain);
	fSurface->UnlockLooper();

	if (oldSwapchain != NULL) {
		oldSwapchain->fRetired = true;
	}
	fSurface->fSwapchain = this;

	return VK_SUCCESS;
}

VkResult VKLayerSwapchain::GetSwapchainImages(uint32_t *count, VkImage *images)
{
	if (images == NULL) {
		*count = fImageCnt;
		return VK_SUCCESS;
	}
	uint32_t copyCnt = std::min<uint32_t>(*count, fImageCnt);
	for (uint32_t i = 0; i < copyCnt; i++) {
		images[i] = fImages[i].ToHandle();
	}
	if (*count < fImageCnt)
		return VK_INCOMPLETE;
	return VK_SUCCESS;
}

VkResult VKLayerSwapchain::AcquireNextImage(const VkAcquireNextImageInfoKHR *pAcquireInfo, uint32_t *pImageIndex)
{
	int32 imageIdx = -1;
	for (;;) {
		fSurface->LockLooper();
		imageIdx = fSurface->AllocBuffer();
		fSurface->UnlockLooper();
		if (imageIdx >= 0) break;
		snooze(1000);
	}
	*pImageIndex = imageIdx;

	if (VK_NULL_HANDLE != pAcquireInfo->semaphore || VK_NULL_HANDLE != pAcquireInfo->fence) {
		VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	
		if (VK_NULL_HANDLE != pAcquireInfo->semaphore) {
			submit.signalSemaphoreCount = 1;
			submit.pSignalSemaphores = &pAcquireInfo->semaphore;
		}

		submit.commandBufferCount = 0;
		submit.pCommandBuffers = nullptr;
		VkCheckRet(fDevice->Hooks().QueueSubmit(fQueue, 1, &submit, pAcquireInfo->fence));
	}

	return CheckSuboptimal();
}

VkResult VKLayerSwapchain::QueuePresent(VkQueue queue, const VkPresentInfoKHR *presentInfo, uint32_t idx)
{
	fDevice->Hooks().ResetFences(fDevice->ToHandle(), 1, &fFence);
	VkPipelineStageFlags pipeline_stage_flags = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

	VkSubmitInfo submit_info = {
		VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, presentInfo->waitSemaphoreCount, presentInfo->pWaitSemaphores, &pipeline_stage_flags, 0, NULL, 0, NULL
	};

	VkResult result = fDevice->Hooks().QueueSubmit(queue, 1, &submit_info, fFence);
	if (result == VK_SUCCESS) {
		fDevice->Hooks().WaitForFences(fDevice->ToHandle(), 1, &fFence, VK_TRUE, UINT64_MAX);
	}

	uint32_t imageIdx = presentInfo->pImageIndices[idx];

	fSurface->LockLooper();
	fSurface->Present(imageIdx);
	fSurface->UnlockLooper();

	return CheckSuboptimal();
}


//#pragma mark - Surface

VkResult Layer_CreateHeadlessSurfaceEXT(VkInstance instance, const VkHeadlessSurfaceCreateInfoEXT *createInfo, const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
	(void)allocator;
	auto wineSurface = new(std::nothrow) VKLayerSurface();
	if (wineSurface == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	VkCheckRet(wineSurface->Init(LayerInstance::FromHandle(instance), *createInfo));
	*surface = wineSurface->ToHandle();
	return VK_SUCCESS;
}

void Layer_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *allocator)
{
	(void)instance;
	(void)allocator;
	delete VKLayerSurface::FromHandle(surface);
}

VkResult Layer_GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physDev, const VkPhysicalDeviceSurfaceInfo2KHR *surface_info, VkSurfaceCapabilities2KHR *capabilities)
{
	(void)physDev;
	(void)surface_info;
	(void)capabilities;
	fprintf(stderr, "vkGetPhysicalDeviceSurfaceCapabilities2KHR(): not implemented\n");
	return VK_NOT_READY;
}

VkResult Layer_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physDev, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *capabilities)
{
	return VKLayerSurface::FromHandle(surface)->GetCapabilities(physDev, capabilities);
}

VkResult Layer_GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physDev, const VkPhysicalDeviceSurfaceInfo2KHR *surface_info, uint32_t *count, VkSurfaceFormat2KHR *formats)
{
	(void)physDev;
	(void)surface_info;
	(void)count;
	(void)formats;
	fprintf(stderr, "vkGetPhysicalDeviceSurfaceFormats2KHR(): not implemented\n");
	return VK_NOT_READY;
}

VkResult Layer_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physDev, VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *formats)
{
	return VKLayerSurface::FromHandle(surface)->GetFormats(physDev, count, formats);
}

VkResult Layer_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physDev, VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *modes)
{
	return VKLayerSurface::FromHandle(surface)->GetPresentModes(physDev, count, modes);
}

VkResult Layer_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physDev, uint32_t index, VkSurfaceKHR surface, VkBool32 *supported)
{
	(void)physDev;
	(void)index;
	(void)surface;
	*supported = VK_TRUE;
	return VK_SUCCESS;
}

VkResult VKAPI_CALL Layer_GetDeviceGroupSurfacePresentModesKHR(VkDevice device, VkSurfaceKHR surface, VkDeviceGroupPresentModeFlagsKHR *pModes)
{
	(void)device;
	(void)surface;
	*pModes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
	return VK_SUCCESS;
}

VkResult VKAPI_CALL Layer_GetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pRectCount, VkRect2D* pRects)
{
	return VKLayerSurface::FromHandle(surface)->GetPresentRectangles(physicalDevice, pRectCount, pRects);
}


//#pragma mark - Swapchain

VkResult Layer_CreateSwapchainKHR(VkDevice device,
        const VkSwapchainCreateInfoKHR *createInfo,
        const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain)
{
	(void)allocator;
	auto wineSwapchain = new(std::nothrow) VKLayerSwapchain();
	if (wineSwapchain == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
	VkCheckRet(wineSwapchain->Init(LayerDevice::FromHandle(device), *createInfo));
	*swapchain = wineSwapchain->ToHandle();
	return VK_SUCCESS;
}

void Layer_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator)
{
	(void)device;
	(void)allocator;
	delete VKLayerSwapchain::FromHandle(swapchain);
}

VkResult Layer_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images)
{
	(void)device;
	return VKLayerSwapchain::FromHandle(swapchain)->GetSwapchainImages(count, images);
}

VkResult Layer_AcquireNextImageKHR(
	VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex
) {
	(void)device;
	VkAcquireNextImageInfoKHR info{
		.sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
		.swapchain = swapchain,
		.timeout = timeout,
		.semaphore = semaphore,
		.fence = fence
	};
	return VKLayerSwapchain::FromHandle(swapchain)->AcquireNextImage(&info, pImageIndex);
}

VkResult Layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
	VkResult ret = VK_SUCCESS;
	for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
		auto *sc = VKLayerSwapchain::FromHandle(pPresentInfo->pSwapchains[i]);
		VkResult res = sc->QueuePresent(queue, pPresentInfo, i);

		if (pPresentInfo->pResults != nullptr)
			pPresentInfo->pResults[i] = res;

		if (res != VK_SUCCESS && ret == VK_SUCCESS)
			ret = res;
	}

	return ret;
}
