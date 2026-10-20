/*
 * guest_memory.h - the guest's (Switch game's) address space.
 *
 * Generated code reads guest address `a` at c->mem + a, so guest memory has to be one
 * contiguous block of host memory starting at guest address 0. We reserve a large block
 * up front with no access allowed, then open up only the parts the game is given
 * (its modules now; heap, stacks, etc. once the OS services exist).
 * Reserving costs no RAM: the host OS only provides a page when it is first touched.
 */
#ifndef GUEST_MEMORY_H
#define GUEST_MEMORY_H

#include <stdint.h>

/* 64 GiB of guest address space. Everything the game gets lives below this, because
 * we choose every address the "kernel" hands out. */
#define GUEST_SPACE_SIZE (1ull << 36)
#define GUEST_PAGE 0x1000ull

/* Access rights, as in Horizon's MemoryPermission */
#define GMEM_NONE  0
#define GMEM_READ  1
#define GMEM_WRITE 2

extern uint8_t *guest_mem;        /* host address of guest address 0 */
extern uint64_t guest_mem_size;

int gmem_reserve(void);                                          /* 0 on success */
int gmem_protect(uint64_t addr, uint64_t size, int perm);        /* 0 on success */
int gmem_decommit(uint64_t addr, uint64_t size);   /* give the RAM back; reads 0 when mapped again */
/* Map a shared-memory file (memfd) at addr: the same RAM is then visible there and wherever
 * else the file is mapped (e.g. by a service). Undo with gmem_decommit. */
int gmem_map_fd(uint64_t addr, uint64_t size, int fd, int writable);
/* Move the pages at src to dst (contents included); src is left reserved with no access. */
int gmem_move(uint64_t dst, uint64_t src, uint64_t size);

#endif
