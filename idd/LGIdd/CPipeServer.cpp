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

#include "CPipeServer.h"
#include "CDebug.h"
#include "CIndirectDeviceContext.h"

CPipeServer g_pipe;

bool CPipeServer::Init()
{
  _DeInit();

  m_pipe.Attach(CreateNamedPipeA(
    LG_PIPE_NAME,
    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
    1,
    1024,
    1024,
    0,
    NULL));

  if (!m_pipe.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create the named pipe");
    return false;
  }

  SECURITY_DESCRIPTOR heliosPipeSd = {};
  SECURITY_ATTRIBUTES heliosPipeSa = {};
  InitializeSecurityDescriptor(&heliosPipeSd, SECURITY_DESCRIPTOR_REVISION);
  SetSecurityDescriptorDacl(&heliosPipeSd, TRUE, NULL, FALSE);
  heliosPipeSa.nLength = sizeof(heliosPipeSa);
  heliosPipeSa.lpSecurityDescriptor = &heliosPipeSd;

  m_heliosUploadMapping.Attach(CreateFileMappingA(
    INVALID_HANDLE_VALUE,
    &heliosPipeSa,
    PAGE_READWRITE,
    0,
    LG_HELIOS_UPLOAD_SIZE,
    LG_HELIOS_UPLOAD_MAPPING_NAME));

  if (!m_heliosUploadMapping.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create the Helios upload mapping");
    return false;
  }

  m_heliosUpload = MapViewOfFile(
    m_heliosUploadMapping.Get(),
    FILE_MAP_READ | FILE_MAP_WRITE,
    0,
    0,
    LG_HELIOS_UPLOAD_SIZE);

  if (!m_heliosUpload)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to map the Helios upload buffer");
    return false;
  }

  m_heliosPipe.Attach(CreateNamedPipeA(
    LG_HELIOS_PIPE_NAME,
    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
    1,
    1024,
    1024,
    0,
    &heliosPipeSa));

  if (!m_heliosPipe.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create the Helios named pipe");
    return false;
  }

  m_signal.Attach(CreateEvent(NULL, TRUE, FALSE, NULL));
  if (!m_signal.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create pipe signal event");
    return false;
  }

  m_heliosSignal.Attach(CreateEvent(NULL, TRUE, FALSE, NULL));
  if (!m_heliosSignal.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create Helios pipe signal event");
    return false;
  }

  m_running = true;
  m_thread.Attach(CreateThread(
    NULL,
    0,
    _pipeThread,
    (LPVOID)this,
    0,
    NULL));

  if (!m_thread.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create the pipe thread");
    return false;
  }

  m_heliosThread.Attach(CreateThread(
    NULL,
    0,
    _heliosPipeThread,
    (LPVOID)this,
    0,
    NULL));

  if (!m_heliosThread.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create the Helios pipe thread");
    return false;
  }

  DEBUG_TRACE("Pipe Initialized");
  return true;
}

void CPipeServer::_DeInit()
{
  m_running = false;
  m_connected = false;
  if (m_signal.IsValid())
    SetEvent(m_signal.Get());
  if (m_heliosSignal.IsValid())
    SetEvent(m_heliosSignal.Get());

  if (m_thread.IsValid())
  {
    WaitForSingleObject(m_thread.Get(), INFINITE);
    m_thread.Close();
  }

  if (m_heliosThread.IsValid())
  {
    WaitForSingleObject(m_heliosThread.Get(), INFINITE);
    m_heliosThread.Close();
  }

  if (m_pipe.IsValid())
  {
    FlushFileBuffers(m_pipe.Get());
    m_pipe.Close();
  }

  if (m_heliosPipe.IsValid())
  {
    FlushFileBuffers(m_heliosPipe.Get());
    m_heliosPipe.Close();
  }

  if (m_heliosUpload)
  {
    UnmapViewOfFile(m_heliosUpload);
    m_heliosUpload = nullptr;
  }
  m_heliosUploadMapping.Close();

  m_signal.Close();
  m_heliosSignal.Close();
}

void CPipeServer::DeInit()
{  
  DEBUG_TRACE("Pipe Stopping");
  _DeInit();
  DEBUG_TRACE("Pipe Stopped");
}

void CPipeServer::Thread()
{
  DEBUG_TRACE("Pipe thread started");

  HandleT<EventTraits> ioEvent(CreateEvent(NULL, TRUE, FALSE, NULL));
  if (!ioEvent.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Can't create event for overlapped I/O!");
    WaitForSingleObject(m_signal.Get(), 5000);
    return;
  }

  while(m_running)
  {
    m_connected = false;

    OVERLAPPED overlapped = { 0 };
    overlapped.hEvent = ioEvent.Get();

    if (!ConnectNamedPipe(m_pipe.Get(), &overlapped))
    {
      DWORD dwError = GetLastError();
      switch (dwError) {
      case ERROR_PIPE_CONNECTED:
        break;
      case ERROR_IO_PENDING:
      {
        HANDLE hWait[] = { ioEvent.Get(), m_signal.Get() };
        switch (WaitForMultipleObjects(2, hWait, FALSE, INFINITE))
        {
        case WAIT_OBJECT_0:
          break;
        case WAIT_OBJECT_0 + 1:
          DEBUG_INFO("Connect interrupted by signal");
          CancelIo(m_pipe.Get());
          WaitForSingleObject(ioEvent.Get(), INFINITE);
          continue;
        }
        break;
      }
      default:
        DEBUG_ERROR_HR(dwError, "Error connecting to the named pipe");
        goto end;
      }
    }

    DEBUG_TRACE("Client connected");

    m_connected = true;

    for (const auto& msg : m_queue)
      WriteMsg(msg);
    m_queue.clear();

    while (m_running && m_connected)
    {
      LGPipeMsg msg;

      if (!ReadFile(m_pipe.Get(), &msg, sizeof(msg), NULL, &overlapped))
      {
        DWORD dwError = GetLastError();
        if (dwError != ERROR_IO_PENDING)
        {
          DEBUG_ERROR_HR(dwError, "ReadFile Failed");
          break;
        }

        HANDLE hWait[] = { ioEvent.Get(), m_signal.Get() };
        switch (WaitForMultipleObjects(2, hWait, FALSE, INFINITE))
        {
        case WAIT_OBJECT_0:
          break;
        case WAIT_OBJECT_0 + 1:
          DEBUG_INFO("I/O interrupted by signal");
          CancelIo(m_pipe.Get());
          WaitForSingleObject(ioEvent.Get(), INFINITE);
          continue;
        }
      }

      DWORD bytesRead;
      GetOverlappedResult(m_pipe.Get(), &overlapped, &bytesRead, TRUE);

      if (bytesRead != sizeof(msg))
      {
        DEBUG_ERROR("Corrupted data, expected %lld bytes, read %lld bytes", sizeof msg, bytesRead);
        break;
      }

      if (msg.size != sizeof(msg))
      {
        DEBUG_ERROR("Corrupted data, expected %lld bytes, actual message size: %lld bytes", sizeof msg, msg.size);
        break;
      }

      switch (msg.type)
      {
      case LGPipeMsg::RELOADSETTINGS:
        HandleReloadSettings();
        break;

      case LGPipeMsg::HELIOS_ACQUIRE_FRAME:
        HandleHeliosAcquireFrame(msg);
        break;

      case LGPipeMsg::HELIOS_COMMIT_FRAME:
        HandleHeliosCommitFrame(msg);
        break;

      default:
        DEBUG_ERROR("Unknown message type %d", msg.type);
        break;
      }
    }

    DEBUG_TRACE("Client disconnected");
    DisconnectNamedPipe(m_pipe.Get());

    if (m_running)
      ResetEvent(m_signal.Get());
  }

end:
  m_running   = false;
  m_connected = false;
  DEBUG_TRACE("Pipe thread shutdown");
}

void CPipeServer::HeliosThread()
{
  DEBUG_TRACE("Helios pipe thread started");

  HandleT<EventTraits> ioEvent(CreateEvent(NULL, TRUE, FALSE, NULL));
  if (!ioEvent.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Can't create event for Helios overlapped I/O");
    WaitForSingleObject(m_heliosSignal.Get(), 5000);
    return;
  }

  while (m_running)
  {
    ResetEvent(ioEvent.Get());
    OVERLAPPED connectOverlapped = { 0 };
    connectOverlapped.hEvent = ioEvent.Get();

    if (!ConnectNamedPipe(m_heliosPipe.Get(), &connectOverlapped))
    {
      DWORD dwError = GetLastError();
      switch (dwError)
      {
      case ERROR_PIPE_CONNECTED:
        break;
      case ERROR_IO_PENDING:
      {
        HANDLE hWait[] = { ioEvent.Get(), m_heliosSignal.Get() };
        switch (WaitForMultipleObjects(2, hWait, FALSE, INFINITE))
        {
        case WAIT_OBJECT_0:
          GetOverlappedResult(m_heliosPipe.Get(), &connectOverlapped, &dwError, FALSE);
          break;
        case WAIT_OBJECT_0 + 1:
          CancelIo(m_heliosPipe.Get());
          WaitForSingleObject(ioEvent.Get(), INFINITE);
          continue;
        }
        break;
      }
      default:
        DEBUG_ERROR_HR(dwError, "Error connecting to the Helios named pipe");
        goto end;
      }
    }

    while (m_running)
    {
      ResetEvent(ioEvent.Get());
      OVERLAPPED readOverlapped = { 0 };
      readOverlapped.hEvent = ioEvent.Get();
      LGPipeMsg msg = {};
      if (!ReadFile(m_heliosPipe.Get(), &msg, sizeof(msg), NULL, &readOverlapped))
      {
        DWORD dwError = GetLastError();
        if (dwError != ERROR_IO_PENDING)
          break;

        HANDLE hWait[] = { ioEvent.Get(), m_heliosSignal.Get() };
        switch (WaitForMultipleObjects(2, hWait, FALSE, INFINITE))
        {
        case WAIT_OBJECT_0:
          break;
        case WAIT_OBJECT_0 + 1:
          CancelIo(m_heliosPipe.Get());
          WaitForSingleObject(ioEvent.Get(), INFINITE);
          goto disconnect;
        }
      }

      DWORD bytesRead = 0;
      GetOverlappedResult(m_heliosPipe.Get(), &readOverlapped, &bytesRead, TRUE);
      if (bytesRead != sizeof(msg) || msg.size != sizeof(msg))
        break;

      switch (msg.type)
      {
      case LGPipeMsg::HELIOS_ACQUIRE_FRAME:
        HandleHeliosAcquireFrame(msg);
        break;
      case LGPipeMsg::HELIOS_COMMIT_FRAME:
        HandleHeliosCommitFrame(msg);
        break;
      default:
        DEBUG_ERROR("Unknown Helios pipe message type %d", msg.type);
        break;
      }
    }

disconnect:
    DisconnectNamedPipe(m_heliosPipe.Get());
    if (m_running)
      ResetEvent(m_heliosSignal.Get());
  }

end:
  DEBUG_TRACE("Helios pipe thread shutdown");
}

void CPipeServer::WriteMsg(const LGPipeMsg & msg)
{
  if (!m_connected)
  {
    m_queue.push_back(msg);
    return;
  }

  DWORD written;
  if (!WriteFile(m_pipe.Get(), &msg, sizeof(msg), &written, NULL))
  {
    DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA)
    {
      DEBUG_WARN_HR(err, "Client disconnected, failed to write");
      m_connected = false;
      SetEvent(m_signal.Get());
      return;
    }

    DEBUG_WARN_HR(err, "WriteFile failed on the pipe");
    return;
  }

  FlushFileBuffers(m_pipe.Get());
}

void CPipeServer::WriteHeliosMsg(const LGPipeMsg & msg)
{
  DWORD written;
  if (!WriteFile(m_heliosPipe.Get(), &msg, sizeof(msg), &written, NULL))
    DEBUG_WARN_HR(GetLastError(), "WriteFile failed on the Helios pipe");
}

void CPipeServer::HandleReloadSettings()
{
  DEBUG_INFO("TODO: reload settings");
}

void CPipeServer::HandleHeliosAcquireFrame(const LGPipeMsg & msg)
{
  LGPipeMsg reply = {};
  reply.size = sizeof(reply);
  reply.type = LGPipeMsg::HELIOS_ACQUIRE_FRAME_REPLY;
  reply.heliosAcquireReply.version = LG_HELIOS_DIRECT_PRESENT_VERSION;
  reply.heliosAcquireReply.status = 1;

  if (msg.heliosAcquire.version != LG_HELIOS_DIRECT_PRESENT_VERSION)
    reply.heliosAcquireReply.status = 2;
  else if (m_deviceContext)
  {
    auto frame = m_deviceContext->AcquireDirectFrame(
      msg.heliosAcquire.width,
      msg.heliosAcquire.height,
      msg.heliosAcquire.pitch,
      msg.heliosAcquire.frameType);
    reply.heliosAcquireReply.status = frame.status;
    reply.heliosAcquireReply.frameIndex = frame.frameIndex;
    reply.heliosAcquireReply.frameOffset = frame.frameOffset;
    reply.heliosAcquireReply.dataOffset = frame.dataOffset;
    reply.heliosAcquireReply.maxSize = frame.maxSize;
    reply.heliosAcquireReply.serial = frame.serial;
  }

  WriteHeliosMsg(reply);
}

void CPipeServer::HandleHeliosCommitFrame(const LGPipeMsg & msg)
{
  LGPipeMsg reply = {};
  reply.size = sizeof(reply);
  reply.type = LGPipeMsg::HELIOS_COMMIT_FRAME_REPLY;
  reply.heliosCommitReply.version = LG_HELIOS_DIRECT_PRESENT_VERSION;
  reply.heliosCommitReply.status = 1;

  if (msg.heliosCommit.version != LG_HELIOS_DIRECT_PRESENT_VERSION)
    reply.heliosCommitReply.status = 2;
  else if (msg.heliosCommit.width == 0 || msg.heliosCommit.height == 0)
    HandleHeliosClearFrame(reply);
  else if (m_deviceContext)
  {
    RECT damage = {};
    damage.left = msg.heliosCommit.damageX;
    damage.top = msg.heliosCommit.damageY;
    damage.right = msg.heliosCommit.damageX + msg.heliosCommit.damageWidth;
    damage.bottom = msg.heliosCommit.damageY + msg.heliosCommit.damageHeight;
    reply.heliosCommitReply.status = m_deviceContext->CommitDirectFrame(
      msg.heliosCommit.frameIndex,
      msg.heliosCommit.width,
      msg.heliosCommit.height,
      msg.heliosCommit.pitch,
      msg.heliosCommit.frameType,
      m_heliosUpload,
      LG_HELIOS_UPLOAD_SIZE,
      damage);
  }

  WriteHeliosMsg(reply);
}

void CPipeServer::HandleHeliosClearFrame(LGPipeMsg & reply)
{
  if (m_deviceContext)
    reply.heliosCommitReply.status = m_deviceContext->ClearDirectFrame();
}

void CPipeServer::SetDeviceContext(CIndirectDeviceContext * context)
{
  m_deviceContext = context;
}

void CPipeServer::SetCursorPos(uint32_t x, uint32_t y)
{
  // do not send cursor messages if we are not connected or they will end up queued
  if (!m_connected)
    return;

  LGPipeMsg msg;
  msg.size       = sizeof(msg);
  msg.type       = LGPipeMsg::SETCURSORPOS;
  msg.curorPos.x = x;
  msg.curorPos.y = y;
  WriteMsg(msg);
}

void CPipeServer::SetDisplayMode(uint32_t width, uint32_t height, uint32_t refresh)
{
  LGPipeMsg msg;
  msg.size                = sizeof(msg);
  msg.type                = LGPipeMsg::SETDISPLAYMODE;
  msg.displayMode.width   = width;
  msg.displayMode.height  = height;
  msg.displayMode.refresh = refresh;
  WriteMsg(msg);
}

void CPipeServer::SetGPUStatus(bool software)
{
  LGPipeMsg msg;
  msg.size               = sizeof(msg);
  msg.type               = LGPipeMsg::GPUSTATUS;
  msg.gpuStatus.software = software;
  WriteMsg(msg);
}
