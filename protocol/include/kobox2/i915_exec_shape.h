/* SPDX-License-Identifier: MIT */
#ifndef KOBOX2_I915_EXEC_SHAPE_H
#define KOBOX2_I915_EXEC_SHAPE_H
#include <kobox2/i915_context_shape.h>
/* No native sync_file descriptor, extension pointer or secure batch flag is
 * admitted here. Binary syncobj fences retain their real Linux semantics. */
#define KB2_I915_EXEC_FLAGS 0x000cfdffu
static inline int kb2_i915_exec_header(const void *input,size_t bytes,size_t *aux_bytes)
{
    const unsigned char *p=input;
    if(!p || bytes!=32) return 0;
    const uint32_t objects=kb2_i915_context_u32(p),fences=kb2_i915_context_u32(p+4);
    const uint64_t flags=kb2_i915_context_u64(p+16);
    if(!objects || objects>KB2_GPU_DRM_I915_MAX_EXEC_OBJECTS ||
        fences>KB2_GPU_DRM_I915_MAX_EXEC_FENCES || (flags & ~(uint64_t)KB2_I915_EXEC_FLAGS) ||
        (!(flags & (1u<<19)) && fences) || kb2_i915_context_u32(p+28)) return 0;
    const size_t total=(size_t)objects*40+(size_t)fences*8;
    if(total>KB2_GPU_DRM_I915_MAX_EXEC_BYTES) return 0;
    if(aux_bytes) *aux_bytes=total;
    return 1;
}
static inline int kb2_i915_exec_payload(const void *input,size_t bytes,uint32_t objects,uint32_t fences)
{
    const unsigned char *p=input;
    if(!p || !objects || objects>KB2_GPU_DRM_I915_MAX_EXEC_OBJECTS ||
        fences>KB2_GPU_DRM_I915_MAX_EXEC_FENCES ||
        bytes!=(size_t)objects*40+(size_t)fences*8 || bytes>KB2_GPU_DRM_I915_MAX_EXEC_BYTES) return 0;
    for(uint32_t i=0;i<objects;++i) {
        const unsigned char *q=p+(size_t)i*40;
        const uint64_t flags=kb2_i915_context_u64(q+24);
        if(kb2_i915_context_u32(q+4) || (flags & ~(uint64_t)0xffu) ||
            (!(flags & (1u<<5)) && kb2_i915_context_u64(q+32))) return 0;
    }
    for(uint32_t i=0;i<fences;++i)
        if(kb2_i915_context_u32(p+(size_t)objects*40+(size_t)i*8+4) & ~3u) return 0;
    return 1;
}
#endif
