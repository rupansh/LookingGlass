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
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#pragma once

#include <stdint.h>

#define LG_PIPE_NAME "\\\\.\\pipe\\LookingGlassIDD"
#define LG_HELIOS_PIPE_NAME "\\\\.\\pipe\\LookingGlassIDDHelios"
#define LG_HELIOS_UPLOAD_MAPPING_NAME "Global\\LookingGlassIDDHeliosUpload"
#define LG_HELIOS_UPLOAD_SIZE (64u * 1024u * 1024u)
#define LG_HELIOS_DIRECT_PRESENT_VERSION 1

struct LGPipeMsg
{
  unsigned size;
  enum
  {
    SETCURSORPOS,
    SETDISPLAYMODE,
    GPUSTATUS,
    RELOADSETTINGS,
    HELIOS_ACQUIRE_FRAME,
    HELIOS_ACQUIRE_FRAME_REPLY,
    HELIOS_COMMIT_FRAME,
    HELIOS_COMMIT_FRAME_REPLY
  }
  type;
  union
  {
    struct
    {
      uint32_t x;
      uint32_t y;
    }
    curorPos;

    struct
    {
      uint32_t width;
      uint32_t height;
      uint32_t refresh;
    }
    displayMode;

    struct
    {
      bool software;
    }
    gpuStatus;

    struct
    {
      uint32_t version;
      uint32_t width;
      uint32_t height;
      uint32_t pitch;
      uint32_t frameType;
    }
    heliosAcquire;

    struct
    {
      uint32_t version;
      uint32_t status;
      uint32_t frameIndex;
      uint32_t frameOffset;
      uint32_t dataOffset;
      uint32_t maxSize;
      uint32_t serial;
    }
    heliosAcquireReply;

    struct
    {
      uint32_t version;
      uint32_t frameIndex;
      uint32_t width;
      uint32_t height;
      uint32_t pitch;
      uint32_t frameType;
      uint32_t damageX;
      uint32_t damageY;
      uint32_t damageWidth;
      uint32_t damageHeight;
    }
    heliosCommit;

    uint8_t heliosWirePadding[64];

    struct
    {
      uint32_t version;
      uint32_t status;
    }
    heliosCommitReply;
  };
};
