/*
 * kernel.c - process state for our Horizon stand-in: memory map, handle table, startup.
 * Threads and synchronization are in kthread.c.
 */
#define _GNU_SOURCE          /* memfd_create */
#include "kernel.h"
#include "guest_memory.h"
#include "ipc.h"
#include "game_lib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#else
#include <unwind.h>
#endif

KProcess kproc;
int ktrace, ktrace_wait;

/* ================================================================ memory map */
/* Sorted, non-overlapping list of mapped blocks. Anything not in the list is Free. */
typedef struct { uint64_t addr, size; int state, perm; } KBlock;
static KBlock *blocks;
static int nblocks, cap_blocks;
static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;   /* several threads may map at once */

static int host_perm(int state, int perm) {
    if (state == MEM_FREE || state == MEM_INACCESSIBLE) return GMEM_NONE;
    /* The host never runs guest code (it runs the recompiled C), so X just means readable. */
    return (perm & PERM_W) ? GMEM_READ | GMEM_WRITE : (perm & (PERM_R | PERM_X)) ? GMEM_READ : GMEM_NONE;
}

static int insert_block(int at, KBlock b) {
    if (nblocks == cap_blocks) {
        int ncap = cap_blocks ? cap_blocks * 2 : 256;
        KBlock *nb = realloc(blocks, ncap * sizeof *nb);
        if (!nb) return -1;
        blocks = nb;
        cap_blocks = ncap;
    }
    memmove(&blocks[at + 1], &blocks[at], (nblocks - at) * sizeof *blocks);
    blocks[at] = b;
    nblocks++;
    return 0;
}

static int kmem_map_locked(uint64_t addr, uint64_t size, int state, int perm) {
    uint64_t end = addr + size;
    if ((addr | size) & (GUEST_PAGE - 1) || size == 0 || end > ASPACE_END || end < addr) return -1;

    /* 1. cut out [addr, end) from every block it touches (a block may split in two) */
    for (int k = 0; k < nblocks; k++) {
        KBlock *b = &blocks[k];
        uint64_t b_end = b->addr + b->size;
        if (b_end <= addr || b->addr >= end) continue;
        if (b->addr < addr && b_end > end) {               /* new range is in the middle */
            KBlock tail = { end, b_end - end, b->state, b->perm };
            b->size = addr - b->addr;
            if (insert_block(k + 1, tail) != 0) return -1;
            break;
        }
        if (b->addr < addr) { b->size = addr - b->addr; continue; }       /* keep the front */
        if (b_end > end) { b->size = b_end - end; b->addr = end; continue; } /* keep the back */
        memmove(b, b + 1, (nblocks - k - 1) * sizeof *b);                    /* fully covered */
        nblocks--;
        k--;
    }
    /* 2. insert the new block in order (Free is simply "no block") */
    if (state != MEM_FREE) {
        int at = 0;
        while (at < nblocks && blocks[at].addr < addr) at++;
        if (insert_block(at, (KBlock){ addr, size, state, perm }) != 0) return -1;
    }
    /* 3. host protection. Unmapped memory is thrown away, so mapping it again reads zeros. */
    if (state == MEM_FREE) return gmem_decommit(addr, size);
    return gmem_protect(addr, size, host_perm(state, perm));
}

int kmem_map(uint64_t addr, uint64_t size, int state, int perm) {
    pthread_mutex_lock(&mem_lock);
    int r = kmem_map_locked(addr, size, state, perm);
    pthread_mutex_unlock(&mem_lock);
    return r;
}

static void kmem_query_locked(uint64_t addr, uint64_t *base, uint64_t *size, int *state, int *perm) {
    if (addr >= ASPACE_END) {                       /* beyond our address space */
        *base = ASPACE_END; *size = 0 - ASPACE_END; *state = MEM_INACCESSIBLE; *perm = PERM_NONE;
        return;
    }
    uint64_t free_lo = 0, free_hi = ASPACE_END;
    for (int k = 0; k < nblocks; k++) {
        const KBlock *b = &blocks[k];
        if (addr >= b->addr && addr - b->addr < b->size) {
            *base = b->addr; *size = b->size; *state = b->state; *perm = b->perm;
            return;
        }
        if (b->addr + b->size <= addr) free_lo = b->addr + b->size;
        else { free_hi = b->addr; break; }
    }
    /* Neighbouring blocks with the same state would be merged by the real kernel; ours
     * may be split, which only means QueryMemory reports smaller pieces. */
    *base = free_lo; *size = free_hi - free_lo; *state = MEM_FREE; *perm = PERM_NONE;
}

void kmem_query(uint64_t addr, uint64_t *base, uint64_t *size, int *state, int *perm) {
    pthread_mutex_lock(&mem_lock);
    kmem_query_locked(addr, base, size, state, perm);
    pthread_mutex_unlock(&mem_lock);
}

static uint64_t find_free_locked(uint64_t lo, uint64_t hi, uint64_t size) {
    uint64_t at = lo;
    for (int k = 0; k < nblocks && at < hi; k++) {
        const KBlock *b = &blocks[k];
        if (b->addr + b->size <= at) continue;
        if (b->addr >= at + size) break;            /* the gap before this block fits */
        at = b->addr + b->size;
    }
    return (at + size <= hi && at + size > at) ? at : 0;
}

uint64_t kmem_map_free(uint64_t lo, uint64_t hi, uint64_t size, int state, int perm) {
    pthread_mutex_lock(&mem_lock);
    uint64_t at = find_free_locked(lo, hi, size);
    if (at && kmem_map_locked(at, size, state, perm) != 0) at = 0;
    pthread_mutex_unlock(&mem_lock);
    return at;
}

/* ================================================================ handles */
#define MAX_HANDLES 1024
static KHandle handles[MAX_HANDLES];
static uint16_t handle_gen[MAX_HANDLES];   /* bumped each time a slot is reused */
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

/* A handle is (generation << 15) | slot, like the real kernel, so a stale handle
 * (closed, then its slot reused) doesn't silently refer to the new object. */
uint32_t khandle_new(int type, void *obj) {
    uint32_t h = 0;
    pthread_mutex_lock(&handle_lock);
    for (uint32_t k = 1; k < MAX_HANDLES; k++) {
        if (handles[k].type == KOBJ_NONE) {
            handles[k].type = type;
            handles[k].obj = obj;
            handle_gen[k] = (uint16_t)(handle_gen[k] % 0x7FFF + 1);    /* 1..0x7FFF */
            h = ((uint32_t)handle_gen[k] << 15) | k;
            break;
        }
    }
    pthread_mutex_unlock(&handle_lock);
    return h;
}

KHandle *khandle_get(uint32_t handle) {
    if (handle == HANDLE_CURRENT_THREAD) {
        static __thread KHandle cur;
        cur.type = KOBJ_THREAD;
        cur.obj = kthread_current();
        return &cur;
    }
    if (handle == HANDLE_CURRENT_PROCESS) {
        static KHandle proc = { KOBJ_PROCESS, &kproc };
        return &proc;
    }
    uint32_t k = handle & 0x7FFF;
    if (k == 0 || k >= MAX_HANDLES || handles[k].type == KOBJ_NONE || handle >> 15 != handle_gen[k])
        return NULL;
    return &handles[k];
}

int khandle_close(uint32_t handle) {
    pthread_mutex_lock(&handle_lock);
    KHandle *h = khandle_get(handle);
    int r = -1;
    if (h && handle < HANDLE_CURRENT_PROCESS) {
        h->type = KOBJ_NONE;
        h->obj = NULL;
        r = 0;
    }
    pthread_mutex_unlock(&handle_lock);
    return r;
}

/* ================================================================ shared memory */
KSharedMemory *kshm_new(uint64_t size) {
    KSharedMemory *s = calloc(1, sizeof *s);
    if (!s) exit(1);
    s->size = (size + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    s->fd = memfd_create("laa-shared", 0);
    if (s->fd < 0 || ftruncate(s->fd, (off_t)s->size) != 0) { perror("kshm_new"); exit(1); }
    s->host = mmap(NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
    if (s->host == MAP_FAILED) { perror("kshm_new: mmap"); exit(1); }
    return s;
}

uint32_t kshm_handle(KSharedMemory *shm) { return khandle_new(KOBJ_SHARED_MEMORY, shm); }

/* ================================================================ thread-local storage */
static uint64_t tls_page, tls_next = GUEST_PAGE;    /* next free slot in the current page */
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t ktls_alloc(void) {
    pthread_mutex_lock(&tls_lock);
    if (tls_next == GUEST_PAGE) {
        uint64_t p = kmem_map_free(KMAP_BASE, KMAP_BASE + KMAP_SIZE, GUEST_PAGE, MEM_THREAD_LOCAL, PERM_RW);
        if (p) { tls_page = p; tls_next = 0; }
    }
    uint64_t slot = 0;
    if (tls_next < GUEST_PAGE) { slot = tls_page + tls_next; tls_next += 0x200; }
    pthread_mutex_unlock(&tls_lock);
    return slot;          /* fresh pages read as zero; freed slots are not reused yet */
}

/* ================================================================ crash reports */
#if !defined(__GLIBC__)
static _Unwind_Reason_Code unwind_frame(struct _Unwind_Context *ctx, void *arg) {
    struct { void *pc[48]; int n; } *bt = arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && bt->n < 48) bt->pc[bt->n++] = (void *)pc;
    return bt->n < 48 ? _URC_NO_REASON : _URC_END_OF_STACK;
}
#endif

static void print_backtrace(void) {
    /* Recompiled functions are named f_<guest address>, so the host backtrace says which guest
     * functions were running. They are hidden symbols in the game library: game_lib_symbol
     * names them from its table; dladdr names the runtime's (exported) functions. */
    void *frames[48];
#if defined(__GLIBC__)
    int n = backtrace(frames, 48);
#else
    /* Android (bionic): walk the stack with the unwinder */
    struct { void *pc[48]; int n; } bt = { { 0 }, 0 };
    _Unwind_Backtrace(unwind_frame, &bt);
    int n = bt.n;
    memcpy(frames, bt.pc, sizeof frames);
#endif
    fprintf(stderr, "host backtrace (f_XXXX = guest function at 0xXXXX):\n");
    for (int i = 0; i < n; i++) {
        char buf[64];
        unsigned long off;
        const char *game = game_lib_symbol((uintptr_t)frames[i], buf, sizeof buf, &off);
        Dl_info info;
        if (game)
            fprintf(stderr, "  %s+%#lx\n", game, off);
        else if (dladdr(frames[i], &info) && info.dli_sname)
            fprintf(stderr, "  %s+%#lx\n", info.dli_sname, (unsigned long)((char *)frames[i] - (char *)info.dli_saddr));
        else
            fprintf(stderr, "  %p\n", frames[i]);
    }
}

static void dump_cpu(const CPU *c) {
    for (int k = 0; k < 31; k++)
        fprintf(stderr, "  x%-2d %016" PRIx64 "%s", k, c->x[k], k % 4 == 3 ? "\n" : "");
    fprintf(stderr, "  sp  %016" PRIx64 "\n  nzcv %d%d%d%d  tpidr %" PRIx64 "  tpidrro %" PRIx64 "\n",
            c->sp, c->n, c->z, c->c, c->v, c->tpidr_el0, c->tpidrro_el0);
}

/* A guest C++ object's class, from its vtable (Itanium ABI: vtable[-1] = typeinfo, whose
 * second word is the mangled name). NULL if p doesn't look like one. Reads only mapped memory. */
static const char *guest_class_name(uint64_t p) {
    uint64_t vt, ti, name;
    if (p < 0x8000000 || p + 8 > guest_mem_size) return NULL;
    memcpy(&vt, guest_mem + p, 8);
    if (vt < 0x8000008 || vt + 8 > guest_mem_size) return NULL;
    memcpy(&ti, guest_mem + vt - 8, 8);
    if (ti < 0x8000000 || ti + 16 > guest_mem_size) return NULL;
    memcpy(&name, guest_mem + ti + 8, 8);
    if (name < 0x8000000 || name + 64 > guest_mem_size) return NULL;
    const char *s = (const char *)guest_mem + name;
    for (int i = 0; i < 64; i++) {                   /* a printable, terminated string */
        if (!s[i]) return i > 1 ? s : NULL;
        if (s[i] < 0x20 || s[i] > 0x7E) return NULL;
    }
    return NULL;
}

void kernel_dump_state(void) {
    KThread *t = kthread_current();
    if (t) {
        fprintf(stderr, "registers of thread %" PRIu64 ":\n", t->id);
        dump_cpu(&t->cpu);
        for (int i = 0; i < 31; i++) {               /* registers that point at C++ objects */
            const char *n = guest_class_name(t->cpu.x[i]);
            if (n) fprintf(stderr, "  x%d -> object of class %s\n", i, n);
        }
    }
    print_backtrace();
}

static void on_fatal_signal(int sig, siginfo_t *si, void *uctx) {
    (void)uctx;
    uint64_t a = (uint64_t)(uintptr_t)si->si_addr, g = (uint64_t)(uintptr_t)guest_mem;
    if (sig == SIGSEGV || sig == SIGBUS) {
        if (a - g < guest_mem_size) {
            uint64_t ga = a - g, base, size;
            int state, perm;
            kmem_query_locked(ga, &base, &size, &state, &perm);   /* no lock: we may have crashed holding it */
            fprintf(stderr, "\nguest memory fault at 0x%" PRIx64 " (block 0x%" PRIx64 "+0x%" PRIx64
                    " state 0x%x perm %d)\n", ga, base, size, state, perm);
        } else {
            fprintf(stderr, "\nhost memory fault at %p (outside guest memory)\n", si->si_addr);
        }
    } else {
        fprintf(stderr, "\nsignal %d\n", sig);
    }
    kernel_dump_state();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fatal_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

/* ================================================================ process setup */
static KThread *main_thread;

static int read_npdm(const char *exefs_dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/main.npdm", exefs_dir);
    FILE *f = fopen(path, "rb");
    uint8_t buf[0x1000];
    size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f) fclose(f);
    if (n < 0x80 || memcmp(buf, "META", 4) != 0) {
        fprintf(stderr, "kernel: %s missing or not an NPDM file\n", path);
        return -1;
    }
    uint32_t stack, aci_off;
    memcpy(&stack, buf + 0x1C, 4);
    memcpy(&aci_off, buf + 0x70, 4);
    kproc.main_priority = buf[0x0E];
    kproc.main_core = buf[0x0F];
    kproc.main_stack_size = (stack + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    if (aci_off + 0x18 <= n && memcmp(buf + aci_off, "ACI0", 4) == 0)
        memcpy(&kproc.program_id, buf + aci_off + 0x10, 8);
    return 0;
}

int kernel_init(const char *exefs_dir, const GameImage *img) {
    ktrace = getenv("LAA_TRACE_SVC") != NULL;
    ktrace_wait = getenv("LAA_TRACE_WAIT") != NULL;
    ipc_trace = getenv("LAA_TRACE_IPC") != NULL;
    install_crash_handlers();
    if (read_npdm(exefs_dir) != 0) return -1;
    kproc.image = *img;

    /* Modules: .text is code (RX), .rodata read-only code (R), .data + .bss writable */
    for (int k = 0; k < img->count; k++) {
        const GameModule *m = &img->mods[k];
        uint64_t ro_end = m->data_off;          /* rodata runs up to where data starts */
        if (kmem_map(m->base + m->text_off, m->ro_off - m->text_off, MEM_CODE, PERM_RX) != 0 ||
            kmem_map(m->base + m->ro_off, ro_end - m->ro_off, MEM_CODE, PERM_R) != 0 ||
            kmem_map(m->base + m->data_off, m->size - m->data_off, MEM_CODE_DATA, PERM_RW) != 0) {
            fprintf(stderr, "kernel: could not map module %s\n", m->name);
            return -1;
        }
    }

    /* The user exception context (svcGetInfo 20) lives in a TLS slot, as on hardware */
    kproc.user_exception_ctx = ktls_alloc();
    if (!kproc.user_exception_ctx) return -1;

    /* Main thread stack, at the top of the stack region */
    uint64_t stack = STACK_BASE + STACK_SIZE - kproc.main_stack_size;
    if (kmem_map(stack, kproc.main_stack_size, MEM_STACK, PERM_RW) != 0) return -1;

    for (int k = 0; k < 4; k++) {
        kproc.random_entropy[k] = ((uint64_t)rand() << 40) ^ ((uint64_t)rand() << 20) ^ (uint64_t)rand() ^
                                  (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ull * (k + 1);
    }

    /* The first module (rtld) starts first, like on hardware */
    const GameModule *first = &img->mods[0];
    main_thread = kthread_create_main(first->base + first->text_off, stack + kproc.main_stack_size,
                                      kproc.main_priority, kproc.main_core);
    if (!main_thread) return -1;
    printf("kernel: program %016" PRIx64 ", main thread prio %d core %d stack 0x%" PRIx64 " at 0x%" PRIx64
           ", TLS 0x%" PRIx64 "\n", kproc.program_id, main_thread->priority, main_thread->core,
           kproc.main_stack_size, stack, main_thread->tls);
    return 0;
}

int kernel_start_main(void) {
    printf("kernel: starting %s at 0x%" PRIx64 "\n", kproc.image.mods[0].name, main_thread->entry);
    fflush(stdout);
    return kthread_start_main(main_thread);
}

int kernel_wait_main(int timeout_ms) { return kthread_wait_main(main_thread, timeout_ms); }

volatile int kernel_settings_gen;

static pthread_mutex_t suspend_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t suspend_cond = PTHREAD_COND_INITIALIZER;
static int suspended;

void kernel_set_suspended(int why, int on) {
    pthread_mutex_lock(&suspend_lock);
    suspended = on ? suspended | why : suspended & ~why;
    pthread_cond_broadcast(&suspend_cond);
    pthread_mutex_unlock(&suspend_lock);
}

int kernel_wait_resumed(void) {
    if (!__atomic_load_n(&suspended, __ATOMIC_ACQUIRE)) return 0;   /* (the usual case, without the lock) */
    pthread_mutex_lock(&suspend_lock);
    int waited = suspended;
    while (suspended) pthread_cond_wait(&suspend_cond, &suspend_lock);
    pthread_mutex_unlock(&suspend_lock);
    return waited;
}
