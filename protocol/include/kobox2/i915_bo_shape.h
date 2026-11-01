/* SPDX-License-Identifier: MIT */
#ifndef KOBOX2_I915_BO_SHAPE_H
#define KOBOX2_I915_BO_SHAPE_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif
#include <kobox2/gpu_drm_i915_layout.h>

struct kb2_i915_bo_shape {
    uint32_t command, ioctl, record;
    size_t bytes, result_bytes;
};

/* Every admitted record is a scalar UAPI. Extensions, user pointers and mmap
 * address spaces use separate typed commands and cannot enter this table. */
static inline int kb2_i915_bo_shape_command(uint32_t command,
    struct kb2_i915_bo_shape *out)
{
    struct kb2_i915_bo_shape shape = {.command = command};
#define BO_SHAPE(name, request, result) \
    case KB2_GPU_DRM_I915_COMMAND_##name: \
        shape.ioctl = request; shape.record = KB2_GPU_DRM_I915_RECORD_##name; \
        shape.bytes = KB2_GPU_DRM_I915_RECORD_##name##_SIZE; \
        shape.result_bytes = result ? shape.bytes : 0; break
    switch (command) {
    BO_SHAPE(GEM_CREATE, 0xc010645bu, 1);
    BO_SHAPE(GEM_BUSY, 0xc0086457u, 1);
    BO_SHAPE(GEM_MADVISE, 0xc00c6466u, 1);
    BO_SHAPE(GEM_WAIT, 0xc010646cu, 1);
    BO_SHAPE(GEM_SET_DOMAIN, 0x400c645fu, 0);
    BO_SHAPE(GEM_SET_CACHING, 0x4008646fu, 0);
    default: return 0;
    }
#undef BO_SHAPE
    if (command == KB2_GPU_DRM_I915_COMMAND_GEM_WAIT)
        shape.result_bytes = KB2_GPU_DRM_I915_RECORD_GEM_WAIT_RESULT_SIZE;
    if (out) *out = shape;
    return 1;
}
static inline int kb2_i915_bo_shape_ioctl(uint32_t request,
    struct kb2_i915_bo_shape *out)
{
    for (uint32_t command = KB2_GPU_DRM_I915_COMMAND_GEM_CREATE;
         command <= KB2_GPU_DRM_I915_COMMAND_GEM_SET_CACHING; ++command) {
        struct kb2_i915_bo_shape shape;
        if (kb2_i915_bo_shape_command(command, &shape) && shape.ioctl == request) {
            if (out) *out = shape;
            return 1;
        }
    }
    return 0;
}
static inline uint32_t kb2_i915_bo_u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static inline int kb2_i915_bo_input_valid(uint32_t command, const void *input, size_t bytes)
{
    struct kb2_i915_bo_shape shape;
    const unsigned char *p = input;
    if (!p || !kb2_i915_bo_shape_command(command, &shape) || bytes != shape.bytes)
        return 0;
    switch (command) {
    case KB2_GPU_DRM_I915_COMMAND_GEM_CREATE:
        return !kb2_i915_bo_u32(p + 8) && !kb2_i915_bo_u32(p + 12);
    case KB2_GPU_DRM_I915_COMMAND_GEM_BUSY:
        return !kb2_i915_bo_u32(p + 4);
    case KB2_GPU_DRM_I915_COMMAND_GEM_MADVISE:
        return kb2_i915_bo_u32(p + 4) <= 1 && !kb2_i915_bo_u32(p + 8);
    case KB2_GPU_DRM_I915_COMMAND_GEM_WAIT:
        return !kb2_i915_bo_u32(p + 4);
    case KB2_GPU_DRM_I915_COMMAND_GEM_SET_DOMAIN:
        return !(kb2_i915_bo_u32(p + 4) & ~0xffu) &&
            !(kb2_i915_bo_u32(p + 8) & ~0xffu);
    case KB2_GPU_DRM_I915_COMMAND_GEM_SET_CACHING:
        return kb2_i915_bo_u32(p + 4) <= 2;
    default: return 0;
    }
}
#endif
