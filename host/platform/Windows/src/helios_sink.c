/**
 * Looking Glass Helios output sink.
 *
 * This is a first integration bridge, not a long-term ABI. It reuses the
 * existing Helios gate hook:
 *
 *   HELIOS_GATE_RESID_FILE -> vn_renderer_helios appends "<res_id> <size>"
 *
 * The sink creates one exportable Venus-backed Vulkan image per captured output
 * size, maps it, copies captured BGRA pixels into the mapped image, performs a
 * zero-work submit so the Helios ICD flushes cached coherent mappings, and then
 * asks the Helios KMD to scan out the resource via SET_SCANOUT_BLOB.
 */

#include "helios_sink.h"

#include "common/debug.h"
#include "common/framebuffer.h"
#include "common/option.h"
#include "common/array.h"

#include <windows.h>
#include <setupapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static const GUID GUID_DEVINTERFACE_HELIOS = {
  0xC8F84237, 0xCD89, 0x48F5,
  { 0xAF, 0xC5, 0x32, 0x94, 0x45, 0x24, 0x62, 0x5C }
};

#define IOCTL_HELIOS_PRESENT_BLOB       0x0022E418u
#define HELIOS_ESCAPE_MAGIC             0x48454C53u
#define HELIOS_ESCAPE_VERSION           1u
#define HELIOS_ESCAPE_PRESENT_BLOB      0x0007u
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1u
#define DRM_FORMAT_MOD_LINEAR           0ULL

struct helios_escape_header
{
  uint32_t magic;
  uint32_t cmd_type;
  uint32_t version;
  uint32_t size;
};

struct helios_escape_present_blob
{
  struct helios_escape_header hdr;
  uint32_t resource_id;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t stride;
  uint32_t offset;
};

#define ILOAD(name) (PFN_##name)(void *)this.gipa(this.inst, #name)
#define DLOAD(name) (PFN_##name)(void *)this.gdpa(this.dev, #name)

struct HeliosSink
{
  bool enabled;
  bool initialized;
  bool imageReady;
  bool warnedFormat;

  char gateFile[MAX_PATH];
  char gpuMatch[128];

  HANDLE hdev;
  HMODULE vulkan;

  PFN_vkGetInstanceProcAddr gipa;
  PFN_vkGetDeviceProcAddr gdpa;

  VkInstance inst;
  VkPhysicalDevice phys;
  VkDevice dev;
  VkQueue queue;
  uint32_t qfi;

  PFN_vkCreateImage pCreateImage;
  PFN_vkDestroyImage pDestroyImage;
  PFN_vkGetImageMemoryRequirements pImgReq;
  PFN_vkAllocateMemory pAlloc;
  PFN_vkFreeMemory pFreeMem;
  PFN_vkBindImageMemory pBindImg;
  PFN_vkGetImageSubresourceLayout pSubLayout;
  PFN_vkMapMemory pMap;
  PFN_vkUnmapMemory pUnmap;
  PFN_vkFlushMappedMemoryRanges pFlush;
  PFN_vkQueueSubmit pSubmit;
  PFN_vkQueueWaitIdle pQueueWaitIdle;
  PFN_vkDeviceWaitIdle pDeviceWaitIdle;

  VkImage image;
  VkDeviceMemory mem;
  void * map;
  VkDeviceSize memSize;
  VkMemoryPropertyFlags memFlags;

  uint32_t resourceId;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t offset;
};

static struct HeliosSink this = { 0 };

static void helios_sink_destroy_image(void);

void helios_sink_initOptions(void)
{
  struct Option options[] =
  {
    {
      .module       = "helios",
      .name         = "enable",
      .description  = "Mirror captured frames to the Helios Vulkan/KMD scanout path",
      .type         = OPTION_TYPE_BOOL,
      .value.x_bool = false,
    },
    {
      .module         = "helios",
      .name           = "gateFile",
      .description    = "Resource-id gate file used by the Helios Venus ICD",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "C:\\Users\\Rupansh\\helios_lg_resid.txt",
    },
    {
      .module         = "helios",
      .name           = "gpu",
      .description    = "Substring used to pick the Venus physical device",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "Intel",
    },
    { 0 }
  };

  option_register(options);
}

static HANDLE helios_open_device(void)
{
  HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_HELIOS, NULL, NULL,
      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (di == INVALID_HANDLE_VALUE)
  {
    DEBUG_ERROR("Helios: SetupDiGetClassDevs failed: %lu", GetLastError());
    return INVALID_HANDLE_VALUE;
  }

  SP_DEVICE_INTERFACE_DATA ifd = { 0 };
  ifd.cbSize = sizeof(ifd);
  if (!SetupDiEnumDeviceInterfaces(di, NULL, &GUID_DEVINTERFACE_HELIOS, 0, &ifd))
  {
    DEBUG_ERROR("Helios: no device interface present: %lu", GetLastError());
    SetupDiDestroyDeviceInfoList(di);
    return INVALID_HANDLE_VALUE;
  }

  DWORD need = 0;
  SetupDiGetDeviceInterfaceDetailW(di, &ifd, NULL, 0, &need, NULL);
  SP_DEVICE_INTERFACE_DETAIL_DATA_W * detail = calloc(1, need);
  HANDLE h = INVALID_HANDLE_VALUE;
  if (detail)
  {
    detail->cbSize = sizeof(*detail);
    if (SetupDiGetDeviceInterfaceDetailW(di, &ifd, detail, need, NULL, NULL))
    {
      h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
      if (h == INVALID_HANDLE_VALUE)
        DEBUG_ERROR("Helios: CreateFile failed: %lu", GetLastError());
    }
    else
      DEBUG_ERROR("Helios: SetupDiGetDeviceInterfaceDetail failed: %lu",
          GetLastError());

    free(detail);
  }

  SetupDiDestroyDeviceInfoList(di);
  return h;
}

static uint32_t read_resid_for_size(const char * path, uint64_t wantSize)
{
  FILE * f = fopen(path, "r");
  if (!f)
    return 0;

  uint32_t res = 0;
  uint32_t rid;
  unsigned long long size;
  while (fscanf(f, "%u %llu", &rid, &size) == 2)
    if (size == wantSize)
      res = rid;

  fclose(f);
  return res;
}

static bool pick_memory_type(VkPhysicalDeviceMemoryProperties * props,
    uint32_t bits, VkMemoryPropertyFlags required, uint32_t * out)
{
  for (uint32_t i = 0; i < props->memoryTypeCount; ++i)
    if ((bits & (1u << i)) &&
        (props->memoryTypes[i].propertyFlags & required) == required)
    {
      *out = i;
      return true;
    }

  return false;
}

static bool helios_init_vulkan(void)
{
  this.vulkan = LoadLibraryW(L"vulkan-1.dll");
  if (!this.vulkan)
  {
    DEBUG_ERROR("Helios: failed to load vulkan-1.dll");
    return false;
  }

  this.gipa = (PFN_vkGetInstanceProcAddr)(void *)
    GetProcAddress(this.vulkan, "vkGetInstanceProcAddr");
  if (!this.gipa)
    return false;

  PFN_vkCreateInstance pCreateInstance =
    (PFN_vkCreateInstance)this.gipa(NULL, "vkCreateInstance");
  if (!pCreateInstance)
    return false;

  const VkApplicationInfo app =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "looking-glass-host-helios",
    .apiVersion = VK_API_VERSION_1_1,
  };
  const VkInstanceCreateInfo ici =
  {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &app,
  };
  if (pCreateInstance(&ici, NULL, &this.inst) != VK_SUCCESS)
  {
    DEBUG_ERROR("Helios: vkCreateInstance failed");
    return false;
  }

  PFN_vkEnumeratePhysicalDevices pEnum = ILOAD(vkEnumeratePhysicalDevices);
  PFN_vkGetPhysicalDeviceProperties pProps =
    ILOAD(vkGetPhysicalDeviceProperties);
  PFN_vkGetPhysicalDeviceQueueFamilyProperties pQFP =
    ILOAD(vkGetPhysicalDeviceQueueFamilyProperties);
  PFN_vkGetPhysicalDeviceMemoryProperties pMemProps =
    ILOAD(vkGetPhysicalDeviceMemoryProperties);
  PFN_vkEnumerateDeviceExtensionProperties pEnumExt =
    ILOAD(vkEnumerateDeviceExtensionProperties);
  PFN_vkCreateDevice pCreateDevice = ILOAD(vkCreateDevice);

  uint32_t count = 0;
  if (pEnum(this.inst, &count, NULL) != VK_SUCCESS || count == 0)
  {
    DEBUG_ERROR("Helios: no Vulkan physical devices");
    return false;
  }

  VkPhysicalDevice devs[16];
  if (count > 16)
    count = 16;
  if (pEnum(this.inst, &count, devs) != VK_SUCCESS)
    return false;

  this.phys = devs[0];
  for (uint32_t i = 0; i < count; ++i)
  {
    VkPhysicalDeviceProperties pp;
    pProps(devs[i], &pp);
    DEBUG_INFO("Helios: Vulkan device[%u]: %s", i, pp.deviceName);
    if (strstr(pp.deviceName, this.gpuMatch))
      this.phys = devs[i];
  }

  uint32_t qfCount = 0;
  pQFP(this.phys, &qfCount, NULL);
  VkQueueFamilyProperties qfs[16];
  if (qfCount > 16)
    qfCount = 16;
  pQFP(this.phys, &qfCount, qfs);
  this.qfi = 0;
  for (uint32_t i = 0; i < qfCount; ++i)
    if (qfs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
    {
      this.qfi = i;
      break;
    }

  uint32_t extCount = 0;
  pEnumExt(this.phys, NULL, &extCount, NULL);
  VkExtensionProperties * exts = calloc(extCount, sizeof(*exts));
  if (!exts)
    return false;
  pEnumExt(this.phys, NULL, &extCount, exts);

  const char * wanted[] =
  {
    "VK_KHR_external_memory",
    "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf",
    "VK_EXT_image_drm_format_modifier",
  };
  const char * enabled[ARRAY_LENGTH(wanted)];
  uint32_t enabledCount = 0;
  for (unsigned w = 0; w < ARRAY_LENGTH(wanted); ++w)
  {
    bool found = false;
    for (uint32_t i = 0; i < extCount; ++i)
      if (strcmp(exts[i].extensionName, wanted[w]) == 0)
      {
        found = true;
        break;
      }
    if (found)
      enabled[enabledCount++] = wanted[w];
    else
      DEBUG_WARN("Helios: Vulkan extension missing: %s", wanted[w]);
  }
  free(exts);

  const float prio = 1.0f;
  const VkDeviceQueueCreateInfo qci =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = this.qfi,
    .queueCount = 1,
    .pQueuePriorities = &prio,
  };
  const VkDeviceCreateInfo dci =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount = 1,
    .pQueueCreateInfos = &qci,
    .enabledExtensionCount = enabledCount,
    .ppEnabledExtensionNames = enabled,
  };
  if (pCreateDevice(this.phys, &dci, NULL, &this.dev) != VK_SUCCESS)
  {
    DEBUG_ERROR("Helios: vkCreateDevice failed");
    return false;
  }

  this.gdpa = ILOAD(vkGetDeviceProcAddr);
  PFN_vkGetDeviceQueue pGetQueue = DLOAD(vkGetDeviceQueue);
  pGetQueue(this.dev, this.qfi, 0, &this.queue);

  this.pCreateImage       = DLOAD(vkCreateImage);
  this.pDestroyImage      = DLOAD(vkDestroyImage);
  this.pImgReq            = DLOAD(vkGetImageMemoryRequirements);
  this.pAlloc             = DLOAD(vkAllocateMemory);
  this.pFreeMem           = DLOAD(vkFreeMemory);
  this.pBindImg           = DLOAD(vkBindImageMemory);
  this.pSubLayout         = DLOAD(vkGetImageSubresourceLayout);
  this.pMap               = DLOAD(vkMapMemory);
  this.pUnmap             = DLOAD(vkUnmapMemory);
  this.pFlush             = DLOAD(vkFlushMappedMemoryRanges);
  this.pSubmit            = DLOAD(vkQueueSubmit);
  this.pQueueWaitIdle     = DLOAD(vkQueueWaitIdle);
  this.pDeviceWaitIdle    = DLOAD(vkDeviceWaitIdle);

  VkPhysicalDeviceMemoryProperties mp;
  pMemProps(this.phys, &mp);
  (void)mp;

  return true;
}

bool helios_sink_init(void)
{
  this.enabled = option_get_bool("helios", "enable");
  if (!this.enabled)
    return true;

  const char * gate = option_get_string("helios", "gateFile");
  const char * gpu  = option_get_string("helios", "gpu");
  snprintf(this.gateFile, sizeof(this.gateFile), "%s", gate ? gate : "");
  snprintf(this.gpuMatch, sizeof(this.gpuMatch), "%s", gpu ? gpu : "");

  SetEnvironmentVariableA("HELIOS_GATE_RESID_FILE", this.gateFile);
  _putenv_s("HELIOS_GATE_RESID_FILE", this.gateFile);
  DeleteFileA(this.gateFile);

  this.hdev = helios_open_device();
  if (this.hdev == INVALID_HANDLE_VALUE)
    return false;

  if (!helios_init_vulkan())
  {
    DEBUG_ERROR("Helios: Vulkan sink initialization failed");
    return false;
  }

  this.initialized = true;
  DEBUG_INFO("Helios: enabled Vulkan output sink");
  return true;
}

static void helios_sink_destroy_image(void)
{
  if (!this.dev)
    return;

  if (this.pDeviceWaitIdle)
    this.pDeviceWaitIdle(this.dev);
  if (this.mem && this.map && this.pUnmap)
    this.pUnmap(this.dev, this.mem);
  this.map = NULL;
  if (this.image && this.pDestroyImage)
    this.pDestroyImage(this.dev, this.image, NULL);
  if (this.mem && this.pFreeMem)
    this.pFreeMem(this.dev, this.mem, NULL);

  this.image = VK_NULL_HANDLE;
  this.mem = VK_NULL_HANDLE;
  this.memSize = 0;
  this.resourceId = 0;
  this.imageReady = false;
}

void helios_sink_deinit(void)
{
  if (!this.enabled)
    return;

  helios_sink_destroy_image();

  if (this.dev)
  {
    PFN_vkDestroyDevice pDestroyDevice = DLOAD(vkDestroyDevice);
    if (pDestroyDevice)
      pDestroyDevice(this.dev, NULL);
  }
  if (this.inst)
  {
    PFN_vkDestroyInstance pDestroyInstance =
      (PFN_vkDestroyInstance)this.gipa(this.inst, "vkDestroyInstance");
    if (pDestroyInstance)
      pDestroyInstance(this.inst, NULL);
  }
  if (this.vulkan)
    FreeLibrary(this.vulkan);
  if (this.hdev && this.hdev != INVALID_HANDLE_VALUE)
    CloseHandle(this.hdev);

  memset(&this, 0, sizeof(this));
}

static bool helios_sink_create_image(uint32_t width, uint32_t height)
{
  helios_sink_destroy_image();
  DeleteFileA(this.gateFile);

  uint64_t mods[1] = { DRM_FORMAT_MOD_LINEAR };
  VkImageDrmFormatModifierListCreateInfoEXT modList =
  {
    .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
    .drmFormatModifierCount = 1,
    .pDrmFormatModifiers = mods,
  };
  VkExternalMemoryImageCreateInfo extImg =
  {
    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
    .pNext = &modList,
    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
  };
  VkImageCreateInfo ici =
  {
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .pNext = &extImg,
    .imageType = VK_IMAGE_TYPE_2D,
    .format = VK_FORMAT_B8G8R8A8_UNORM,
    .extent = { width, height, 1 },
    .mipLevels = 1,
    .arrayLayers = 1,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  if (this.pCreateImage(this.dev, &ici, NULL, &this.image) != VK_SUCCESS)
  {
    DEBUG_ERROR("Helios: vkCreateImage failed for %ux%u", width, height);
    return false;
  }

  VkMemoryRequirements req;
  this.pImgReq(this.dev, this.image, &req);

  PFN_vkGetPhysicalDeviceMemoryProperties pMemProps =
    ILOAD(vkGetPhysicalDeviceMemoryProperties);
  VkPhysicalDeviceMemoryProperties mp;
  pMemProps(this.phys, &mp);
  uint32_t mti = UINT32_MAX;
  const VkMemoryPropertyFlags want =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
    VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  if (!pick_memory_type(&mp, req.memoryTypeBits, want, &mti) &&
      !pick_memory_type(&mp, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &mti))
  {
    DEBUG_ERROR("Helios: no host-visible image memory type");
    return false;
  }
  this.memFlags = mp.memoryTypes[mti].propertyFlags;

  VkExportMemoryAllocateInfo exp =
  {
    .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
  };
  VkMemoryDedicatedAllocateInfo dedi =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
    .pNext = &exp,
    .image = this.image,
  };
  VkMemoryAllocateInfo mai =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .pNext = &dedi,
    .allocationSize = req.size,
    .memoryTypeIndex = mti,
  };
  if (this.pAlloc(this.dev, &mai, NULL, &this.mem) != VK_SUCCESS ||
      this.pBindImg(this.dev, this.image, this.mem, 0) != VK_SUCCESS)
  {
    DEBUG_ERROR("Helios: image memory allocation/bind failed");
    return false;
  }
  this.memSize = req.size;

  VkImageSubresource sub =
  {
    .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
    .mipLevel = 0,
    .arrayLayer = 0,
  };
  VkSubresourceLayout layout;
  this.pSubLayout(this.dev, this.image, &sub, &layout);
  this.stride = (uint32_t)layout.rowPitch;
  this.offset = (uint32_t)layout.offset;

  if (this.pMap(this.dev, this.mem, 0, VK_WHOLE_SIZE, 0, &this.map) != VK_SUCCESS)
  {
    DEBUG_ERROR("Helios: vkMapMemory failed");
    return false;
  }

  this.resourceId = read_resid_for_size(this.gateFile, (uint64_t)req.size);
  if (!this.resourceId)
  {
    DEBUG_ERROR("Helios: no resource id found for image allocation size %llu",
        (unsigned long long)req.size);
    return false;
  }

  this.width = width;
  this.height = height;
  this.imageReady = true;
  DEBUG_INFO("Helios: scanout image %ux%u res_id=%u stride=%u offset=%u memFlags=0x%x",
      width, height, this.resourceId, this.stride, this.offset, this.memFlags);
  return true;
}

static bool helios_supported_format(CaptureFormat format)
{
  return format == CAPTURE_FMT_BGRA || format == CAPTURE_FMT_BGR_32;
}

void helios_sink_present(const CaptureFrame * frame, const FrameBuffer * fb)
{
  if (!this.enabled || !this.initialized || !frame || !fb)
    return;

  if (!helios_supported_format(frame->format))
  {
    if (!this.warnedFormat)
    {
      DEBUG_WARN("Helios: unsupported capture format %d; only BGRA/BGR_32 are mirrored",
          frame->format);
      this.warnedFormat = true;
    }
    return;
  }

  if (!this.imageReady ||
      this.width != frame->frameWidth ||
      this.height != frame->frameHeight)
  {
    if (!helios_sink_create_image(frame->frameWidth, frame->frameHeight))
      return;
  }

  const uint8_t * src = framebuffer_get_buffer(fb);
  uint8_t * dstBase = (uint8_t *)this.map + this.offset;
  const uint32_t copyPitch = frame->pitch < this.stride ? frame->pitch : this.stride;
  const uint32_t rows = frame->dataHeight < this.height ? frame->dataHeight : this.height;

  for (uint32_t y = 0; y < rows; ++y)
    memcpy(dstBase + (size_t)y * this.stride, src + (size_t)y * frame->pitch,
        copyPitch);

  if (!(this.memFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
  {
    VkMappedMemoryRange range =
    {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = this.mem,
      .offset = 0,
      .size = VK_WHOLE_SIZE,
    };
    this.pFlush(this.dev, 1, &range);
  }

  /* The Helios ICD flushes tracked cached coherent mappings before queue submit.
   * A zero-work submit is enough to make CPU-written pixels visible to the host
   * before SET_SCANOUT_BLOB/RESOURCE_FLUSH reads the dmabuf. */
  this.pSubmit(this.queue, 0, NULL, VK_NULL_HANDLE);
  this.pQueueWaitIdle(this.queue);

  struct helios_escape_present_blob pb =
  {
    .hdr =
    {
      .magic = HELIOS_ESCAPE_MAGIC,
      .cmd_type = HELIOS_ESCAPE_PRESENT_BLOB,
      .version = HELIOS_ESCAPE_VERSION,
      .size = sizeof(pb),
    },
    .resource_id = this.resourceId,
    .width = this.width,
    .height = this.height,
    .format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
    .stride = this.stride,
    .offset = this.offset,
  };

  DWORD returned = 0;
  if (!DeviceIoControl(this.hdev, IOCTL_HELIOS_PRESENT_BLOB, &pb, sizeof(pb),
        &pb, sizeof(pb), &returned, NULL))
  {
    DEBUG_ERROR("Helios: PRESENT_BLOB failed: %lu", GetLastError());
    helios_sink_destroy_image();
  }
}
