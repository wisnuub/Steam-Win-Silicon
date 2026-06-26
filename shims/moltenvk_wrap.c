/*
 * MoltenVK wrapper v10 for Aether/Steam/CEF on Apple Silicon.
 *
 * Fixes:
 * 1. Hides VK_EXT/KHR_buffer_device_address so ANGLE's VMA never uses BDA mode.
 * 2. Fixes memoryTypeBits=0 from vkGetImageMemoryRequirements[2] (VMA -8 errors).
 * 3. Shadow buffer content preservation: CAMetalLayer drawables rotate and come
 *    back with undefined content. Chrome's compositor assumes the previous frame
 *    is still in the swapchain image for partial redraws (damage tracking). Fix:
 *    copy the presented image to a per-swapchain shadow VkImage before each present,
 *    then copy the shadow back into the newly acquired image before returning to ANGLE.
 *    v9: supports up to MAX_SLOTS swapchains simultaneously (fixes dropdown/popup
 *    swapchains clobbering the main window's shadow state in v8).
 */
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* ---- existing v7 function pointers ---- */
static PFN_vkAllocateMemory                      real_allocMem;
static PFN_vkGetInstanceProcAddr                 real_gpa;
static PFN_vkGetDeviceProcAddr                   real_gdpa;
static PFN_vkEnumerateDeviceExtensionProperties  real_enumDevExts;
static PFN_vkGetPhysicalDeviceFeatures2          real_getFeatures2;
static PFN_vkBindImageMemory                     real_bindImage;
static PFN_vkBindImageMemory2                    real_bindImage2;
static PFN_vkGetImageMemoryRequirements          real_getImgMemReqs;
static PFN_vkGetImageMemoryRequirements2         real_getImgMemReqs2;

/* ---- v9: shadow buffer function pointers ---- */
static PFN_vkGetDeviceQueue         real_getDeviceQueue;
static PFN_vkCreateSwapchainKHR     real_createSwapchain;
static PFN_vkGetSwapchainImagesKHR  real_getSwapImages;
static PFN_vkDestroySwapchainKHR    real_destroySwapchain;
static PFN_vkAcquireNextImageKHR    real_acquireNext;
static PFN_vkQueuePresentKHR        real_queuePresent;
static PFN_vkQueueSubmit            real_queueSubmit;
static PFN_vkQueueWaitIdle          real_queueWaitIdle;
static PFN_vkCreateCommandPool      real_createCmdPool;
static PFN_vkDestroyCommandPool     real_destroyCmdPool;
static PFN_vkAllocateCommandBuffers real_allocCmdBufs;
static PFN_vkBeginCommandBuffer     real_beginCmd;
static PFN_vkEndCommandBuffer       real_endCmd;
static PFN_vkResetCommandBuffer     real_resetCmd;
static PFN_vkCmdCopyImage           real_cmdCopyImage;
static PFN_vkCmdPipelineBarrier     real_cmdBarrier;
static PFN_vkCreateImage            real_createImage;
static PFN_vkDestroyImage           real_destroyImage;
static PFN_vkFreeMemory             real_freeMemory;

/* ---- per-device queue (one device assumed in Steam) ---- */
static VkDevice  g_device       = VK_NULL_HANDLE;
static VkQueue   g_queue        = VK_NULL_HANDLE;
static uint32_t  g_queueFamily  = UINT32_MAX;

/* ---- per-swapchain shadow state ---- */
#define MAX_SLOTS 32
#define MAX_IMAGES 8

typedef struct {
    VkSwapchainKHR  swapchain;
    VkFormat        format;
    uint32_t        width, height;
    VkImage         images[MAX_IMAGES];
    uint32_t        imageCount;
    VkImage         shadow;
    VkDeviceMemory  shadowMem;
    VkCommandPool   pool;
    VkCommandBuffer cmd;
    int             shadowReady;
    int             valid;
} SwapSlot;

static SwapSlot g_slots[MAX_SLOTS];

static SwapSlot *slot_find(VkSwapchainKHR sw) {
    for (int i = 0; i < MAX_SLOTS; i++)
        if (g_slots[i].valid && g_slots[i].swapchain == sw) return &g_slots[i];
    return NULL;
}

static SwapSlot *slot_alloc(VkSwapchainKHR sw) {
    /* Prefer an existing slot for this swapchain */
    SwapSlot *s = slot_find(sw);
    if (s) return s;
    /* Find a free slot */
    for (int i = 0; i < MAX_SLOTS; i++)
        if (!g_slots[i].valid) { memset(&g_slots[i], 0, sizeof(g_slots[i])); return &g_slots[i]; }
    /* All slots full: evict slot 0 (oldest) */
    return &g_slots[0];
}

/* ================================================================
 * Shadow infrastructure helpers (per-slot)
 * ================================================================ */

static void slot_destroy_shadow(SwapSlot *s, VkDevice dev) {
    if (s->pool   != VK_NULL_HANDLE && real_destroyCmdPool)
        real_destroyCmdPool(dev, s->pool, NULL);
    if (s->shadow != VK_NULL_HANDLE && real_destroyImage)
        real_destroyImage(dev, s->shadow, NULL);
    if (s->shadowMem != VK_NULL_HANDLE && real_freeMemory)
        real_freeMemory(dev, s->shadowMem, NULL);
    s->pool       = VK_NULL_HANDLE;
    s->cmd        = VK_NULL_HANDLE;
    s->shadow     = VK_NULL_HANDLE;
    s->shadowMem  = VK_NULL_HANDLE;
    s->shadowReady = 0;
}

static void slot_setup_shadow(SwapSlot *s, VkDevice dev) {
    if (s->shadow != VK_NULL_HANDLE) return;
    if (!real_createImage || !real_allocMem || !real_bindImage) return;
    if (!real_createCmdPool || !real_allocCmdBufs) return;
    if (g_queueFamily == UINT32_MAX) return;
    /* Skip trivial surfaces (1x1 etc.) */
    if (s->width < 4 || s->height < 4) return;

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = s->format;
    ici.extent      = (VkExtent3D){ s->width, s->height, 1 };
    ici.mipLevels   = 1;
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (real_createImage(dev, &ici, NULL, &s->shadow) != VK_SUCCESS) {
        s->shadow = VK_NULL_HANDLE; return;
    }

    VkMemoryRequirements reqs = {0};
    real_getImgMemReqs(dev, s->shadow, &reqs);
    uint32_t mt = 0;
    for (uint32_t i = 0; i < 32; i++) {
        if (reqs.memoryTypeBits & (1u << i)) { mt = i; break; }
    }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = reqs.size;
    mai.memoryTypeIndex = mt;
    if (real_allocMem(dev, &mai, NULL, &s->shadowMem) != VK_SUCCESS) {
        real_destroyImage(dev, s->shadow, NULL);
        s->shadow = VK_NULL_HANDLE; return;
    }
    real_bindImage(dev, s->shadow, s->shadowMem, 0);

    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = g_queueFamily;
    if (real_createCmdPool(dev, &cpci, NULL, &s->pool) != VK_SUCCESS) return;

    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = s->pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    real_allocCmdBufs(dev, &cbai, &s->cmd);

    FILE *log = fopen("/tmp/mvk_shadow.log", "a");
    if (log) {
        fprintf(log, "slot_setup pid=%d sw=%p fmt=%d %ux%u shadow=%p\n",
                (int)getpid(), (void*)s->swapchain, s->format,
                s->width, s->height, (void*)s->shadow);
        fclose(log);
    }
}

/* Record and submit a copy between two images.
 * src comes from PRESENT_SRC_KHR (after present) or PRESENT_SRC_KHR (shadow).
 * dst comes from PRESENT_SRC_KHR (previously presented) or UNDEFINED (first use).
 * We use GENERAL as the safe transitional layout since MoltenVK doesn't enforce
 * layout rules strictly, and GENERAL is always valid for transfers. */
static void do_copy(SwapSlot *s,
                    VkImage src, VkImageLayout srcOld,
                    VkImage dst, VkImageLayout dstOld) {
    if (!real_beginCmd || !real_endCmd || !real_cmdCopyImage ||
        !real_cmdBarrier || !real_queueSubmit || !real_queueWaitIdle) return;
    if (s->cmd == VK_NULL_HANDLE || g_queue == VK_NULL_HANDLE) return;

    real_resetCmd(s->cmd, 0);
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    real_beginCmd(s->cmd, &cbbi);

    VkImageMemoryBarrier b[2] = {0};
    b[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b[0].srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT;
    b[0].dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].oldLayout           = srcOld;
    b[0].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b[0].image               = src;
    b[0].subresourceRange    = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    b[1].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b[1].srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b[1].dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].oldLayout           = dstOld;
    b[1].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b[1].image               = dst;
    b[1].subresourceRange    = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    real_cmdBarrier(s->cmd,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 2, b);

    VkImageCopy region = {0};
    region.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = (VkExtent3D){ s->width, s->height, 1 };
    real_cmdCopyImage(s->cmd,
        src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &region);

    /* Restore layouts:
     * - src back to PRESENT_SRC_KHR (shadow stays in PRESENT_SRC_KHR, swapchain image
     *   is restored for ANGLE's benefit)
     * - dst to PRESENT_SRC_KHR (not UNDEFINED) so ANGLE's PRESENT_SRC_KHR→COLOR_ATTACHMENT
     *   barrier preserves our shadow content */
    VkImageMemoryBarrier b2[2] = {0};
    b2[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b2[0].srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    b2[0].dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b2[0].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b2[0].newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b2[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2[0].image               = src;
    b2[0].subresourceRange    = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    b2[1].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b2[1].srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2[1].dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b2[1].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    /* Leave in PRESENT_SRC_KHR: ANGLE will do PRESENT_SRC_KHR→COLOR_ATTACHMENT
     * which preserves content on MoltenVK (no content discard on layout change). */
    b2[1].newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b2[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2[1].image               = dst;
    b2[1].subresourceRange    = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    real_cmdBarrier(s->cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, NULL, 0, NULL, 2, b2);

    real_endCmd(s->cmd);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &s->cmd;
    real_queueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    real_queueWaitIdle(g_queue);
}

/* ================================================================
 * Constructor: load real function pointers
 * ================================================================ */
__attribute__((constructor))
static void mvk_wrap_init(void) {
    void *real = NULL;
    char abs_path[4096] = {0};

    Dl_info info = {0};
    if (dladdr((void *)mvk_wrap_init, &info) && info.dli_fname) {
        const char *slash = strrchr(info.dli_fname, '/');
        if (slash) {
            snprintf(abs_path, sizeof(abs_path), "%.*slibMoltenVK_real.dylib",
                     (int)(slash - info.dli_fname + 1), info.dli_fname);
            real = dlopen(abs_path, RTLD_NOLOAD | RTLD_LAZY);
            if (!real) real = dlopen(abs_path, RTLD_LAZY | RTLD_LOCAL);
        }
    }
    if (!real) real = dlopen("libMoltenVK_real.dylib", RTLD_NOLOAD | RTLD_LAZY);
    if (!real) real = dlopen("libMoltenVK_real.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (!real) return;

#define SYM(var, name) var = (PFN_vk##name)dlsym(real, "vk" #name)
    SYM(real_gpa,           GetInstanceProcAddr);
    SYM(real_gdpa,          GetDeviceProcAddr);
    SYM(real_allocMem,      AllocateMemory);
    SYM(real_enumDevExts,   EnumerateDeviceExtensionProperties);
    SYM(real_getFeatures2,  GetPhysicalDeviceFeatures2);
    SYM(real_bindImage,     BindImageMemory);
    SYM(real_bindImage2,    BindImageMemory2);
    SYM(real_getImgMemReqs,  GetImageMemoryRequirements);
    SYM(real_getImgMemReqs2, GetImageMemoryRequirements2);
    SYM(real_getDeviceQueue,  GetDeviceQueue);
    SYM(real_createSwapchain, CreateSwapchainKHR);
    SYM(real_getSwapImages,   GetSwapchainImagesKHR);
    SYM(real_destroySwapchain,DestroySwapchainKHR);
    SYM(real_acquireNext,     AcquireNextImageKHR);
    SYM(real_queuePresent,    QueuePresentKHR);
    SYM(real_queueSubmit,     QueueSubmit);
    SYM(real_queueWaitIdle,   QueueWaitIdle);
    SYM(real_createCmdPool,   CreateCommandPool);
    SYM(real_destroyCmdPool,  DestroyCommandPool);
    SYM(real_allocCmdBufs,    AllocateCommandBuffers);
    SYM(real_beginCmd,        BeginCommandBuffer);
    SYM(real_endCmd,          EndCommandBuffer);
    SYM(real_resetCmd,        ResetCommandBuffer);
    SYM(real_cmdCopyImage,    CmdCopyImage);
    SYM(real_cmdBarrier,      CmdPipelineBarrier);
    SYM(real_createImage,     CreateImage);
    SYM(real_destroyImage,    DestroyImage);
    SYM(real_freeMemory,      FreeMemory);
#undef SYM

    char dbgpath[64];
    snprintf(dbgpath, sizeof(dbgpath), "/tmp/mvk_wrap_%d.txt", (int)getpid());
    FILE *dbg = fopen(dbgpath, "w");
    if (dbg) {
        fprintf(dbg, "mvk_wrap v10 pid=%d path=%s real=%p\n"
                     "gpa=%p createSwap=%p acquireNext=%p queuePresent=%p\n",
                (int)getpid(), abs_path, real,
                real_gpa, real_createSwapchain, real_acquireNext, real_queuePresent);
        fclose(dbg);
    }
}

/* ================================================================
 * Existing v7 intercepts (BDA + memoryTypeBits fixes)
 * ================================================================ */

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
        VkPhysicalDevice physDev, const char *pLayerName,
        uint32_t *pPropertyCount, VkExtensionProperties *pProperties) {
    if (!real_enumDevExts) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real_enumDevExts(physDev, pLayerName, pPropertyCount, pProperties);
    if (r == VK_SUCCESS && pProperties) {
        uint32_t src = 0, dst = 0;
        for (; src < *pPropertyCount; src++) {
            if (strcmp(pProperties[src].extensionName, "VK_EXT_buffer_device_address") == 0) continue;
            if (strcmp(pProperties[src].extensionName, "VK_KHR_buffer_device_address") == 0) continue;
            /* MoltenVK exposes this but it fails for ANGLE's implicit MSAA images (-8 errors),
             * causing the Store page content layer to render black. Hide it so ANGLE uses
             * its traditional MSAA fallback path instead. */
            if (strcmp(pProperties[src].extensionName, "VK_EXT_multisampled_render_to_single_sampled") == 0) continue;
            if (src != dst) pProperties[dst] = pProperties[src];
            dst++;
        }
        *pPropertyCount = dst;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
        VkPhysicalDevice physDev, VkPhysicalDeviceFeatures2 *pFeatures) {
    if (!real_getFeatures2) return;
    real_getFeatures2(physDev, pFeatures);
    VkBaseOutStructure *s = (VkBaseOutStructure *)pFeatures->pNext;
    while (s) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT) {
            VkPhysicalDeviceBufferDeviceAddressFeaturesEXT *f =
                (VkPhysicalDeviceBufferDeviceAddressFeaturesEXT *)s;
            f->bufferDeviceAddress = f->bufferDeviceAddressCaptureReplay =
            f->bufferDeviceAddressMultiDevice = VK_FALSE;
        }
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            VkPhysicalDeviceVulkan12Features *f = (VkPhysicalDeviceVulkan12Features *)s;
            f->bufferDeviceAddress = f->bufferDeviceAddressCaptureReplay =
            f->bufferDeviceAddressMultiDevice = VK_FALSE;
        }
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) {
            VkPhysicalDeviceBufferDeviceAddressFeatures *f =
                (VkPhysicalDeviceBufferDeviceAddressFeatures *)s;
            f->bufferDeviceAddress = f->bufferDeviceAddressCaptureReplay =
            f->bufferDeviceAddressMultiDevice = VK_FALSE;
        }
        s = s->pNext;
    }
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(
        VkDevice device, VkImage image, VkMemoryRequirements *pMemReqs) {
    if (!real_getImgMemReqs) return;
    real_getImgMemReqs(device, image, pMemReqs);
    if (pMemReqs && pMemReqs->memoryTypeBits == 0) {
        pMemReqs->memoryTypeBits = UINT32_MAX;
        FILE *log = fopen("/tmp/vkalloc_fail.log", "a");
        if (log) { fprintf(log, "pid=%d getImgMemReqs memoryTypeBits=0 fixed\n", (int)getpid()); fclose(log); }
    }
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements2(
        VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
        VkMemoryRequirements2 *pMemReqs2) {
    if (!real_getImgMemReqs2) return;
    real_getImgMemReqs2(device, pInfo, pMemReqs2);
    if (pMemReqs2 && pMemReqs2->memoryRequirements.memoryTypeBits == 0) {
        pMemReqs2->memoryRequirements.memoryTypeBits = UINT32_MAX;
        FILE *log = fopen("/tmp/vkalloc_fail.log", "a");
        if (log) { fprintf(log, "pid=%d getImgMemReqs2 memoryTypeBits=0 fixed\n", (int)getpid()); fclose(log); }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(
        VkDevice device, const VkMemoryAllocateInfo *pInfo,
        const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMem) {
    if (!real_allocMem) return VK_ERROR_INITIALIZATION_FAILED;
    VkMemoryAllocateFlagsInfo *flagsNode = NULL;
    const VkBaseInStructure *s = (const VkBaseInStructure *)pInfo->pNext;
    while (s) {
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) {
            VkMemoryAllocateFlagsInfo *fi = (VkMemoryAllocateFlagsInfo *)(uintptr_t)s;
            if (fi->flags & (VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT |
                             VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT))
                flagsNode = fi;
        }
        s = (const VkBaseInStructure *)s->pNext;
    }
    if (!flagsNode) return real_allocMem(device, pInfo, pAllocator, pMem);

    VkMemoryAllocateFlagsInfo localFlags = *flagsNode;
    localFlags.flags &= ~(VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT |
                          VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT);
    VkMemoryAllocateInfo localInfo = *pInfo;
    if ((const VkBaseInStructure *)pInfo->pNext == (const VkBaseInStructure *)flagsNode) {
        localInfo.pNext = &localFlags;
    } else {
        VkBaseOutStructure *cur = (VkBaseOutStructure *)localInfo.pNext;
        while (cur) {
            if (cur->pNext == (VkBaseOutStructure *)flagsNode) {
                cur->pNext = (VkBaseOutStructure *)&localFlags; break;
            }
            cur = cur->pNext;
        }
    }
    return real_allocMem(device, &localInfo, pAllocator, pMem);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(
        VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset) {
    if (!real_bindImage) return VK_ERROR_INITIALIZATION_FAILED;
    return real_bindImage(device, image, memory, offset);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2(
        VkDevice device, uint32_t bindInfoCount, const VkBindImageMemoryInfo *pBindInfos) {
    if (!real_bindImage2) return VK_ERROR_INITIALIZATION_FAILED;
    return real_bindImage2(device, bindInfoCount, pBindInfos);
}

/* ================================================================
 * v9: shadow buffer intercepts (multi-swapchain)
 * ================================================================ */

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(
        VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue) {
    if (real_getDeviceQueue) real_getDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
    if (pQueue && *pQueue != VK_NULL_HANDLE) {
        g_device      = device;
        g_queue       = *pQueue;
        g_queueFamily = queueFamilyIndex;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
        VkDevice device, const VkSwapchainCreateInfoKHR *pCI,
        const VkAllocationCallbacks *pAlloc, VkSwapchainKHR *pSwap) {
    if (!real_createSwapchain) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult r = real_createSwapchain(device, pCI, pAlloc, pSwap);
    if (r == VK_SUCCESS) {
        SwapSlot *s = slot_alloc(*pSwap);
        /* Destroy old shadow if we're reusing a slot */
        if (s->valid) slot_destroy_shadow(s, device);
        memset(s, 0, sizeof(*s));
        s->swapchain = *pSwap;
        s->format    = pCI->imageFormat;
        s->width     = pCI->imageExtent.width;
        s->height    = pCI->imageExtent.height;
        s->valid     = 1;
        g_device     = device;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
        VkDevice device, VkSwapchainKHR swapchain,
        uint32_t *pCount, VkImage *pImages) {
    if (!real_getSwapImages) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult r = real_getSwapImages(device, swapchain, pCount, pImages);
    if (r == VK_SUCCESS && pImages) {
        SwapSlot *s = slot_find(swapchain);
        if (s) {
            uint32_t n = *pCount < MAX_IMAGES ? *pCount : MAX_IMAGES;
            s->imageCount = n;
            for (uint32_t i = 0; i < n; i++) s->images[i] = pImages[i];
        }
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(
        VkDevice device, VkSwapchainKHR swapchain,
        const VkAllocationCallbacks *pAlloc) {
    SwapSlot *s = slot_find(swapchain);
    if (s) {
        slot_destroy_shadow(s, device);
        s->valid = 0;
    }
    if (real_destroySwapchain) real_destroySwapchain(device, swapchain, pAlloc);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(
        VkQueue queue, const VkPresentInfoKHR *pPI) {
    if (!real_queuePresent) return VK_ERROR_DEVICE_LOST;

    for (uint32_t sc = 0; sc < pPI->swapchainCount; sc++) {
        SwapSlot *s = slot_find(pPI->pSwapchains[sc]);
        if (!s || !s->valid) continue;

        /* Lazily create the shadow on first present */
        if (s->shadow == VK_NULL_HANDLE) slot_setup_shadow(s, g_device);
        if (s->shadow == VK_NULL_HANDLE || s->cmd == VK_NULL_HANDLE) continue;

        uint32_t idx = pPI->pImageIndices[sc];
        if (idx >= s->imageCount || s->images[idx] == VK_NULL_HANDLE) continue;

        /* Wait for ANGLE's rendering to finish, then copy presented image → shadow */
        real_queueWaitIdle(queue);
        do_copy(s,
                s->images[idx], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                s->shadow,      VK_IMAGE_LAYOUT_UNDEFINED);
        s->shadowReady = 1;
    }

    return real_queuePresent(queue, pPI);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
        VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
        VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex) {
    if (!real_acquireNext) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult r = real_acquireNext(device, swapchain, timeout, semaphore, fence, pImageIndex);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return r;

    SwapSlot *s = slot_find(swapchain);
    if (!s || !s->valid || !s->shadowReady) return r;
    if (s->shadow == VK_NULL_HANDLE || s->cmd == VK_NULL_HANDLE) return r;
    if (g_queue == VK_NULL_HANDLE) return r;

    uint32_t idx = *pImageIndex;
    if (idx >= s->imageCount || s->images[idx] == VK_NULL_HANDLE) return r;

    /* Copy shadow → acquired image.
     * Use PRESENT_SRC_KHR as old layout (what ANGLE expects after a prior present).
     * After this copy the image is left in PRESENT_SRC_KHR.
     * ANGLE's PRESENT_SRC_KHR→COLOR_ATTACHMENT barrier preserves content on MoltenVK. */
    do_copy(s,
            s->shadow,       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            s->images[idx],  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    return r;
}

/* ================================================================
 * Proc address dispatch
 * ================================================================ */

#define INTERCEPT(name) if (strcmp(pName, #name) == 0) return (PFN_vkVoidFunction)name;

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
        VkInstance instance, const char *pName) {
    if (pName) {
        INTERCEPT(vkAllocateMemory)
        INTERCEPT(vkEnumerateDeviceExtensionProperties)
        INTERCEPT(vkGetPhysicalDeviceFeatures2)
        INTERCEPT(vkBindImageMemory)
        INTERCEPT(vkBindImageMemory2)
        INTERCEPT(vkGetImageMemoryRequirements)
        if (strcmp(pName, "vkGetImageMemoryRequirements2") == 0 ||
            strcmp(pName, "vkGetImageMemoryRequirements2KHR") == 0)
            return (PFN_vkVoidFunction)vkGetImageMemoryRequirements2;
        INTERCEPT(vkGetDeviceQueue)
        INTERCEPT(vkCreateSwapchainKHR)
        INTERCEPT(vkGetSwapchainImagesKHR)
        INTERCEPT(vkDestroySwapchainKHR)
        INTERCEPT(vkAcquireNextImageKHR)
        INTERCEPT(vkQueuePresentKHR)
        INTERCEPT(vkGetInstanceProcAddr)
        INTERCEPT(vkGetDeviceProcAddr)
    }
    if (!real_gpa) return NULL;
    return real_gpa(instance, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
        VkDevice device, const char *pName) {
    if (pName) {
        INTERCEPT(vkAllocateMemory)
        INTERCEPT(vkEnumerateDeviceExtensionProperties)
        INTERCEPT(vkBindImageMemory)
        INTERCEPT(vkBindImageMemory2)
        INTERCEPT(vkGetImageMemoryRequirements)
        if (strcmp(pName, "vkGetImageMemoryRequirements2") == 0 ||
            strcmp(pName, "vkGetImageMemoryRequirements2KHR") == 0)
            return (PFN_vkVoidFunction)vkGetImageMemoryRequirements2;
        INTERCEPT(vkGetDeviceQueue)
        INTERCEPT(vkCreateSwapchainKHR)
        INTERCEPT(vkGetSwapchainImagesKHR)
        INTERCEPT(vkDestroySwapchainKHR)
        INTERCEPT(vkAcquireNextImageKHR)
        INTERCEPT(vkQueuePresentKHR)
    }
    if (!real_gdpa) return NULL;
    return real_gdpa(device, pName);
}
