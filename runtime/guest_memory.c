/*
 * guest_memory.c - reserve the guest address space and change access rights in it.
 * Uses POSIX mmap/mprotect, which Linux and Android both have.
 */
#define _GNU_SOURCE          /* mremap */
#include "guest_memory.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

uint8_t *guest_mem;
uint64_t guest_mem_size;

int gmem_reserve(void) {
    /* PROT_NONE + MAP_NORESERVE: address space only, no RAM or swap is set aside.
     * Touching a page we haven't opened up crashes immediately, which is what we want:
     * it means the game used an address it was never given. */
    void *p = mmap(NULL, GUEST_SPACE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        perror("gmem_reserve: mmap");
        return -1;
    }
    guest_mem = p;
    guest_mem_size = GUEST_SPACE_SIZE;
    return 0;
}

int gmem_protect(uint64_t addr, uint64_t size, int perm) {
    /* The host page can be bigger than the Switch's 4 KiB (some Android kernels use 16 KiB).
     * mprotect works on whole host pages, so round outward; callers apply the most
     * permissive range last so it wins where two ranges share a host page. */
    uint64_t hp = (uint64_t)sysconf(_SC_PAGESIZE);
    uint64_t start = addr & ~(hp - 1), end = (addr + size + hp - 1) & ~(hp - 1);
    if (end > guest_mem_size || end < start) {
        fprintf(stderr, "gmem_protect: 0x%llx + 0x%llx is outside guest memory\n",
                (unsigned long long)addr, (unsigned long long)size);
        return -1;
    }
    int prot = PROT_NONE;
    if (perm & GMEM_READ) prot |= PROT_READ;
    if (perm & GMEM_WRITE) prot |= PROT_READ | PROT_WRITE;
    if (mprotect(guest_mem + start, end - start, prot) != 0) {
        perror("gmem_protect: mprotect");
        return -1;
    }
    return 0;
}

int gmem_decommit(uint64_t addr, uint64_t size) {
    /* Mapping fresh anonymous memory over the range drops the old pages (and their RAM).
     * Only whole host pages can be dropped: on a host with pages bigger than 4 KiB, an edge
     * page shared with a neighbouring mapping is left as it is. 4 KiB-page hosts are exact. */
    uint64_t hp = (uint64_t)sysconf(_SC_PAGESIZE);
    uint64_t start = (addr + hp - 1) & ~(hp - 1), end = (addr + size) & ~(hp - 1);
    if (addr + size > guest_mem_size || addr + size < addr) return -1;
    if (end <= start) return 0;
    void *p = mmap(guest_mem + start, end - start, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        perror("gmem_decommit: mmap");
        return -1;
    }
    return 0;
}

int gmem_map_fd(uint64_t addr, uint64_t size, int fd, int writable) {
    if (addr + size > guest_mem_size || addr + size < addr) return -1;
    void *p = mmap(guest_mem + addr, size, PROT_READ | (writable ? PROT_WRITE : 0), MAP_SHARED | MAP_FIXED, fd, 0);
    if (p == MAP_FAILED) {
        perror("gmem_map_fd: mmap");
        return -1;
    }
    return 0;
}

int gmem_move(uint64_t dst, uint64_t src, uint64_t size) {
    uint64_t hp = (uint64_t)sysconf(_SC_PAGESIZE);
    if (dst + size > guest_mem_size || src + size > guest_mem_size || dst + size < dst || src + size < src)
        return -1;
    if (((dst | src | size) & (hp - 1)) == 0) {
        /* mremap moves the pages themselves: no copying, and untouched pages stay free */
        void *p = mremap(guest_mem + src, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, guest_mem + dst);
        if (p == MAP_FAILED) {
            perror("gmem_move: mremap");
            return -1;
        }
        /* mremap left a hole at src; fill it back in with reserved, no-access memory */
        p = mmap(guest_mem + src, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
        return p == MAP_FAILED ? -1 : 0;
    }
    /* Host pages bigger than 4 KiB: copy instead */
    if (gmem_protect(dst, size, GMEM_READ | GMEM_WRITE) != 0) return -1;
    memcpy(guest_mem + dst, guest_mem + src, size);
    return gmem_decommit(src, size);
}
