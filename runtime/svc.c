/*
 * svc.c - the SVCs (system calls) the game makes, answered in C.
 *
 * The recompiled SVC instruction calls recomp_svc(c, pc, number). Arguments arrive in
 * X0..X7 as on hardware; the result code goes back in W0 and outputs in X1, X2...
 * Calling conventions: switchbrew.org/wiki/SVC. Each one we haven't written yet stops
 * the program with a message, so it's obvious what to implement next.
 */
#include "kernel.h"
#include "guest_memory.h"
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

typedef void (*SvcHandler)(CPU *c);

static void ret(CPU *c, uint32_t result) { c->x[0] = result; }

/* 0x01 SetHeapSize(size) -> addr. The heap is one block that grows or shrinks at its end. */
static void svc_set_heap_size(CPU *c) {
    uint64_t size = c->x[1];
    if (size & 0x1FFFFF || size > HEAP_SIZE) { ret(c, R_INVALID_SIZE); return; }   /* 2 MiB units */
    int r = 0;
    if (size > kproc.heap_size)
        r = kmem_map(HEAP_BASE + kproc.heap_size, size - kproc.heap_size, MEM_HEAP, PERM_RW);
    else if (size < kproc.heap_size)
        r = kmem_map(HEAP_BASE + size, kproc.heap_size - size, MEM_FREE, PERM_NONE);
    if (r != 0) { ret(c, R_OUT_OF_RANGE); return; }
    kproc.heap_size = size;
    c->x[1] = HEAP_BASE;
    ret(c, R_SUCCESS);
}

/* 0x03 SetMemoryAttribute(addr, size, mask, value): e.g. mark memory uncached for the GPU.
 * Our "GPU" shares the host's cache-coherent memory, so there is nothing to change. */
static void svc_set_memory_attribute(CPU *c) {
    uint64_t addr = c->x[0], size = c->x[1];
    if ((addr | size) & (GUEST_PAGE - 1) || size == 0) { ret(c, R_INVALID_ADDRESS); return; }
    ret(c, R_SUCCESS);
}

/* 0x04 MapMemory(dst, src, size): make heap memory (or a module's static data) appear at
 * dst, in the stack or alias region; e.g. for a thread stack. The source keeps its state
 * but can't be used until it's unmapped. */
static void svc_map_memory(CPU *c) {
    uint64_t dst = c->x[0], src = c->x[1], size = c->x[2];
    if ((dst | src | size) & (GUEST_PAGE - 1) || size == 0) { ret(c, R_INVALID_ADDRESS); return; }
    uint64_t b, sz;
    int state, perm;
    kmem_query(src, &b, &sz, &state, &perm);
    int src_state = state;   /* memory the kernel lets you alias: heap and module data */
    if ((state != MEM_HEAP && state != MEM_CODE_DATA) || perm != PERM_RW || src + size > b + sz) {
        ret(c, R_INVALID_CURRENT_MEM);
        return;
    }
    kmem_query(dst, &b, &sz, &state, &perm);
    if (state != MEM_FREE || dst + size > b + sz) { ret(c, R_INVALID_CURRENT_MEM); return; }
    int dst_state = dst >= STACK_BASE && dst < STACK_BASE + STACK_SIZE ? MEM_STACK : MEM_ALIAS;
    if (gmem_move(dst, src, size) != 0 || kmem_map(dst, size, dst_state, PERM_RW) != 0 ||
        kmem_map(src, size, src_state, PERM_NONE) != 0) {   /* same state, but locked */
        ret(c, R_OUT_OF_RANGE);
        return;
    }
    ret(c, R_SUCCESS);
}

/* 0x05 UnmapMemory(dst, src, size): undo MapMemory; the data goes back to the heap */
static void svc_unmap_memory(CPU *c) {
    uint64_t dst = c->x[0], src = c->x[1], size = c->x[2];
    if ((dst | src | size) & (GUEST_PAGE - 1) || size == 0) { ret(c, R_INVALID_ADDRESS); return; }
    uint64_t b, sz;
    int state, perm;
    kmem_query(dst, &b, &sz, &state, &perm);
    if ((state != MEM_STACK && state != MEM_ALIAS) || dst + size > b + sz) { ret(c, R_INVALID_CURRENT_MEM); return; }
    kmem_query(src, &b, &sz, &state, &perm);
    if ((state != MEM_HEAP && state != MEM_CODE_DATA) || perm != PERM_NONE) { ret(c, R_INVALID_CURRENT_MEM); return; }
    if (gmem_move(src, dst, size) != 0 || kmem_map(src, size, state, PERM_RW) != 0 ||
        kmem_map(dst, size, MEM_FREE, PERM_NONE) != 0) {
        ret(c, R_OUT_OF_RANGE);
        return;
    }
    ret(c, R_SUCCESS);
}

/* 0x06 QueryMemory(MemoryInfo *out, -, addr) -> page info */
static void svc_query_memory(CPU *c) {
    uint64_t out = c->x[0], base, size;
    int state, perm;
    kmem_query(c->x[2], &base, &size, &state, &perm);
    /* MemoryInfo: addr(8) size(8) state(4) attribute(4) perm(4) ipc_refs(4) device_refs(4) pad(4) */
    st64(c, out + 0x00, base);
    st64(c, out + 0x08, size);
    st32(c, out + 0x10, (uint32_t)state);
    st32(c, out + 0x14, 0);
    st32(c, out + 0x18, (uint32_t)perm);
    st32(c, out + 0x1C, 0);
    st32(c, out + 0x20, 0);
    st32(c, out + 0x24, 0);
    c->x[1] = 0;
    ret(c, R_SUCCESS);
}

/* 0x07 ExitProcess(): the game is quitting */
static void svc_exit_process(CPU *c) {
    (void)c;
    printf("kernel: the game called ExitProcess\n");
    fflush(stdout);
    exit(0);
}

/* 0x13 MapSharedMemory(handle, addr, size, perm): the game picks a free address */
static void svc_map_shared_memory(CPU *c) {
    KHandle *h = khandle_get((uint32_t)c->x[0]);
    uint64_t addr = c->x[1], size = c->x[2];
    uint32_t perm = (uint32_t)c->x[3];
    if (!h || h->type != KOBJ_SHARED_MEMORY) { ret(c, R_INVALID_HANDLE); return; }
    KSharedMemory *shm = h->obj;
    if ((addr | size) & (GUEST_PAGE - 1) || size != shm->size) { ret(c, R_INVALID_SIZE); return; }
    uint64_t b, sz;
    int state, p;
    kmem_query(addr, &b, &sz, &state, &p);
    if (state != MEM_FREE || addr + size > b + sz) { ret(c, R_INVALID_CURRENT_MEM); return; }
    if (kmem_map(addr, size, MEM_SHARED, (int)perm) != 0 || gmem_map_fd(addr, size, shm->fd, perm & PERM_W) != 0) {
        ret(c, R_OUT_OF_RANGE);
        return;
    }
    ret(c, R_SUCCESS);
}

/* 0x14 UnmapSharedMemory(handle, addr, size) */
static void svc_unmap_shared_memory(CPU *c) {
    uint64_t addr = c->x[1], size = c->x[2];
    uint64_t b, sz;
    int state, p;
    kmem_query(addr, &b, &sz, &state, &p);
    if (state != MEM_SHARED) { ret(c, R_INVALID_CURRENT_MEM); return; }
    ret(c, kmem_map(addr, size, MEM_FREE, PERM_NONE) == 0 ? R_SUCCESS : R_OUT_OF_RANGE);
}

/* 0x15 CreateTransferMemory(-, addr, size, perm) -> handle */
static void svc_create_transfer_memory(CPU *c) {
    uint64_t addr = c->x[1], size = c->x[2];
    if ((addr | size) & (GUEST_PAGE - 1) || size == 0 || addr + size < addr || addr + size > guest_mem_size) {
        ret(c, R_INVALID_ADDRESS);
        return;
    }
    KTransferMemory *t = malloc(sizeof *t);
    uint32_t h = t ? khandle_new(KOBJ_TRANSFER_MEMORY, t) : 0;
    if (!h) { free(t); ret(c, R_OUT_OF_RANGE); return; }
    t->addr = addr;
    t->size = size;
    t->perm = (uint32_t)c->x[3];
    c->x[1] = h;
    ret(c, R_SUCCESS);
}

/* 0x16 CloseHandle(handle) */
static void svc_close_handle(CPU *c) {
    uint32_t handle = (uint32_t)c->x[0];
    KHandle *h = khandle_get(handle);
    if (!h) { ret(c, R_INVALID_HANDLE); return; }
    if (h->type == KOBJ_SESSION) ipc_close_session(h->obj);
    if (h->type == KOBJ_TRANSFER_MEMORY) free(h->obj);
    ret(c, khandle_close(handle) == 0 ? R_SUCCESS : R_INVALID_HANDLE);
}

/* 0x1E GetSystemTick() -> ticks of the 19.2 MHz counter */
static void svc_get_system_tick(CPU *c) { c->x[0] = recomp_cntpct(); }

/* 0x1F ConnectToNamedPort(-, name) -> session handle. The only named port is "sm:". */
static void svc_connect_to_named_port(CPU *c) {
    char name[12] = { 0 };
    uint64_t p = c->x[1];
    for (int k = 0; k < 11 && p + k < guest_mem_size && guest_mem[p + k]; k++) name[k] = (char)guest_mem[p + k];
    uint32_t h = 0;
    ret(c, ipc_connect_named_port(name, &h));
    c->x[1] = h;
}

/* 0x21 SendSyncRequest(session): the message is in this thread's TLS */
static void svc_send_sync_request(CPU *c) {
    ret(c, ipc_send_sync_request(c, (uint32_t)c->x[0], kthread_current()->tls));
}

/* 0x22 SendSyncRequestWithUserBuffer(buffer, size, session): the message is in the buffer */
static void svc_send_sync_request_user(CPU *c) {
    ret(c, ipc_send_sync_request(c, (uint32_t)c->x[2], c->x[0]));
}

/* 0x27 OutputDebugString(str, len): the game's debug log */
static void svc_output_debug_string(CPU *c) {
    uint64_t p = c->x[0], n = c->x[1];
    if (n > 0x10000) n = 0x10000;
    if (p < guest_mem_size && n <= guest_mem_size - p)
        fprintf(stderr, "[game] %.*s%s", (int)n, (const char *)guest_mem + p,
                n && guest_mem[p + n - 1] == '\n' ? "" : "\n");
    ret(c, R_SUCCESS);
}

/* 0x26 Break(reason, info, info_size): the game hit a fatal error (an assert, abort...) */
static void svc_break(CPU *c) {
    uint64_t reason = c->x[0];
    if (reason & 0x80000000) {          /* "notification only": tells a debugger, then continues */
        if (ktrace) fprintf(stderr, "svc: Break notification 0x%" PRIx64 " (ignored)\n", reason);
        ret(c, R_SUCCESS);
        return;
    }
    fprintf(stderr, "\nsvc: the game called Break: reason 0x%" PRIx64 ", info 0x%" PRIx64 " size 0x%" PRIx64 "\n",
            reason, c->x[1], c->x[2]);
    if (c->x[2] == 4 && c->x[1] < guest_mem_size) fprintf(stderr, "     info word: 0x%08" PRIx64 "\n", ld32(c, c->x[1]));
    kernel_dump_state();
    exit(1);
}

/* 0x29 GetInfo(-, id, handle, subid) -> value: facts about the process and system */
static void svc_get_info(CPU *c) {
    uint32_t id = (uint32_t)c->x[1];
    uint64_t sub = c->x[3], v;
    switch (id) {
    case 0:  v = 0xF; break;                                    /* CoreMask: cores 0-3 */
    case 1:  v = 0xFFFFFFFFull << 28; break;                     /* PriorityMask: priorities 28..59 */
    case 2:  v = ALIAS_BASE; break;                              /* AliasRegionAddress */
    case 3:  v = ALIAS_SIZE; break;
    case 4:  v = HEAP_BASE; break;                               /* HeapRegionAddress */
    case 5:  v = HEAP_SIZE; break;
    case 6:  v = 0xCD500000ull; break;                           /* TotalMemorySize: ~3.2 GiB, an application's share */
    case 7:  v = kproc.heap_size + kproc.main_stack_size; break; /* UsedMemorySize (approximately) */
    case 8:  v = 0; break;                                       /* IsDebuggerAttached */
    case 11:                                                     /* RandomEntropy[sub] */
        if (sub > 3) { ret(c, R_OUT_OF_RANGE); return; }
        v = kproc.random_entropy[sub];
        break;
    case 12: v = ASPACE_BASE; break;                             /* AslrRegionAddress */
    case 13: v = ASPACE_END - ASPACE_BASE; break;
    case 14: v = STACK_BASE; break;                              /* StackRegionAddress */
    case 15: v = STACK_SIZE; break;
    case 16: case 17: v = 0; break;                              /* SystemResourceSize total/used: none */
    case 18: v = kproc.program_id; break;                        /* ProgramId */
    case 20: v = kproc.user_exception_ctx; break;                /* UserExceptionContextAddress */
    case 21: v = 0xCD500000ull; break;                           /* TotalNonSystemMemorySize */
    case 22: v = kproc.heap_size + kproc.main_stack_size; break; /* UsedNonSystemMemorySize */
    case 23: v = 1; break;                                       /* IsApplication */
    default:
        fprintf(stderr, "svc: GetInfo id %u sub 0x%" PRIx64 " not implemented\n", id, sub);
        ret(c, R_INVALID_ENUM);
        return;
    }
    c->x[1] = v;
    ret(c, R_SUCCESS);
}

static const struct { SvcHandler fn; const char *name; } svcs[0x80] = {
    [0x01] = { svc_set_heap_size, "SetHeapSize" },
    [0x03] = { svc_set_memory_attribute, "SetMemoryAttribute" },
    [0x04] = { svc_map_memory, "MapMemory" },
    [0x05] = { svc_unmap_memory, "UnmapMemory" },
    [0x06] = { svc_query_memory, "QueryMemory" },
    [0x07] = { svc_exit_process, "ExitProcess" },
    [0x08] = { svc_create_thread, "CreateThread" },
    [0x09] = { svc_start_thread, "StartThread" },
    [0x0A] = { svc_exit_thread, "ExitThread" },
    [0x0B] = { svc_sleep_thread, "SleepThread" },
    [0x0C] = { svc_get_thread_priority, "GetThreadPriority" },
    [0x0D] = { svc_set_thread_priority, "SetThreadPriority" },
    [0x0E] = { svc_get_thread_core_mask, "GetThreadCoreMask" },
    [0x0F] = { svc_set_thread_core_mask, "SetThreadCoreMask" },
    [0x10] = { svc_get_current_processor_number, "GetCurrentProcessorNumber" },
    [0x11] = { svc_signal_event, "SignalEvent" },
    [0x12] = { svc_clear_event, "ClearEvent" },
    [0x13] = { svc_map_shared_memory, "MapSharedMemory" },
    [0x14] = { svc_unmap_shared_memory, "UnmapSharedMemory" },
    [0x15] = { svc_create_transfer_memory, "CreateTransferMemory" },
    [0x16] = { svc_close_handle, "CloseHandle" },
    [0x17] = { svc_reset_signal, "ResetSignal" },
    [0x18] = { svc_wait_synchronization, "WaitSynchronization" },
    [0x19] = { svc_cancel_synchronization, "CancelSynchronization" },
    [0x1A] = { svc_arbitrate_lock, "ArbitrateLock" },
    [0x1B] = { svc_arbitrate_unlock, "ArbitrateUnlock" },
    [0x1C] = { svc_wait_process_wide_key_atomic, "WaitProcessWideKeyAtomic" },
    [0x1D] = { svc_signal_process_wide_key, "SignalProcessWideKey" },
    [0x1E] = { svc_get_system_tick, "GetSystemTick" },
    [0x1F] = { svc_connect_to_named_port, "ConnectToNamedPort" },
    [0x21] = { svc_send_sync_request, "SendSyncRequest" },
    [0x22] = { svc_send_sync_request_user, "SendSyncRequestWithUserBuffer" },
    [0x25] = { svc_get_thread_id, "GetThreadId" },
    [0x26] = { svc_break, "Break" },
    [0x27] = { svc_output_debug_string, "OutputDebugString" },
    [0x29] = { svc_get_info, "GetInfo" },
    [0x34] = { svc_wait_for_address, "WaitForAddress" },
    [0x35] = { svc_signal_to_address, "SignalToAddress" },
    [0x45] = { svc_create_event, "CreateEvent" },
};

void recomp_svc(CPU *c, uint64_t pc, uint32_t imm) {
    if (imm < 0x80 && svcs[imm].fn) {
        if (ktrace)
            fprintf(stderr, "svc 0x%02x %-18s at 0x%" PRIx64 " (x0=%" PRIx64 " x1=%" PRIx64 " x2=%" PRIx64 " x3=%" PRIx64 ")",
                    imm, svcs[imm].name, pc, c->x[0], c->x[1], c->x[2], c->x[3]);
        svcs[imm].fn(c);
        if (ktrace) fprintf(stderr, " -> w0=%" PRIx64 " x1=%" PRIx64 "\n", c->x[0] & 0xFFFFFFFF, c->x[1]);
        return;
    }
    fprintf(stderr, "\nsvc: SVC 0x%" PRIx32 " at 0x%" PRIx64 " is not implemented yet "
                    "(see switchbrew.org/wiki/SVC)\n", imm, pc);
    kernel_dump_state();
    exit(1);
}
