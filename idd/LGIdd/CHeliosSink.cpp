/**
 * Looking Glass Helios IDD Vulkan sink.
 *
 * This opt-in bridge mirrors completed LG IDD frames into a Venus-backed Vulkan
 * image and asks the Helios KMD to scan out the resulting virtio-gpu blob. It is
 * intentionally conservative: the IDD/KVMFR path remains authoritative, and this
 * sink is enabled only by HKLM\SOFTWARE\LookingGlass\IDD\HeliosEnable.
 */

#include "CHeliosSink.h"

#include "CDebug.h"
#include "CSettings.h"

#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

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
  helios_escape_header hdr;
  uint32_t resource_id;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t stride;
  uint32_t offset;
};

#define ILOAD(name) (PFN_##name)(void *)m_gipa(m_inst, #name)
#define DLOAD(name) (PFN_##name)(void *)m_gdpa(m_dev, #name)

static void narrow_copy(char * dst, size_t dstSize, const std::wstring& src)
{
  if (!dst || !dstSize)
    return;

  if (src.empty())
  {
    dst[0] = 0;
    return;
  }

  WideCharToMultiByte(CP_UTF8, 0, src.c_str(), -1, dst, (int)dstSize, NULL, NULL);
  dst[dstSize - 1] = 0;
}

static uint32_t read_resid_for_size(const char * path, uint64_t wantSize)
{
  FILE * f = nullptr;
  if (fopen_s(&f, path, "r") != 0 || !f)
    return 0;

  uint32_t res = 0;
  uint32_t rid;
  unsigned long long size;
  while (fscanf_s(f, "%u %llu", &rid, &size) == 2)
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

HANDLE CHeliosSink::OpenDevice()
{
  HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_HELIOS, NULL, NULL,
      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (di == INVALID_HANDLE_VALUE)
  {
    DEBUG_ERROR("Helios: SetupDiGetClassDevs failed: %lu", GetLastError());
    return INVALID_HANDLE_VALUE;
  }

  SP_DEVICE_INTERFACE_DATA ifd = {};
  ifd.cbSize = sizeof(ifd);
  if (!SetupDiEnumDeviceInterfaces(di, NULL, &GUID_DEVINTERFACE_HELIOS, 0, &ifd))
  {
    DEBUG_ERROR("Helios: no device interface present: %lu", GetLastError());
    SetupDiDestroyDeviceInfoList(di);
    return INVALID_HANDLE_VALUE;
  }

  DWORD need = 0;
  SetupDiGetDeviceInterfaceDetailW(di, &ifd, NULL, 0, &need, NULL);
  SP_DEVICE_INTERFACE_DETAIL_DATA_W * detail =
    (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)calloc(1, need);

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

bool CHeliosSink::InitVulkan()
{
  m_vulkan = LoadLibraryW(L"vulkan-1.dll");
  if (!m_vulkan)
  {
    DEBUG_ERROR("Helios: failed to load vulkan-1.dll");
    return false;
  }

  m_gipa = (PFN_vkGetInstanceProcAddr)(void *)
    GetProcAddress(m_vulkan, "vkGetInstanceProcAddr");
  if (!m_gipa)
    return false;

  PFN_vkCreateInstance pCreateInstance =
    (PFN_vkCreateInstance)m_gipa(NULL, "vkCreateInstance");
  if (!pCreateInstance)
    return false;

  VkApplicationInfo app = {};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "looking-glass-idd-helios";
  app.apiVersion = VK_API_VERSION_1_1;

  VkInstanceCreateInfo ici = {};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  if (pCreateInstance(&ici, NULL, &m_inst) != VK_SUCCESS)
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
  if (pEnum(m_inst, &count, NULL) != VK_SUCCESS || count == 0)
  {
    DEBUG_ERROR("Helios: no Vulkan physical devices");
    return false;
  }

  VkPhysicalDevice devs[16];
  if (count > _countof(devs))
    count = _countof(devs);
  if (pEnum(m_inst, &count, devs) != VK_SUCCESS)
    return false;

  m_phys = devs[0];
  for (uint32_t i = 0; i < count; ++i)
  {
    VkPhysicalDeviceProperties pp;
    pProps(devs[i], &pp);
    DEBUG_INFO("Helios: Vulkan device[%u]: %s", i, pp.deviceName);
    if (strstr(pp.deviceName, m_gpuMatch))
      m_phys = devs[i];
  }

  uint32_t qfCount = 0;
  pQFP(m_phys, &qfCount, NULL);
  VkQueueFamilyProperties qfs[16];
  if (qfCount > _countof(qfs))
    qfCount = _countof(qfs);
  pQFP(m_phys, &qfCount, qfs);

  m_qfi = 0;
  for (uint32_t i = 0; i < qfCount; ++i)
    if (qfs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
    {
      m_qfi = i;
      break;
    }

  uint32_t extCount = 0;
  pEnumExt(m_phys, NULL, &extCount, NULL);
  VkExtensionProperties * exts =
    (VkExtensionProperties *)calloc(extCount ? extCount : 1, sizeof(*exts));
  if (!exts)
    return false;
  pEnumExt(m_phys, NULL, &extCount, exts);

  const char * wanted[] =
  {
    "VK_KHR_external_memory",
    "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf",
    "VK_EXT_image_drm_format_modifier",
  };
  const char * enabled[_countof(wanted)];
  uint32_t enabledCount = 0;
  for (uint32_t w = 0; w < _countof(wanted); ++w)
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
  VkDeviceQueueCreateInfo qci = {};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = m_qfi;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;

  VkDeviceCreateInfo dci = {};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = enabledCount;
  dci.ppEnabledExtensionNames = enabled;
  if (pCreateDevice(m_phys, &dci, NULL, &m_dev) != VK_SUCCESS)
  {
    DEBUG_ERROR("Helios: vkCreateDevice failed");
    return false;
  }

  m_gdpa = ILOAD(vkGetDeviceProcAddr);
  PFN_vkGetDeviceQueue pGetQueue = DLOAD(vkGetDeviceQueue);
  pGetQueue(m_dev, m_qfi, 0, &m_queue);

  m_pCreateImage = DLOAD(vkCreateImage);
  m_pDestroyImage = DLOAD(vkDestroyImage);
  m_pImgReq = DLOAD(vkGetImageMemoryRequirements);
  m_pAlloc = DLOAD(vkAllocateMemory);
  m_pFreeMem = DLOAD(vkFreeMemory);
  m_pBindImg = DLOAD(vkBindImageMemory);
  m_pSubLayout = DLOAD(vkGetImageSubresourceLayout);
  m_pMap = DLOAD(vkMapMemory);
  m_pUnmap = DLOAD(vkUnmapMemory);
  m_pFlush = DLOAD(vkFlushMappedMemoryRanges);
  m_pSubmit = DLOAD(vkQueueSubmit);
  m_pQueueWaitIdle = DLOAD(vkQueueWaitIdle);
  m_pDeviceWaitIdle = DLOAD(vkDeviceWaitIdle);

  VkPhysicalDeviceMemoryProperties mp;
  pMemProps(m_phys, &mp);
  (void)mp;
  return true;
}

bool CHeliosSink::Init()
{
  m_enabled = g_settings.ReadBoolValue(L"HeliosEnable", false);
  if (!m_enabled)
    return true;

  const std::wstring gate =
    g_settings.ReadStringValue(L"HeliosGateFile", L"C:\\Users\\Rupansh\\helios_lg_idd_resid.txt");
  const std::wstring gpu =
    g_settings.ReadStringValue(L"HeliosGpu", L"Intel");

  narrow_copy(m_gateFile, sizeof(m_gateFile), gate);
  narrow_copy(m_gpuMatch, sizeof(m_gpuMatch), gpu);

  SetEnvironmentVariableA("HELIOS_GATE_RESID_FILE", m_gateFile);
  _putenv_s("HELIOS_GATE_RESID_FILE", m_gateFile);
  DeleteFileA(m_gateFile);

  m_hdev = OpenDevice();
  if (m_hdev == INVALID_HANDLE_VALUE)
    return false;

  if (!InitVulkan())
  {
    DEBUG_ERROR("Helios: Vulkan IDD sink initialization failed");
    return false;
  }

  m_initialized = true;
  DEBUG_INFO("Helios: enabled IDD Vulkan output sink");
  return true;
}

void CHeliosSink::DestroyImage()
{
  if (!m_dev)
    return;

  if (m_pDeviceWaitIdle)
    m_pDeviceWaitIdle(m_dev);
  if (m_mem && m_map && m_pUnmap)
    m_pUnmap(m_dev, m_mem);
  m_map = nullptr;
  if (m_image && m_pDestroyImage)
    m_pDestroyImage(m_dev, m_image, NULL);
  if (m_mem && m_pFreeMem)
    m_pFreeMem(m_dev, m_mem, NULL);

  m_image = VK_NULL_HANDLE;
  m_mem = VK_NULL_HANDLE;
  m_memSize = 0;
  m_resourceId = 0;
  m_imageReady = false;
}

void CHeliosSink::Deinit()
{
  if (!m_enabled)
    return;

  DestroyImage();

  if (m_dev)
  {
    PFN_vkDestroyDevice pDestroyDevice = DLOAD(vkDestroyDevice);
    if (pDestroyDevice)
      pDestroyDevice(m_dev, NULL);
  }
  if (m_inst)
  {
    PFN_vkDestroyInstance pDestroyInstance =
      (PFN_vkDestroyInstance)m_gipa(m_inst, "vkDestroyInstance");
    if (pDestroyInstance)
      pDestroyInstance(m_inst, NULL);
  }
  if (m_vulkan)
    FreeLibrary(m_vulkan);
  if (m_hdev && m_hdev != INVALID_HANDLE_VALUE)
    CloseHandle(m_hdev);

  *this = CHeliosSink();
}

bool CHeliosSink::CreateImage(uint32_t width, uint32_t height)
{
  DestroyImage();
  DeleteFileA(m_gateFile);

  uint64_t mods[1] = { DRM_FORMAT_MOD_LINEAR };
  VkImageDrmFormatModifierListCreateInfoEXT modList = {};
  modList.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
  modList.drmFormatModifierCount = 1;
  modList.pDrmFormatModifiers = mods;

  VkExternalMemoryImageCreateInfo extImg = {};
  extImg.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  extImg.pNext = &modList;
  extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  VkImageCreateInfo ici = {};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.pNext = &extImg;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_B8G8R8A8_UNORM;
  ici.extent.width = width;
  ici.extent.height = height;
  ici.extent.depth = 1;
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  if (m_pCreateImage(m_dev, &ici, NULL, &m_image) != VK_SUCCESS)
  {
    DEBUG_ERROR("Helios: vkCreateImage failed for %ux%u", width, height);
    return false;
  }

  VkMemoryRequirements req;
  m_pImgReq(m_dev, m_image, &req);

  PFN_vkGetPhysicalDeviceMemoryProperties pMemProps =
    ILOAD(vkGetPhysicalDeviceMemoryProperties);
  VkPhysicalDeviceMemoryProperties mp;
  pMemProps(m_phys, &mp);

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
  m_memFlags = mp.memoryTypes[mti].propertyFlags;

  VkExportMemoryAllocateInfo exp = {};
  exp.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  VkMemoryDedicatedAllocateInfo dedi = {};
  dedi.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedi.pNext = &exp;
  dedi.image = m_image;

  VkMemoryAllocateInfo mai = {};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.pNext = &dedi;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mti;

  if (m_pAlloc(m_dev, &mai, NULL, &m_mem) != VK_SUCCESS ||
      m_pBindImg(m_dev, m_image, m_mem, 0) != VK_SUCCESS)
  {
    DEBUG_ERROR("Helios: image memory allocation/bind failed");
    return false;
  }
  m_memSize = req.size;

  VkImageSubresource sub = {};
  sub.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
  sub.mipLevel = 0;
  sub.arrayLayer = 0;

  VkSubresourceLayout layout;
  m_pSubLayout(m_dev, m_image, &sub, &layout);
  m_stride = (uint32_t)layout.rowPitch;
  m_offset = (uint32_t)layout.offset;

  if (m_pMap(m_dev, m_mem, 0, VK_WHOLE_SIZE, 0, &m_map) != VK_SUCCESS)
  {
    DEBUG_ERROR("Helios: vkMapMemory failed");
    return false;
  }

  m_resourceId = read_resid_for_size(m_gateFile, (uint64_t)req.size);
  if (!m_resourceId)
  {
    DEBUG_ERROR("Helios: no resource id found for image allocation size %llu",
        (unsigned long long)req.size);
    return false;
  }

  m_width = width;
  m_height = height;
  m_imageReady = true;
  DEBUG_INFO("Helios: IDD scanout image %ux%u res_id=%u stride=%u offset=%u memFlags=0x%x",
      width, height, m_resourceId, m_stride, m_offset, m_memFlags);
  return true;
}

bool CHeliosSink::SupportedFormat(FrameType type) const
{
  return type == FRAME_TYPE_BGRA || type == FRAME_TYPE_BGR_32;
}

void CHeliosSink::Present(const KVMFRFrame * frame, const void * data)
{
  if (!m_enabled || !m_initialized || !frame || !data)
    return;

  if (!SupportedFormat(frame->type))
  {
    if (!m_warnedFormat)
    {
      DEBUG_WARN("Helios: unsupported IDD frame format %u", frame->type);
      m_warnedFormat = true;
    }
    return;
  }

  if (!m_imageReady ||
      m_width != frame->frameWidth ||
      m_height != frame->frameHeight)
  {
    if (!CreateImage(frame->frameWidth, frame->frameHeight))
      return;
  }

  const uint8_t * src = (const uint8_t *)data;
  uint8_t * dstBase = (uint8_t *)m_map + m_offset;
  const uint32_t copyPitch = frame->pitch < m_stride ? frame->pitch : m_stride;
  const uint32_t rows = frame->dataHeight < m_height ? frame->dataHeight : m_height;

  for (uint32_t y = 0; y < rows; ++y)
    memcpy(dstBase + (size_t)y * m_stride, src + (size_t)y * frame->pitch,
        copyPitch);

  if (!(m_memFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
  {
    VkMappedMemoryRange range = {};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = m_mem;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    m_pFlush(m_dev, 1, &range);
  }

  m_pSubmit(m_queue, 0, NULL, VK_NULL_HANDLE);
  m_pQueueWaitIdle(m_queue);

  helios_escape_present_blob pb = {};
  pb.hdr.magic = HELIOS_ESCAPE_MAGIC;
  pb.hdr.cmd_type = HELIOS_ESCAPE_PRESENT_BLOB;
  pb.hdr.version = HELIOS_ESCAPE_VERSION;
  pb.hdr.size = sizeof(pb);
  pb.resource_id = m_resourceId;
  pb.width = m_width;
  pb.height = m_height;
  pb.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
  pb.stride = m_stride;
  pb.offset = m_offset;

  DWORD returned = 0;
  if (!DeviceIoControl(m_hdev, IOCTL_HELIOS_PRESENT_BLOB, &pb, sizeof(pb),
        &pb, sizeof(pb), &returned, NULL))
  {
    DEBUG_ERROR("Helios: PRESENT_BLOB failed: %lu", GetLastError());
    DestroyImage();
  }
}
