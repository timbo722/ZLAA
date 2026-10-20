/*
 * gpu.h - the GPU (an NVIDIA Maxwell "GM20B" in the Switch), as nvdrv sees it.
 *
 * For now this is a *null GPU*: it keeps the GPU's address space and its syncpoints
 * exactly, accepts command lists, but draws nothing. Each submission's fence completes
 * at once, so a game that waits for the GPU every frame keeps running. Phase 4 replaces
 * gpu_submit() with a real command processor and a Vulkan renderer.
 *
 * Address space: the GPU sees memory through its own virtual addresses ("GPU VA"),
 * which the game maps onto its memory with nvhost-as-gpu. gpu_translate() turns a GPU VA
 * into a guest address.
 *
 * Syncpoints: 32-bit counters the GPU increments as it finishes work. A fence is
 * (syncpoint id, value): "done when the counter reaches value". `max` is the value the
 * last submitted work will reach; `min` is what the GPU has actually reached.
 */
#ifndef GPU_H
#define GPU_H

#include <stdint.h>

#define GPU_MAX_SYNCPOINTS 192

typedef struct { uint32_t id, value; } GpuFence;

/* ---- address space ---- */
/* Reserve a free range of GPU VA (page-aligned) and return its start, or 0. */
uint64_t gpu_va_alloc(uint64_t size, uint64_t page_size);
/* Map [va, va+size) to guest memory at guest_addr (replaces what was there). */
void gpu_map(uint64_t va, uint64_t guest_addr, uint64_t size);
void gpu_unmap(uint64_t va, uint64_t size);
/* Guest address for a GPU VA, or 0 if it isn't mapped. */
uint64_t gpu_translate(uint64_t va);
/* The address-space layout the game asked for (ALLOC_AS_EX), or defaults. */
void gpu_va_set_layout(uint64_t start, uint64_t split, uint64_t end, uint32_t big_page_size);
void gpu_va_get_layout(uint64_t *start, uint64_t *split, uint64_t *end, uint32_t *big_page_size);

/* ---- syncpoints ---- */
uint32_t gpu_syncpoint_alloc(void);
uint32_t gpu_syncpoint_read(uint32_t id);        /* min: reached so far */
uint32_t gpu_syncpoint_read_max(uint32_t id);    /* max: will reach after queued work */
/* Has the GPU reached the fence's value? (Counters wrap, so compare the difference.) */
int gpu_fence_reached(GpuFence f);

/* ---- command submission ---- */
/* Run `count` GPFIFO entries (each 64 bits: address and size of a command list) on a
 * channel, then increment its syncpoint by `increments`. Returns the fence to wait on. */
GpuFence gpu_submit(uint32_t syncpoint, const uint64_t *entries, uint32_t count, uint32_t increments);

#endif
