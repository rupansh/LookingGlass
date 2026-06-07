/**
 * Looking Glass Helios output sink.
 *
 * Optional Windows-only path that mirrors captured Looking Glass frames into the
 * Helios System-class Venus ICD/KMD and presents the backing virtio-gpu blob via
 * IOCTL_HELIOS_PRESENT_BLOB. Non-Windows builds get no-op stubs.
 */

#pragma once

#include "interface/capture.h"

#ifdef _WIN32
void helios_sink_initOptions(void);
bool helios_sink_init(void);
void helios_sink_deinit(void);
void helios_sink_present(const CaptureFrame * frame, const FrameBuffer * fb);
#else
static inline void helios_sink_initOptions(void) {}
static inline bool helios_sink_init(void) { return true; }
static inline void helios_sink_deinit(void) {}
static inline void helios_sink_present(const CaptureFrame * frame,
    const FrameBuffer * fb)
{
  (void)frame;
  (void)fb;
}
#endif
