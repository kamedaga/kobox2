/* SPDX-License-Identifier: MIT */
#ifndef KOBOX2_AMDGPU_INFO_SHAPE_H
#define KOBOX2_AMDGPU_INFO_SHAPE_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif
#include "../../generated/include/kobox2/gpu_drm_amdgpu_layout.h"

/* One shape definition is shared by admission and completion validation.
 * These are bounded schema records, never arbitrary ioctl byte buffers. */
struct kb2_amdgpu_info_shape { uint32_t record, count, bytes; };
static inline int kb2_amdgpu_info_shape(uint32_t query, const uint32_t s[4],
    struct kb2_amdgpu_info_shape *out)
{
    uint32_t record = 0, count = 1, size = 0, selectors = 0;
#define SHAPE(name) do { record = KB2_GPU_DRM_AMDGPU_RECORD_##name; \
    size = KB2_GPU_DRM_AMDGPU_RECORD_##name##_SIZE; } while (0)
    switch (query) {
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_READ_MMR_REGISTER:
        SHAPE(SCALAR_U32); selectors = 4; count = s[1];
        if (!count || count > 128 || s[0] > ((uint32_t)~0u) - (count - 1)) return 0;
        break;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_MAXIMUM_IBS:
        SHAPE(SCALAR_U32); count = 10; break;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_HW_IP_COUNT:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_SENSOR:
        selectors = 1;
        /* fall through */
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_ACCELERATION_WORKING:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_VRAM_LOST_COUNTER:
        SHAPE(SCALAR_U32); break;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_TIMESTAMP:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_BYTES_MOVED:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_VRAM_USAGE:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_GTT_USAGE:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_VISIBLE_VRAM_USAGE:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_EVICTION_COUNT:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_VRAM_CPU_PAGE_FAULTS:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_RAS_ENABLED_FEATURES:
        SHAPE(SCALAR_U64); break;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_DEVICE_INFO:
        SHAPE(INFO_DEVICE); break;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_HW_IP_INFO:
        SHAPE(INFO_HW_IP); selectors = 2; break;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_FIRMWARE_VERSION:
        SHAPE(INFO_FIRMWARE); selectors = 3; break;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_VRAM_GTT:
        SHAPE(INFO_VRAM_GTT); break;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_MEMORY_INFO:
        SHAPE(INFO_MEMORY); break;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_VIDEO_CAPS:
        SHAPE(INFO_VIDEO_CAPS); selectors = 1;
        if (s[0] > 1) return 0;
        break;
    default: return 0;
    }
#undef SHAPE
    for (uint32_t i = selectors; i < 4; ++i) if (s[i]) return 0;
    *out = (struct kb2_amdgpu_info_shape){record, count, size * count};
    return 1;
}
#endif
