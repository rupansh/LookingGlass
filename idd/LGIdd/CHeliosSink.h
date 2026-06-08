/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */

#pragma once

#include <Windows.h>
#include <stdint.h>

#include "common/KVMFR.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

class CHeliosSink
{
private:
  bool m_enabled = false;
  bool m_initialized = false;
  bool m_imageReady = false;
  bool m_warnedFormat = false;

  char m_gateFile[MAX_PATH] = {};
  char m_gpuMatch[128] = {};

  HANDLE m_hdev = INVALID_HANDLE_VALUE;
  HMODULE m_vulkan = nullptr;

  PFN_vkGetInstanceProcAddr m_gipa = nullptr;
  PFN_vkGetDeviceProcAddr m_gdpa = nullptr;

  VkInstance m_inst = VK_NULL_HANDLE;
  VkPhysicalDevice m_phys = VK_NULL_HANDLE;
  VkDevice m_dev = VK_NULL_HANDLE;
  VkQueue m_queue = VK_NULL_HANDLE;
  uint32_t m_qfi = 0;

  PFN_vkCreateImage m_pCreateImage = nullptr;
  PFN_vkDestroyImage m_pDestroyImage = nullptr;
  PFN_vkGetImageMemoryRequirements m_pImgReq = nullptr;
  PFN_vkAllocateMemory m_pAlloc = nullptr;
  PFN_vkFreeMemory m_pFreeMem = nullptr;
  PFN_vkBindImageMemory m_pBindImg = nullptr;
  PFN_vkGetImageSubresourceLayout m_pSubLayout = nullptr;
  PFN_vkMapMemory m_pMap = nullptr;
  PFN_vkUnmapMemory m_pUnmap = nullptr;
  PFN_vkFlushMappedMemoryRanges m_pFlush = nullptr;
  PFN_vkQueueSubmit m_pSubmit = nullptr;
  PFN_vkQueueWaitIdle m_pQueueWaitIdle = nullptr;
  PFN_vkDeviceWaitIdle m_pDeviceWaitIdle = nullptr;

  VkImage m_image = VK_NULL_HANDLE;
  VkDeviceMemory m_mem = VK_NULL_HANDLE;
  void * m_map = nullptr;
  VkDeviceSize m_memSize = 0;
  VkMemoryPropertyFlags m_memFlags = 0;

  uint32_t m_resourceId = 0;
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  uint32_t m_stride = 0;
  uint32_t m_offset = 0;

  HANDLE OpenDevice();
  bool InitVulkan();
  void DestroyImage();
  bool CreateImage(uint32_t width, uint32_t height);
  bool SupportedFormat(FrameType type) const;

public:
  bool Init();
  void Deinit();
  void Present(const KVMFRFrame * frame, const void * data);
};
