/* SPDX-License-Identifier: MIT */
#ifndef KOBOX2_GPU_SYNC_FILE_H
#define KOBOX2_GPU_SYNC_FILE_H
#include <kobox2/gpu.h>

/* Private owner-control values, never a Linux FD or dma_fence pointer.
 * Native capability admission belongs to gpud before constructing a request. */
typedef struct kb2_gpu_sync_file_request {
    uint32_t opcode;
    uint64_t token, flags, session_id, sync_file_token;
} kb2_gpu_sync_file_request_t;

static inline uint64_t kb2_gpu_sync_file_load(const uint8_t *p)
{
    uint64_t value=0;
    for(unsigned i=0;i<8;++i) value|=(uint64_t)p[i]<<(i*8);
    return value;
}
static inline void kb2_gpu_sync_file_store(uint8_t *p,uint64_t value)
{ for(unsigned i=0;i<8;++i) p[i]=(uint8_t)(value>>(i*8)); }
static inline size_t kb2_gpu_sync_file_request_size(uint32_t op)
{
    switch(op) {
    case KB2_GPU_OPCODE_DMA_BUF_EXPORT_SYNC_FILE:return KB2_GPU_DMA_BUF_EXPORT_SYNC_FILE_REQUEST_SIZE;
    case KB2_GPU_OPCODE_DMA_BUF_IMPORT_SYNC_FILE:return KB2_GPU_DMA_BUF_IMPORT_SYNC_FILE_REQUEST_SIZE;
    case KB2_GPU_OPCODE_SYNC_FILE_RELEASE:return KB2_GPU_SYNC_FILE_RELEASE_REQUEST_SIZE;
    default:return 0;
    }
}
static inline size_t kb2_gpu_sync_file_response_size(uint32_t op)
{
    switch(op) {
    case KB2_GPU_OPCODE_DMA_BUF_EXPORT_SYNC_FILE:return KB2_GPU_DMA_BUF_EXPORT_SYNC_FILE_RESPONSE_SIZE;
    case KB2_GPU_OPCODE_DMA_BUF_IMPORT_SYNC_FILE:return KB2_GPU_DMA_BUF_IMPORT_SYNC_FILE_RESPONSE_SIZE;
    case KB2_GPU_OPCODE_SYNC_FILE_RELEASE:return KB2_GPU_SYNC_FILE_RELEASE_RESPONSE_SIZE;
    default:return 0;
    }
}
static inline int kb2_gpu_sync_file_request_valid(const kb2_gpu_sync_file_request_t *r)
{
    if(!r || !r->token) return 0;
    switch(r->opcode) {
    case KB2_GPU_OPCODE_DMA_BUF_EXPORT_SYNC_FILE:return r->session_id && !r->sync_file_token;
    case KB2_GPU_OPCODE_DMA_BUF_IMPORT_SYNC_FILE:return r->sync_file_token && !r->session_id;
    case KB2_GPU_OPCODE_SYNC_FILE_RELEASE:return !r->flags && !r->session_id && !r->sync_file_token;
    default:return 0;
    }
}
static inline kb2_protocol_status_t kb2_gpu_sync_file_request_encode(
    uint8_t *p,size_t size,const kb2_gpu_sync_file_request_t *r)
{
    if(!p || !kb2_gpu_sync_file_request_valid(r) ||
        size!=kb2_gpu_sync_file_request_size(r->opcode)) return KB2_PROTOCOL_MALFORMED;
    for(size_t i=0;i<size;++i) p[i]=0;
    kb2_gpu_sync_file_store(p,r->token);
    if(r->opcode==KB2_GPU_OPCODE_DMA_BUF_EXPORT_SYNC_FILE) {
        kb2_gpu_sync_file_store(p+8,r->flags);kb2_gpu_sync_file_store(p+16,r->session_id);
    } else if(r->opcode==KB2_GPU_OPCODE_DMA_BUF_IMPORT_SYNC_FILE) {
        kb2_gpu_sync_file_store(p+8,r->sync_file_token);kb2_gpu_sync_file_store(p+16,r->flags);
    }
    return KB2_PROTOCOL_OK;
}
static inline kb2_protocol_status_t kb2_gpu_sync_file_request_decode(
    const uint8_t *p,size_t size,uint32_t op,kb2_gpu_sync_file_request_t *out)
{
    const size_t expected=kb2_gpu_sync_file_request_size(op);
    if(!p || !out || !expected || size!=expected) return KB2_PROTOCOL_MALFORMED;
    kb2_gpu_sync_file_request_t r={.opcode=op,.token=kb2_gpu_sync_file_load(p)};
    if(op==KB2_GPU_OPCODE_DMA_BUF_EXPORT_SYNC_FILE) {
        if(kb2_gpu_sync_file_load(p+24)) return KB2_PROTOCOL_MALFORMED;
        r.flags=kb2_gpu_sync_file_load(p+8);r.session_id=kb2_gpu_sync_file_load(p+16);
    } else if(op==KB2_GPU_OPCODE_DMA_BUF_IMPORT_SYNC_FILE) {
        r.sync_file_token=kb2_gpu_sync_file_load(p+8);r.flags=kb2_gpu_sync_file_load(p+16);
    }
    if(!kb2_gpu_sync_file_request_valid(&r)) return KB2_PROTOCOL_MALFORMED;
    *out=r;return KB2_PROTOCOL_OK;
}
static inline kb2_protocol_status_t kb2_gpu_sync_file_response_encode(uint8_t *p,
    size_t size,const kb2_gpu_sync_file_request_t *r,int result,uint64_t exported)
{
    if(!p || !kb2_gpu_sync_file_request_valid(r) ||
        size!=kb2_gpu_sync_file_response_size(r->opcode) || result>0 || result< -4095 ||
        (r->opcode==KB2_GPU_OPCODE_DMA_BUF_EXPORT_SYNC_FILE ?
            (result ? exported!=0:exported==0) : exported!=0)) return KB2_PROTOCOL_MALFORMED;
    for(size_t i=0;i<size;++i) p[i]=0;
    kb2_gpu_sync_file_store(p,r->token);
    if(r->opcode==KB2_GPU_OPCODE_DMA_BUF_EXPORT_SYNC_FILE) {
        kb2_gpu_sync_file_store(p+8,r->flags);kb2_gpu_sync_file_store(p+16,r->session_id);
        kb2_gpu_sync_file_store(p+24,exported);
    } else if(r->opcode==KB2_GPU_OPCODE_DMA_BUF_IMPORT_SYNC_FILE) {
        kb2_gpu_sync_file_store(p+8,r->sync_file_token);kb2_gpu_sync_file_store(p+16,r->flags);
    }
    kb2_gpu_sync_file_store(p+size-8,(uint64_t)(int64_t)result);
    return KB2_PROTOCOL_OK;
}
static inline kb2_protocol_status_t kb2_gpu_sync_file_response_decode(const uint8_t *p,
    size_t size,const kb2_gpu_sync_file_request_t *r,int *result,uint64_t *exported)
{
    if(!p || !result || !exported || !kb2_gpu_sync_file_request_valid(r) ||
        size!=kb2_gpu_sync_file_response_size(r->opcode) || kb2_gpu_sync_file_load(p)!=r->token)
        return KB2_PROTOCOL_MALFORMED;
    uint64_t token=0;
    if(r->opcode==KB2_GPU_OPCODE_DMA_BUF_EXPORT_SYNC_FILE) {
        if(kb2_gpu_sync_file_load(p+8)!=r->flags || kb2_gpu_sync_file_load(p+16)!=r->session_id)
            return KB2_PROTOCOL_MALFORMED;
        token=kb2_gpu_sync_file_load(p+24);
    } else if(r->opcode==KB2_GPU_OPCODE_DMA_BUF_IMPORT_SYNC_FILE &&
        (kb2_gpu_sync_file_load(p+8)!=r->sync_file_token || kb2_gpu_sync_file_load(p+16)!=r->flags))
        return KB2_PROTOCOL_MALFORMED;
    const int64_t status=(int64_t)kb2_gpu_sync_file_load(p+size-8);
    if(status>0 || status< -4095 || (r->opcode==KB2_GPU_OPCODE_DMA_BUF_EXPORT_SYNC_FILE &&
        (status ? token!=0:token==0))) return KB2_PROTOCOL_MALFORMED;
    *result=(int)status;*exported=token;return KB2_PROTOCOL_OK;
}
#endif
