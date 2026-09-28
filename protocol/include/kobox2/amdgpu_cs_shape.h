/* SPDX-License-Identifier: MIT */
#ifndef KOBOX2_AMDGPU_CS_SHAPE_H
#define KOBOX2_AMDGPU_CS_SHAPE_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif
#include "../../generated/include/kobox2/gpu_drm_amdgpu_layout.h"
#define KB2_AMDGPU_CS_PARTS 64u
#define KB2_AMDGPU_CS_BYTES 1048576u
/* Decoded private plan, not a serialized C layout or a user pointer. */
struct kb2_amdgpu_cs_part { uint32_t record, count, offset, bytes; };
static inline uint32_t kb2_amdgpu_cs_element_size(uint32_t record)
{
    switch (record) {
#define PART(name) case KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_##name: \
    return KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_##name##_SIZE
    PART(IB); PART(USER_FENCE); PART(DEPENDENCY); PART(SYNCOBJ_IN);
    PART(SYNCOBJ_OUT); PART(BO_HANDLE); PART(SCHEDULED_DEPENDENCY);
    PART(TIMELINE_WAIT); PART(TIMELINE_SIGNAL); PART(GFX_SHADOW);
#undef PART
    default: return 0;
    }
}
static inline int kb2_amdgpu_cs_part_valid(const struct kb2_amdgpu_cs_part *p)
{
    uint32_t size = kb2_amdgpu_cs_element_size(p->record);
    if (!size || !p->count || p->count > 65536 ||
        p->count > KB2_AMDGPU_CS_BYTES / size || p->bytes != size * p->count ||
        p->offset > KB2_AMDGPU_CS_BYTES - p->bytes) return 0;
    if ((p->record == KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_IB ||
         p->record == KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_USER_FENCE ||
         p->record == KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_GFX_SHADOW) && p->count != 1)
        return 0;
    return 1;
}
#endif
