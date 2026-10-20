/*
 * kernel.h - a small stand-in for the Horizon kernel (the Switch's OS kernel).
 *
 * On a real Switch the game asks the kernel for things with the SVC instruction
 * ("supervisor call"): map memory, create threads, wait on events, talk to services.
 * Our recompiled code calls recomp_svc() instead, and svc.c answers using the state
 * kept here: the process's memory map and its handle table.
 *
 * Reference: switchbrew.org/wiki/SVC, and Atmosphere's kernel (mesosphere) for exact behaviour.
 */
#ifndef KERNEL_H
#define KERNEL_H

#include "recomp_runtime.h"
#include "nso_loader.h"
#include <stdint.h>

/* ---- result codes: (description << 9) | module, kernel module = 1 ---- */
#define KRESULT(desc) (((desc) << 9) | 1)
#define R_SUCCESS              0
#define R_INVALID_SIZE         KRESULT(101)
#define R_INVALID_ADDRESS      KRESULT(102)
#define R_INVALID_CURRENT_MEM  KRESULT(106)
#define R_INVALID_HANDLE       KRESULT(114)
#define R_TIMED_OUT            KRESULT(117)
#define R_OUT_OF_RANGE         KRESULT(119)
#define R_INVALID_ENUM         KRESULT(120)
#define R_NOT_FOUND            KRESULT(121)
#define R_CANCELLED            KRESULT(118)
#define R_INVALID_STATE        KRESULT(125)
#define R_INVALID_PRIORITY     KRESULT(112)
#define R_OUT_OF_RESOURCE      KRESULT(103)

/* ---- the address space we give the game (all inside guest_memory's reservation) ----
 * A real 39-bit Switch process has bigger regions; we pick smaller ones that fit our
 * 64 GiB reservation. The game asks for these with svcGetInfo, so it adapts. */
#define ASPACE_BASE    0x0008000000ull   /* code (modules) starts here */
#define ASPACE_END     0x1000000000ull   /* = GUEST_SPACE_SIZE */
#define HEAP_BASE      0x0100000000ull   /* svcSetHeapSize memory */
#define HEAP_SIZE      0x0180000000ull   /* 6 GiB, as on hardware */
#define ALIAS_BASE     0x0400000000ull   /* svcMapMemory destinations */
#define ALIAS_SIZE     0x0400000000ull
#define STACK_BASE     0x0800000000ull   /* thread stacks (svcMapMemory into here) */
#define STACK_SIZE     0x0080000000ull
#define KMAP_BASE      0x0900000000ull   /* where the kernel itself puts things: TLS, shared memory */
#define KMAP_SIZE      0x0400000000ull

/* ---- memory map, as svcQueryMemory reports it ---- */
enum {                                 /* MemoryState (low byte); switchbrew "MemoryType" */
    MEM_FREE = 0x00, MEM_CODE = 0x03, MEM_CODE_DATA = 0x04, MEM_HEAP = 0x05, MEM_SHARED = 0x06,
    MEM_ALIAS = 0x07, MEM_IPC = 0x0A, MEM_STACK = 0x0B, MEM_THREAD_LOCAL = 0x0C,
    MEM_TRANSFERRED = 0x0D, MEM_INACCESSIBLE = 0x10,
};
enum { PERM_NONE = 0, PERM_R = 1, PERM_W = 2, PERM_X = 4, PERM_RW = 3, PERM_RX = 5 };

/* Record that [addr, addr+size) now has this state and permission (replaces what was
 * there), and set the host page protection to match. 0 on success. */
int kmem_map(uint64_t addr, uint64_t size, int state, int perm);
/* Describe the block containing addr (Free blocks are the gaps between mapped ones). */
void kmem_query(uint64_t addr, uint64_t *base, uint64_t *size, int *state, int *perm);
/* Find a free, page-aligned range of `size` bytes inside [lo, hi) and map it.
 * Returns its address, or 0 if there is no room. */
uint64_t kmem_map_free(uint64_t lo, uint64_t hi, uint64_t size, int state, int perm);

/* ---- handles: small integers the game uses to refer to kernel objects ---- */
enum { KOBJ_NONE, KOBJ_THREAD, KOBJ_PROCESS, KOBJ_SESSION, KOBJ_WRITABLE_EVENT, KOBJ_READABLE_EVENT,
       KOBJ_TRANSFER_MEMORY, KOBJ_SHARED_MEMORY };

/* Memory shared between a service and the game (svcMapSharedMemory). It lives in a memfd:
 * the service writes through `host`, the game sees the same bytes where it maps it. */
typedef struct { int fd; uint64_t size; uint8_t *host; } KSharedMemory;
KSharedMemory *kshm_new(uint64_t size);            /* zero-filled; exits if out of memory */
uint32_t kshm_handle(KSharedMemory *shm);          /* a new handle to give the game */

/* Memory the game lends to a service (svcCreateTransferMemory). Our services can read guest
 * memory directly, so this just remembers where it is. */
typedef struct { uint64_t addr, size; uint32_t perm; } KTransferMemory;
#define HANDLE_CURRENT_PROCESS 0xFFFF8000u   /* pseudo-handles that always mean "me" */
#define HANDLE_CURRENT_THREAD  0xFFFF8001u

typedef struct {
    int type;
    void *obj;              /* points to the object (thread, session, ...) */
} KHandle;

uint32_t khandle_new(int type, void *obj);         /* 0 if the table is full */
KHandle *khandle_get(uint32_t handle);             /* NULL if not a valid handle */
int khandle_close(uint32_t handle);                /* 0 on success */
/* Thread-local storage: one 0x200-byte slot per thread, carved from pages we map. */
uint64_t ktls_alloc(void);                         /* 0 if out of memory */

/* One lock for all thread/sync state. Kernel code that sleeps holds it while deciding. */
void klock(void);
void kunlock(void);

/* ---- threads: each guest thread runs on its own host thread (kthread.c) ---- */
enum { WAIT_NONE, WAIT_MUTEX, WAIT_CONDVAR, WAIT_ADDRESS, WAIT_SYNC };
#define MAX_WAIT_HANDLES 64

typedef struct KThread {
    CPU cpu;                /* this thread's registers */
    uint64_t id;
    uint64_t tls;           /* guest address of its 0x200-byte thread-local area */
    uint64_t entry, arg, stack_top;
    int priority, core;
    uint32_t handle;
    int started, exited;

    /* while asleep in the kernel (all guarded by the kernel lock) */
    int wait;               /* WAIT_* : what it is waiting for */
    uint64_t wait_seq;      /* when it started waiting: equal priorities wake in order */
    uint64_t wait_addr;     /* mutex / condvar key / arbiter address */
    uint64_t mutex_addr;    /* WAIT_CONDVAR: the mutex to take back when signalled */
    uint32_t mutex_tag;     /* the value that marks this thread as a mutex's owner */
    uint32_t wait_handles[MAX_WAIT_HANDLES];
    int n_wait_handles;
    int cancel_pending;     /* svcCancelSynchronization arrived while not waiting */
    int woken;
    uint32_t wait_result;
    int wait_index;         /* WAIT_SYNC: which handle was signalled */
    void *host_cond;        /* pthread_cond_t, sleeps here */
} KThread;

KThread *kthread_current(void);
/* Make the main thread (not started). kthread_run_main() then runs it and returns when
 * the process exits. */
KThread *kthread_create_main(uint64_t entry, uint64_t stack_top, int priority, int core);
void kthread_run_main(KThread *t);

/* Events (svcCreateEvent): one object, a "writable" and a "readable" handle to it */
typedef struct { int signaled; char name[64]; } KEvent;   /* name: for debugging */
/* Something waitable changed (an event was signalled, a thread exited). */
void ksync_signal_all(void);
/* For services: an event they signal, and a new readable handle to give the game. */
KEvent *kevent_new(const char *name);
uint32_t kevent_readable_handle(KEvent *e);
void kevent_signal(KEvent *e);
void kevent_clear(KEvent *e);

/* The SVCs for threads and synchronization, in kthread.c */
void svc_create_thread(CPU *c);
void svc_start_thread(CPU *c);
void svc_exit_thread(CPU *c);
void svc_sleep_thread(CPU *c);
void svc_get_thread_priority(CPU *c);
void svc_set_thread_priority(CPU *c);
void svc_get_thread_core_mask(CPU *c);
void svc_set_thread_core_mask(CPU *c);
void svc_get_current_processor_number(CPU *c);
void svc_get_thread_id(CPU *c);
void svc_arbitrate_lock(CPU *c);
void svc_arbitrate_unlock(CPU *c);
void svc_wait_process_wide_key_atomic(CPU *c);
void svc_signal_process_wide_key(CPU *c);
void svc_wait_for_address(CPU *c);
void svc_signal_to_address(CPU *c);
void svc_wait_synchronization(CPU *c);
void svc_cancel_synchronization(CPU *c);
void svc_create_event(CPU *c);
void svc_signal_event(CPU *c);
void svc_clear_event(CPU *c);
void svc_reset_signal(CPU *c);

/* ---- the process ---- */
typedef struct {
    uint64_t program_id;
    int main_priority, main_core;
    uint64_t main_stack_size;
    uint64_t heap_size;                 /* current svcSetHeapSize size */
    uint64_t user_exception_ctx;        /* svcGetInfo(UserExceptionContextAddress) */
    uint64_t random_entropy[4];
    GameImage image;
} KProcess;

extern KProcess kproc;

/* Read main.npdm, set up the memory map for the loaded modules, create the main thread.
 * Then kernel_run_main() calls the first module's entry point (rtld) like the real kernel. */
int kernel_init(const char *exefs_dir, const GameImage *img);
void kernel_run_main(void);
void kernel_dump_state(void);       /* registers + host backtrace, for crash reports */

/* Log of SVCs as they happen, when the LAA_TRACE_SVC environment variable is set */
extern int ktrace;
extern int ktrace_wait;     /* LAA_TRACE_WAIT: what threads go to sleep on */

#endif
