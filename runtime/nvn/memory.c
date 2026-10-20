/*
 * memory.c - NVNmemoryPool and NVNbuffer.
 *
 * A memory pool is a block of the game's own memory that the GPU may use. A buffer is a range
 * inside a pool. Because GPU addresses are guest addresses here, "mapping" a pool or buffer
 * just returns where it already is.
 */
#include "nvn.h"

static uint64_t next_debug_id = 1;

/* ---- memory pool ---- */

#define MPB ((MemoryPoolBuilder *)gptr(ARG(0)))
static void nvnMemoryPoolBuilderSetDevice(CPU *c)   { MPB->device = ARG(1); }
static void nvnMemoryPoolBuilderSetDefaults(CPU *c) {
    uint64_t device = MPB->device;
    /* default flags: CPU_UNCACHED | GPU_CACHED */
    *MPB = (MemoryPoolBuilder){ .device = device, .flags = 0x9 };
}
static void nvnMemoryPoolBuilderSetFlags(CPU *c)    { MPB->flags = (uint32_t)ARG(1); }
static void nvnMemoryPoolBuilderSetStorage(CPU *c)  { MPB->storage = ARG(1); MPB->size = ARG(2); }
static void nvnMemoryPoolBuilderGetDevice(CPU *c)   { RET(MPB->device); }
static void nvnMemoryPoolBuilderGetFlags(CPU *c)    { RET(MPB->flags); }
static void nvnMemoryPoolBuilderGetMemory(CPU *c)   { RET(MPB->storage); }
static void nvnMemoryPoolBuilderGetSize(CPU *c)     { RET(MPB->size); }

static void nvnMemoryPoolInitialize(CPU *c) {
    MemoryPoolBuilder *b = gptr(ARG(1));
    *(MemoryPool *)gptr(ARG(0)) = (MemoryPool){ b->storage, b->size, b->flags };
    RET(1);
}
#define MP ((MemoryPool *)gptr(ARG(0)))
static void nvnMemoryPoolMap(CPU *c)              { RET(MP->storage); }
static void nvnMemoryPoolGetBufferAddress(CPU *c) { RET(MP->storage); }
static void nvnMemoryPoolGetSize(CPU *c)          { RET(MP->size); }
static void nvnMemoryPoolGetFlags(CPU *c)         { RET(MP->flags); }

/* ---- buffer ---- */

#define BB ((BufferBuilder *)gptr(ARG(0)))
static void nvnBufferBuilderSetDevice(CPU *c)       { BB->device = ARG(1); }
static void nvnBufferBuilderSetDefaults(CPU *c)     { uint64_t d = BB->device; *BB = (BufferBuilder){ .device = d }; }
/* (builder, pool, offset, size) */
static void nvnBufferBuilderSetStorage(CPU *c)      { BB->pool = ARG(1); BB->offset = ARG(2); BB->size = ARG(3); }
static void nvnBufferBuilderGetDevice(CPU *c)       { RET(BB->device); }
static void nvnBufferBuilderGetMemoryPool(CPU *c)   { RET(BB->pool); }
static void nvnBufferBuilderGetMemoryOffset(CPU *c) { RET(BB->offset); }
static void nvnBufferBuilderGetSize(CPU *c)         { RET(BB->size); }

static void nvnBufferInitialize(CPU *c) {
    BufferBuilder *b = gptr(ARG(1));
    *(Buffer *)gptr(ARG(0)) = (Buffer){ pool_address(b->pool, b->offset), b->pool, b->offset, b->size, next_debug_id++ };
    RET(1);
}
#define BUF ((Buffer *)gptr(ARG(0)))
static void nvnBufferMap(CPU *c)             { RET(BUF->address); }
static void nvnBufferGetAddress(CPU *c)      { RET(BUF->address); }
static void nvnBufferGetMemoryPool(CPU *c)   { RET(BUF->pool); }
static void nvnBufferGetMemoryOffset(CPU *c) { RET(BUF->offset); }
static void nvnBufferGetSize(CPU *c)         { RET(BUF->size); }
static void nvnBufferGetDebugID(CPU *c)      { RET(BUF->debug_id); }

const NvnEntry nvn_memory_functions[] = {
    NVN_ENTRY(nvnMemoryPoolBuilderSetDevice), NVN_ENTRY(nvnMemoryPoolBuilderSetDefaults),
    NVN_ENTRY(nvnMemoryPoolBuilderSetFlags), NVN_ENTRY(nvnMemoryPoolBuilderSetStorage),
    NVN_ENTRY(nvnMemoryPoolBuilderGetDevice), NVN_ENTRY(nvnMemoryPoolBuilderGetFlags),
    NVN_ENTRY(nvnMemoryPoolBuilderGetMemory), NVN_ENTRY(nvnMemoryPoolBuilderGetSize),
    NVN_ENTRY(nvnMemoryPoolInitialize), { "nvnMemoryPoolFinalize", nvn_nothing },
    { "nvnMemoryPoolSetDebugLabel", nvn_nothing },
    NVN_ENTRY(nvnMemoryPoolMap), NVN_ENTRY(nvnMemoryPoolGetBufferAddress),
    NVN_ENTRY(nvnMemoryPoolGetSize), NVN_ENTRY(nvnMemoryPoolGetFlags),
    { "nvnMemoryPoolFlushMappedRange", nvn_nothing }, { "nvnMemoryPoolInvalidateMappedRange", nvn_nothing },
    { "nvnMemoryPoolMapVirtual", nvn_true },

    NVN_ENTRY(nvnBufferBuilderSetDevice), NVN_ENTRY(nvnBufferBuilderSetDefaults), NVN_ENTRY(nvnBufferBuilderSetStorage),
    NVN_ENTRY(nvnBufferBuilderGetDevice), NVN_ENTRY(nvnBufferBuilderGetMemoryPool),
    NVN_ENTRY(nvnBufferBuilderGetMemoryOffset), NVN_ENTRY(nvnBufferBuilderGetSize),
    NVN_ENTRY(nvnBufferInitialize), { "nvnBufferFinalize", nvn_nothing }, { "nvnBufferSetDebugLabel", nvn_nothing },
    NVN_ENTRY(nvnBufferMap), NVN_ENTRY(nvnBufferGetAddress), NVN_ENTRY(nvnBufferGetMemoryPool),
    NVN_ENTRY(nvnBufferGetMemoryOffset), NVN_ENTRY(nvnBufferGetSize), NVN_ENTRY(nvnBufferGetDebugID),
    { "nvnBufferFlushMappedRange", nvn_nothing }, { "nvnBufferInvalidateMappedRange", nvn_nothing },
    { NULL, NULL },
};
