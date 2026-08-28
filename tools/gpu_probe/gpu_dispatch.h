/*
 * Spektrafilm for Android — GPU device probe: generic Vulkan dispatch host. GPLv3.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Film modeling powered by spektrafilm.
 *
 * --------------------------------------------------------------------------------
 * Probe-local host for the M2 kernels (filming.comp / printing.comp). Same
 * per-call design as the engine's gpu/vulkan_compute.cpp scan host (instance +
 * device cached per process; buffers, pipeline, descriptor set and command buffer
 * re-created every dispatch; host-visible memory, vkQueueWaitIdle round trip) so
 * the measured behaviour matches the M1 probe's dispatch regime. Engine sources
 * stay byte-untouched — this file lives under tools/.
 * --------------------------------------------------------------------------------
 */
#ifndef SPK_TOOLS_GPU_PROBE_DISPATCH_H
#define SPK_TOOLS_GPU_PROBE_DISPATCH_H

#include <cstddef>
#include <cstdint>

namespace probe {

// One storage-buffer binding. `src` non-null: uploaded before dispatch.
// `dst` non-null: read back after dispatch. Binding index = array position.
struct Buf {
    const void* src;
    void* dst;
    size_t bytes;
};

bool gpu_available();

// Dispatch `groups` workgroups of the compute shader in `spirv` with the given
// push-constant blob and storage buffers. Returns false on any Vulkan failure.
bool dispatch(const uint32_t* spirv, size_t spirv_bytes,
              const void* push, uint32_t push_bytes,
              Buf* bufs, uint32_t nbufs, uint32_t groups);

}  // namespace probe

#endif  // SPK_TOOLS_GPU_PROBE_DISPATCH_H
