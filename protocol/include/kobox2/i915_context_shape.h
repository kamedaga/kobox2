/* SPDX-License-Identifier: MIT */
#ifndef KOBOX2_I915_CONTEXT_SHAPE_H
#define KOBOX2_I915_CONTEXT_SHAPE_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif
#include <kobox2/gpu_drm_i915_layout.h>

static inline uint32_t kb2_i915_context_u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static inline uint64_t kb2_i915_context_u64(const unsigned char *p)
{ return kb2_i915_context_u32(p) | (uint64_t)kb2_i915_context_u32(p+4)<<32; }
static inline int kb2_i915_context_scalar(uint64_t parameter)
{
    switch (parameter) {
    case 4: case 5: case 6: case 8: case 9: case 11: case 12: case 13: case 14:
        return 1;
    default: return 0;
    }
}
static inline int kb2_i915_context_command(uint32_t command)
{
    return command >= KB2_GPU_DRM_I915_COMMAND_CONTEXT_CREATE &&
        command <= KB2_GPU_DRM_I915_COMMAND_CONTEXT_DESTROY;
}
static inline int kb2_i915_context_header(uint32_t command,
    const void *input, size_t bytes, size_t *aux_bytes)
{
    const unsigned char *p = input;
    if (!p || !kb2_i915_context_command(command)) return 0;
    if (command == KB2_GPU_DRM_I915_COMMAND_CONTEXT_DESTROY) {
        if (bytes != 8 || kb2_i915_context_u32(p+4)) return 0;
        if (aux_bytes) *aux_bytes = 0;
        return 1;
    }
    if (bytes != 16 || kb2_i915_context_u32(p+12)) return 0;
    const uint32_t params = kb2_i915_context_u32(p+4);
    const uint32_t engines = kb2_i915_context_u32(p+8);
    if (params > KB2_GPU_DRM_I915_MAX_CONTEXT_PARAMS ||
        engines > KB2_GPU_DRM_I915_MAX_CONTEXT_ENGINES || (!params && engines)) return 0;
    if (command == KB2_GPU_DRM_I915_COMMAND_CONTEXT_CREATE) {
        const uint32_t flags = kb2_i915_context_u32(p);
        if ((flags & ~3u) || (params && !(flags & 1u))) return 0;
    } else if (params != 1) return 0;
    if (aux_bytes) *aux_bytes = (size_t)params * 32 + (size_t)engines * 4;
    return 1;
}
/* A canonical parameter stream has no peer pointers, no overlap and no
 * unreferenced engines. Each engine list immediately follows its predecessor. */
static inline int kb2_i915_context_params(const void *data, size_t bytes,
    uint32_t params, uint32_t engines)
{
    const unsigned char *p = data;
    uint32_t next = 0;
    if (params > KB2_GPU_DRM_I915_MAX_CONTEXT_PARAMS ||
        engines > KB2_GPU_DRM_I915_MAX_CONTEXT_ENGINES ||
        bytes != (size_t)params * 32 + (size_t)engines * 4 || (!data && bytes)) return 0;
    for (uint32_t i = 0; i < params; ++i) {
        const unsigned char *q = p + (size_t)i * 32;
        const uint64_t parameter = kb2_i915_context_u64(q);
        const uint64_t value = kb2_i915_context_u64(q+8);
        const uint32_t size = kb2_i915_context_u32(q+16);
        const uint32_t first = kb2_i915_context_u32(q+20);
        const uint32_t count = kb2_i915_context_u32(q+24);
        if (kb2_i915_context_u32(q+28)) return 0;
        if (parameter == 10) {
            if (value || count > KB2_GPU_DRM_I915_MAX_CONTEXT_ENGINES_PER_PARAM) return 0;
            if (!size) { if (first || count) return 0; continue; }
            if (size != 8u + count * 4u || first != next || count > engines - next) return 0;
            next += count;
        } else if (!kb2_i915_context_scalar(parameter) || size || first || count) return 0;
    }
    return next == engines;
}
#endif
