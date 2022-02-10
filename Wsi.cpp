#include "Wsi.h"

#include <OS.h>

#include <private/shared/AutoDeleter.h>
#include <private/shared/AutoDeleterOS.h>
#include <private/shared/PthreadMutexLocker.h>

#include <stdio.h>
#include <string.h>
#include <new>
#include <algorithm>
#include <cassert>

#include <Bitmap.h>


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


//#pragma mark - BufferQueue

class BufferQueue {
private:
	ArrayDeleter<int32> fItems;
	int32 fBeg, fLen, fMaxLen;

public:
	BufferQueue(int32 maxLen = 0);
	bool SetMaxLen(int32 maxLen);

	inline int32 Length() {return fLen;}
	bool Add(int32 val);
	int32 Remove();
	int32 Begin();
};

BufferQueue::BufferQueue(int32 maxLen):
	fItems((maxLen > 0) ? new int32[maxLen] : NULL),
	fBeg(0), fLen(0), fMaxLen(maxLen)
{}

bool BufferQueue::SetMaxLen(int32 maxLen)
{
	if (!(maxLen > 0)) {
		fItems.Unset();
	} else {
		auto newItems = new(std::nothrow) int32[maxLen];
		if (newItems == NULL)
			return false;
		fItems.SetTo(newItems);
	}
	fMaxLen = maxLen;
	fBeg = 0; fLen = 0; fMaxLen = maxLen;
	return true;
}


bool BufferQueue::Add(int32 val)
{
	if (!(fLen < fMaxLen))
		return false;
	fItems[(fBeg + fLen)%fMaxLen] = val;
	fLen++;
	return true;
}

int32 BufferQueue::Remove()
{
	if (!(fLen > 0))
		return -1;
	int32 res = fItems[fBeg%fMaxLen];
	fBeg = (fBeg + 1)%fMaxLen;
	fLen--;
	return res;
}

int32 BufferQueue::Begin()
{
	if (!(fLen > 0))
		return -1;
	return fItems[fBeg%fMaxLen];
}


//#pragma mark -

class VKLayerImage {
private:
	LayerDevice *fDevice;
	VkImage fImage;
	VkDeviceMemory fMemory;

public:
	VKLayerImage();
	~VKLayerImage();
	VkResult Init(LayerDevice *device, const VkImageCreateInfo &createInfo, bool cpuMem = false, area_id *area = NULL);

	VkImage ToHandle() {return fImage;}
	VkDeviceMemory GetMemoryHandle() {return fMemory;}
};

class BitmapHook {
public:
	virtual ~BitmapHook() {};
	virtual void GetSize(uint32_t &width, uint32_t &height) = 0;
	virtual BBitmap *SetBitmap(BBitmap *bmp) = 0;
};

class VKLayerSurfaceBase {
public:
	virtual ~VKLayerSurfaceBase() {};
	virtual void SetBitmapHook(BitmapHook *hook) = 0;
};

class VKLayerSurface: public VKLayerSurfaceBase {
private:
	LayerInstance *fInstance;
	BitmapHook *fBitmapHook;

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

	BitmapHook *GetBitmapHook() {return fBitmapHook;}
	void SetBitmapHook(BitmapHook *hook) override;
};

class VKLayerSwapchain {
private:
	pthread_mutex_t fLock;
	LayerDevice *fDevice;
	VKLayerSurface *fSurface;
	VkExtent2D fImageExtent;
	uint32 fImageCnt;
	ArrayDeleter<VKLayerImage> fImages;
	BufferQueue fImagePool;
	ObjectDeleter<VKLayerImage> fBuffer;
	VkCommandPool fCommandPool;
	VkQueue fQueue;
	VkFence fFence;

	ObjectDeleter<BBitmap> fBitmap;
	AreaDeleter fBitmapArea;
	BBitmap *fCurBitmap;

	VkImageCreateInfo ImageFromCreateInfo(const VkSwapchainCreateInfoKHR &createInfo);
	VkResult CreateBuffer();
	VkResult CopyToBuffer(VkImage srcImage, int32_t width, int32_t height);
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

	VkMemoryAllocateInfo memAllocInfo{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
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


//#pragma mark - VKLayerSurface

VKLayerSurface::VKLayerSurface(): fInstance(NULL), fBitmapHook(NULL)
{}

VKLayerSurface::~VKLayerSurface()
{
}

VkResult VKLayerSurface::Init(LayerInstance *instance, const VkHeadlessSurfaceCreateInfoEXT &createInfo)
{
	(void)createInfo;
	fInstance = instance;
	return VK_SUCCESS;
}

VkResult VKLayerSurface::GetCapabilities(VkPhysicalDevice physDev, VkSurfaceCapabilitiesKHR *surfaceCapabilities)
{
	/* Image count limits */
	surfaceCapabilities->minImageCount = 1;
	surfaceCapabilities->maxImageCount = 3;

	/* Surface extents */
	if (fBitmapHook != NULL)
		fBitmapHook->GetSize(surfaceCapabilities->currentExtent.width, surfaceCapabilities->currentExtent.height);
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
	fBitmapHook = hook;
}


//#pragma mark - VKLayerSwapchain

VKLayerSwapchain::VKLayerSwapchain():
	fLock(PTHREAD_RECURSIVE_MUTEX_INITIALIZER),
	fCommandPool(VK_NULL_HANDLE),
	fFence(VK_NULL_HANDLE)
{
}

VKLayerSwapchain::~VKLayerSwapchain()
{
	if (fCommandPool != VK_NULL_HANDLE) {
		fDevice->Hooks().DestroyCommandPool(fDevice->ToHandle(), fCommandPool, nullptr);
		fDevice->Hooks().QueueWaitIdle(fQueue);
	}

	fDevice->Hooks().DestroyFence(fDevice->ToHandle(), fFence, NULL);
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
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = createInfo.imageUsage,
		.sharingMode = createInfo.imageSharingMode,
		.queueFamilyIndexCount = createInfo.queueFamilyIndexCount,
		.pQueueFamilyIndices = createInfo.pQueueFamilyIndices,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
	};
}

VkResult VKLayerSwapchain::CreateBuffer()
{
	VkCommandPoolCreateInfo cmdPoolInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = 0
	};
	VkCheckRet(fDevice->Hooks().CreateCommandPool(fDevice->ToHandle(), &cmdPoolInfo, nullptr, &fCommandPool));

	VkImageCreateInfo createInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_B8G8R8A8_UNORM,
		.extent = {
			.width = fImageExtent.width,
			.height = fImageExtent.height,
			.depth = 1
		},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_LINEAR,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
	};
	fBuffer.SetTo(new(std::nothrow) VKLayerImage());
	if (!fBuffer.IsSet())
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	area_id area;
	VkCheckRet(fBuffer->Init(fDevice, createInfo, true, &area));
	fBitmapArea.SetTo(area);

	VkImageSubresource subResource{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
	VkSubresourceLayout subResourceLayout;
	fDevice->Hooks().GetImageSubresourceLayout(fDevice->ToHandle(), fBuffer->ToHandle(), &subResource, &subResourceLayout);
	fBitmap.SetTo(new(std::nothrow) BBitmap(fBitmapArea.Get(), 0, BRect(0, 0, fImageExtent.width - 1, fImageExtent.height - 1), B_BITMAP_IS_AREA, B_RGB32, subResourceLayout.rowPitch));
	if (!fBitmap.IsSet())
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	fCurBitmap = fBitmap.Get();

	return VK_SUCCESS;
}

VkResult VKLayerSwapchain::CopyToBuffer(VkImage srcImage, int32_t width, int32_t height)
{
	// Do the actual blit from the offscreen image to our host visible destination image
	VkCommandBufferAllocateInfo cmdBufAllocateInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = fCommandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1
	};
	VkCommandBuffer copyCmd;
	VkCheckRet(fDevice->Hooks().AllocateCommandBuffers(fDevice->ToHandle(), &cmdBufAllocateInfo, &copyCmd));
	VkCommandBufferBeginInfo cmdBufInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	VkCheckRet(fDevice->Hooks().BeginCommandBuffer(copyCmd, &cmdBufInfo));

	// Transition destination image to transfer destination layout
	insertImageMemoryBarrier(
		fDevice,
		copyCmd,
		fBuffer->ToHandle(),
		0,
		VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
	);

	VkOffset3D blitSize{.x = width, .y = height, .z = 1};
	VkImageBlit imageBlitRegion{
		.srcSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1
		},
		.srcOffsets = {{}, blitSize},
		.dstSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1,
		},
		.dstOffsets = {{}, blitSize}
	};

	// Issue the blit command
	fDevice->Hooks().CmdBlitImage(
		copyCmd,
		srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		fBuffer->ToHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1,
		&imageBlitRegion,
		VK_FILTER_NEAREST
	);

	// Transition destination image to general layout, which is the required layout for mapping the image memory later on
	insertImageMemoryBarrier(
		fDevice,
		copyCmd,
		fBuffer->ToHandle(),
		VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_MEMORY_READ_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
	);

	VkCheckRet(fDevice->Hooks().EndCommandBuffer(copyCmd));

	VkCheckRet(submitWork(fDevice, copyCmd, fQueue));
	fDevice->Hooks().FreeCommandBuffers(fDevice->ToHandle(), fCommandPool, 1, &copyCmd);

	return VK_SUCCESS;
}

VkResult VKLayerSwapchain::CheckSuboptimal()
{
	auto bitmapHook = fSurface->GetBitmapHook();
	if (bitmapHook == NULL)
		return VK_SUCCESS;
	uint32_t width, height;
	bitmapHook->GetSize(width, height);

	if (!(fImageExtent.width == width && fImageExtent.height == height))
		return VK_SUBOPTIMAL_KHR;

	return VK_SUCCESS;
}

VkResult VKLayerSwapchain::Init(LayerDevice *device, const VkSwapchainCreateInfoKHR &createInfo)
{
	fDevice = device;
	fSurface = VKLayerSurface::FromHandle(createInfo.surface);

	VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
	VkCheckRet(fDevice->Hooks().CreateFence(fDevice->ToHandle(), &fence_info, NULL, &fFence));

	fImageExtent.width = createInfo.imageExtent.width;
	fImageExtent.height = createInfo.imageExtent.height;

	VkImageCreateInfo imageCreateInfo = ImageFromCreateInfo(createInfo);

	fImageCnt = createInfo.minImageCount;
	fImages.SetTo(new(std::nothrow) VKLayerImage[fImageCnt]);
	if (!fImages.IsSet())
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	if(!fImagePool.SetMaxLen(fImageCnt))
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	for (uint32_t i = 0; i < fImageCnt; i++) {
		VkCheckRet(fImages[i].Init(device, imageCreateInfo));
		fImagePool.Add(i);
	}

	fDevice->Hooks().GetDeviceQueue(fDevice->ToHandle(), 0, 0, &fQueue);
	//VkCheckRet(vkSetDeviceLoaderData(device, fQueue));

	VkCheckRet(CreateBuffer());

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
	int32 imageIdx;
	for(;;) {
		{
			PthreadMutexLocker lock(&fLock);
			imageIdx = fImagePool.Remove();
		}
		if (imageIdx < 0) {
			snooze(100);
			continue;
		}
		break;
	};
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

	PthreadMutexLocker lock(&fLock);
	uint32_t imageIdx = presentInfo->pImageIndices[idx];
	fImagePool.Add(imageIdx);

	auto bitmapHook = fSurface->GetBitmapHook();
	if (bitmapHook != NULL) {
		CopyToBuffer(fImages[imageIdx].ToHandle(), fImageExtent.width, fImageExtent.height);
		if (fBitmap.IsSet()) {
			delete bitmapHook->SetBitmap(fBitmap.Detach());
		} else {
			bitmapHook->SetBitmap(fCurBitmap);
		}
	}

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
