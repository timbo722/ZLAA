/*
 * kthread.c - guest threads and the SVCs that make threads wait for each other.
 *
 * Every guest thread runs on its own host thread (pthread), so the game's threads really
 * run in parallel, as on the Switch's 4 cores. All waiting goes through one kernel lock:
 * a thread that has to sleep marks what it waits for (t->wait), then sleeps on its own
 * condition variable until another thread wakes it with a result.
 *
 * Mutexes and condition variables live in the game's memory; the kernel only gets involved
 * when a thread has to wait. A mutex is a 32-bit "tag": 0 = free, otherwise the owner's
 * handle, with bit 30 set if other threads are waiting for it. The game's own code changes
 * the tag with atomic instructions, so the rules here must match Horizon exactly
 * (we follow Atmosphere's kernel, mesosphere: KConditionVariable and KAddressArbiter).
 */
#include "kernel.h"
#include "guest_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

#define MUTEX_HAS_WAITERS 0x40000000u
#define MAX_THREADS 256
#define HOST_STACK_SIZE (64u << 20)   /* recompiled code can nest deeply; this is only address space */

static pthread_mutex_t kernel_lock = PTHREAD_MUTEX_INITIALIZER;
static KThread *threads[MAX_THREADS];  /* every thread ever created (exited ones stay, for now) */
static int nthreads;
static uint64_t next_thread_id = 0x51, wait_counter;
static __thread KThread *cur_thread;
static pthread_cond_t thread_exited = PTHREAD_COND_INITIALIZER;   /* kthread_run_main waits on it */

void klock(void) { pthread_mutex_lock(&kernel_lock); }
void kunlock(void) { pthread_mutex_unlock(&kernel_lock); }
KThread *kthread_current(void) { return cur_thread; }

static void ret(CPU *c, uint32_t result) { c->x[0] = result; }

static KThread *thread_from_handle(uint32_t handle) {
    KHandle *h = khandle_get(handle);
    return h && h->type == KOBJ_THREAD ? h->obj : NULL;
}

/* Guest memory as 32-bit atomics (the game uses LDAXR/STLXR on the same words) */
static uint32_t *word(uint64_t addr) { return (uint32_t *)(guest_mem + addr); }
static int bad_word_addr(uint64_t addr) { return (addr & 3) || addr >= guest_mem_size - 4; }

/* ================================================================ sleeping and waking */
/* Sleep until woken or the timeout (ns; negative = forever) passes. Kernel lock held. */
static uint32_t sleep_locked(KThread *t, int64_t timeout_ns) {
    pthread_cond_t *cv = t->host_cond;
    t->woken = 0;
    t->wait_seq = ++wait_counter;
    if (timeout_ns == 0) {
        t->wait = WAIT_NONE;
        return R_TIMED_OUT;
    }
    struct timespec until;
    if (timeout_ns > 0) {
        clock_gettime(CLOCK_MONOTONIC, &until);
        int64_t ns = until.tv_nsec + timeout_ns % 1000000000;
        until.tv_sec += timeout_ns / 1000000000 + ns / 1000000000;
        until.tv_nsec = ns % 1000000000;
    }
    while (!t->woken) {
        if (timeout_ns < 0) {
            pthread_cond_wait(cv, &kernel_lock);
        } else if (pthread_cond_timedwait(cv, &kernel_lock, &until) != 0 && !t->woken) {
            t->wait = WAIT_NONE;
            return R_TIMED_OUT;
        }
    }
    return t->wait_result;
}

static void wake_locked(KThread *t, uint32_t result) {
    t->wait = WAIT_NONE;
    t->wait_result = result;
    t->woken = 1;
    pthread_cond_signal(t->host_cond);
}

/* The waiter that should go first: highest priority (lowest number), then longest waiting. */
static KThread *first_waiter(int kind, uint64_t addr) {
    KThread *best = NULL;
    for (int k = 0; k < nthreads; k++) {
        KThread *t = threads[k];
        if (t->wait != kind || t->wait_addr != addr) continue;
        if (!best || t->priority < best->priority ||
            (t->priority == best->priority && t->wait_seq < best->wait_seq))
            best = t;
    }
    return best;
}

static int count_waiters(int kind, uint64_t addr) {
    int n = 0;
    for (int k = 0; k < nthreads; k++) n += threads[k]->wait == kind && threads[k]->wait_addr == addr;
    return n;
}

/* ================================================================ threads */
static KThread *new_thread(uint64_t entry, uint64_t arg, uint64_t stack_top, int priority, int core) {
    if (nthreads == MAX_THREADS) return NULL;
    KThread *t = calloc(1, sizeof *t);
    pthread_cond_t *cv = malloc(sizeof *cv);
    if (!t || !cv) { free(t); free(cv); return NULL; }
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);     /* timeouts aren't affected by clock changes */
    pthread_cond_init(cv, &attr);
    pthread_condattr_destroy(&attr);
    t->host_cond = cv;
    t->tls = ktls_alloc();
    t->handle = khandle_new(KOBJ_THREAD, t);
    if (!t->tls || !t->handle) { free(cv); free(t); return NULL; }
    t->entry = entry;
    t->arg = arg;
    t->stack_top = stack_top;
    t->priority = priority;
    t->core = core;
    t->id = next_thread_id++;
    threads[nthreads++] = t;
    return t;
}

static void *thread_main(void *p) {
    KThread *t = p;
    cur_thread = t;
    CPU *c = &t->cpu;
    memset(c, 0, sizeof *c);
    c->mem = guest_mem;
    c->mem_size = guest_mem_size;
    c->excl_addr = ~0ull;
    c->sp = t->stack_top;
    c->tpidrro_el0 = t->tls;
    c->x[0] = t->arg;
    c->x[1] = t->handle;    /* only meaningful for the main thread */
    recomp_call_addr(c, t->entry);
    /* A thread function that returns would jump to whatever X30 held: the game's threads
     * always call svcExitThread instead, so returning here is treated the same way. */
    svc_exit_thread(c);
    return NULL;
}

static int start_host_thread(KThread *t) {
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, HOST_STACK_SIZE);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&th, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
    return r;
}

KThread *kthread_create_main(uint64_t entry, uint64_t stack_top, int priority, int core) {
    klock();
    /* What the kernel passes to a new application's first instruction:
     * X0 = 0 (no exception info: a normal start), X1 = handle of the main thread. */
    KThread *t = new_thread(entry, 0, stack_top, priority, core);
    kunlock();
    return t;
}

int kthread_start_main(KThread *t) {
    /* It runs on its own thread with a big stack, like every guest thread; the process ends
     * with svcExitProcess (which exits the program) or when the main thread exits. */
    klock();
    t->started = 1;
    int r = start_host_thread(t);
    kunlock();
    if (r != 0) fprintf(stderr, "kernel: could not start the main thread\n");
    return r;
}

int kthread_wait_main(KThread *t, int timeout_ms) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += (long)timeout_ms * 1000000;
    until.tv_sec += until.tv_nsec / 1000000000;
    until.tv_nsec %= 1000000000;
    klock();
    while (!t->exited && pthread_cond_timedwait(&thread_exited, &kernel_lock, &until) == 0) {}
    int exited = t->exited;
    kunlock();
    return exited;
}

/* 0x08 CreateThread(-, entry, arg, stack_top, priority, core) -> handle */
void svc_create_thread(CPU *c) {
    int prio = (int)(uint32_t)c->x[4], core = (int)(int32_t)c->x[5];
    if (prio < 0 || prio > 63) { ret(c, R_INVALID_PRIORITY); return; }
    if (core == -2) core = kproc.main_core;              /* -2: "the process's default core" */
    klock();
    KThread *t = new_thread(c->x[1], c->x[2], c->x[3], prio, core);
    kunlock();
    if (!t) { ret(c, R_OUT_OF_RESOURCE); return; }
    c->x[1] = t->handle;
    ret(c, R_SUCCESS);
}

/* 0x09 StartThread(handle) */
void svc_start_thread(CPU *c) {
    klock();
    KThread *t = thread_from_handle((uint32_t)c->x[0]);
    uint32_t r = !t ? R_INVALID_HANDLE : t->started ? R_INVALID_STATE : R_SUCCESS;
    if (r == R_SUCCESS) {
        t->started = 1;
        if (start_host_thread(t) != 0) r = R_OUT_OF_RESOURCE;
    }
    kunlock();
    ret(c, r);
}

/* 0x0A ExitThread(): never returns */
void svc_exit_thread(CPU *c) {
    (void)c;
    KThread *t = kthread_current();
    klock();
    t->exited = 1;
    pthread_cond_broadcast(&thread_exited);    /* kthread_run_main may be waiting for this */
    kunlock();
    ksync_signal_all();                   /* WaitSynchronization on a thread ends when it exits */
    pthread_exit(NULL);
}

/* 0x0B SleepThread(ns). 0, -1, -2 mean "let other threads run" (yield). */
void svc_sleep_thread(CPU *c) {
    int64_t ns = (int64_t)c->x[0];
    if (ns <= 0) {
        sched_yield();
        return;
    }
    struct timespec ts = { ns / 1000000000, ns % 1000000000 };
    while (nanosleep(&ts, &ts) != 0) {}
}

/* 0x0C GetThreadPriority(-, handle) -> priority */
void svc_get_thread_priority(CPU *c) {
    KThread *t = thread_from_handle((uint32_t)c->x[1]);
    if (!t) { ret(c, R_INVALID_HANDLE); return; }
    c->x[1] = (uint64_t)t->priority;
    ret(c, R_SUCCESS);
}

/* 0x0D SetThreadPriority(handle, priority). Only used to order waiters here: host threads
 * all get the same priority from the host OS. */
void svc_set_thread_priority(CPU *c) {
    KThread *t = thread_from_handle((uint32_t)c->x[0]);
    int prio = (int)(uint32_t)c->x[1];
    if (!t) { ret(c, R_INVALID_HANDLE); return; }
    if (prio < 0 || prio > 63) { ret(c, R_INVALID_PRIORITY); return; }
    klock();
    t->priority = prio;
    kunlock();
    ret(c, R_SUCCESS);
}

/* 0x0E GetThreadCoreMask(-, -, handle) -> core, mask */
void svc_get_thread_core_mask(CPU *c) {
    KThread *t = thread_from_handle((uint32_t)c->x[2]);
    if (!t) { ret(c, R_INVALID_HANDLE); return; }
    c->x[1] = (uint64_t)(uint32_t)t->core;
    c->x[2] = 1ull << t->core;
    ret(c, R_SUCCESS);
}

/* 0x0F SetThreadCoreMask(handle, core, mask). We note the core; the host OS picks real cores. */
void svc_set_thread_core_mask(CPU *c) {
    KThread *t = thread_from_handle((uint32_t)c->x[0]);
    int core = (int)(int32_t)c->x[1];
    if (!t) { ret(c, R_INVALID_HANDLE); return; }
    if (core >= 0 && core < 4) t->core = core;
    ret(c, R_SUCCESS);
}

/* 0x10 GetCurrentProcessorNumber() -> core (no result code) */
void svc_get_current_processor_number(CPU *c) { c->x[0] = (uint64_t)kthread_current()->core; }

/* 0x25 GetThreadId(-, handle) -> id */
void svc_get_thread_id(CPU *c) {
    KThread *t = thread_from_handle((uint32_t)c->x[1]);
    if (!t) { ret(c, R_INVALID_HANDLE); return; }
    c->x[1] = t->id;
    ret(c, R_SUCCESS);
}

/* ================================================================ mutexes */
/* Give the mutex at addr to its next waiter, or mark it free. Kernel lock held. */
static void release_mutex_locked(uint64_t addr) {
    KThread *next = first_waiter(WAIT_MUTEX, addr);
    if (!next) {
        __atomic_store_n(word(addr), 0, __ATOMIC_SEQ_CST);
        return;
    }
    uint32_t tag = next->mutex_tag | (count_waiters(WAIT_MUTEX, addr) > 1 ? MUTEX_HAS_WAITERS : 0);
    __atomic_store_n(word(addr), tag, __ATOMIC_SEQ_CST);
    wake_locked(next, R_SUCCESS);
}

/* 0x1A ArbitrateLock(owner tag, mutex addr, my tag): the game found the mutex taken and
 * set the "has waiters" bit; sleep until the owner hands it over. */
void svc_arbitrate_lock(CPU *c) {
    uint32_t owner = (uint32_t)c->x[0], self_tag = (uint32_t)c->x[2];
    uint64_t addr = c->x[1];
    if (bad_word_addr(addr)) { ret(c, R_INVALID_ADDRESS); return; }
    KThread *t = kthread_current();
    klock();
    if (!thread_from_handle(owner)) { kunlock(); ret(c, R_INVALID_HANDLE); return; }
    /* If the tag changed meanwhile (owner already released it), just let the game retry. */
    if (__atomic_load_n(word(addr), __ATOMIC_SEQ_CST) != (owner | MUTEX_HAS_WAITERS)) {
        kunlock();
        ret(c, R_SUCCESS);
        return;
    }
    t->wait = WAIT_MUTEX;
    t->wait_addr = addr;
    t->mutex_tag = self_tag;
    uint32_t r = sleep_locked(t, -1);
    kunlock();
    ret(c, r);
}

/* 0x1B ArbitrateUnlock(mutex addr): the game is releasing a mutex that has waiters */
void svc_arbitrate_unlock(CPU *c) {
    uint64_t addr = c->x[0];
    if (bad_word_addr(addr)) { ret(c, R_INVALID_ADDRESS); return; }
    klock();
    release_mutex_locked(addr);
    kunlock();
    ret(c, R_SUCCESS);
}

/* ================================================================ condition variables */
/* 0x1C WaitProcessWideKeyAtomic(mutex addr, cv key, my tag, timeout): release the mutex
 * and wait on the condition variable, in one step (so no signal can be missed). When
 * signalled, the thread gets the mutex back before it returns. */
void svc_wait_process_wide_key_atomic(CPU *c) {
    uint64_t mutex = c->x[0], key = c->x[1];
    uint32_t tag = (uint32_t)c->x[2];
    int64_t timeout = (int64_t)c->x[3];
    if (bad_word_addr(mutex) || bad_word_addr(key)) { ret(c, R_INVALID_ADDRESS); return; }
    KThread *t = kthread_current();
    klock();
    release_mutex_locked(mutex);
    __atomic_store_n(word(key), 1, __ATOMIC_SEQ_CST);     /* "someone is waiting on this cv" */
    t->wait = WAIT_CONDVAR;
    t->wait_addr = key;
    t->mutex_addr = mutex;
    t->mutex_tag = tag;
    /* On timeout the thread does NOT hold the mutex: the game's code relocks it itself. */
    uint32_t r = sleep_locked(t, timeout);
    kunlock();
    ret(c, r);
}

/* 0x1D SignalProcessWideKey(cv key, count): wake up to `count` waiters (<= 0: all) */
void svc_signal_process_wide_key(CPU *c) {
    uint64_t key = c->x[0];
    int32_t count = (int32_t)c->x[1];
    klock();
    for (int n = 0; count <= 0 || n < count; n++) {
        KThread *t = first_waiter(WAIT_CONDVAR, key);
        if (!t) break;
        /* It must take its mutex back: if the mutex is free, take it now; otherwise
         * mark it as having waiters and queue up behind the owner. */
        uint32_t *m = word(t->mutex_addr), cur = __atomic_load_n(m, __ATOMIC_SEQ_CST);
        for (;;) {
            uint32_t want = cur == 0 ? t->mutex_tag : cur | MUTEX_HAS_WAITERS;
            if (__atomic_compare_exchange_n(m, &cur, want, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
        }
        if (cur == 0) {
            wake_locked(t, R_SUCCESS);
        } else {
            t->wait = WAIT_MUTEX;
            t->wait_addr = t->mutex_addr;
        }
    }
    if (!first_waiter(WAIT_CONDVAR, key)) __atomic_store_n(word(key), 0, __ATOMIC_SEQ_CST);
    kunlock();
}

/* ================================================================ address arbiter */
/* Lower-level waiting on a memory word (the game builds semaphores etc. on these). */
enum { ARB_WAIT_IF_LESS_THAN, ARB_DECREMENT_AND_WAIT_IF_LESS_THAN, ARB_WAIT_IF_EQUAL };
enum { SIG_SIGNAL, SIG_INCREMENT_IF_EQUAL, SIG_MODIFY_BY_WAITING_COUNT_IF_EQUAL };

/* 0x34 WaitForAddress(addr, type, value, timeout) */
void svc_wait_for_address(CPU *c) {
    uint64_t addr = c->x[0];
    uint32_t type = (uint32_t)c->x[1];
    int32_t value = (int32_t)c->x[2];
    int64_t timeout = (int64_t)c->x[3];
    if (bad_word_addr(addr)) { ret(c, R_INVALID_ADDRESS); return; }
    if (type > ARB_WAIT_IF_EQUAL) { ret(c, R_INVALID_ENUM); return; }
    KThread *t = kthread_current();
    klock();
    int32_t cur = (int32_t)__atomic_load_n(word(addr), __ATOMIC_SEQ_CST);
    int should_wait = type == ARB_WAIT_IF_EQUAL ? cur == value : cur < value;
    if (!should_wait) { kunlock(); ret(c, R_INVALID_STATE); return; }
    if (type == ARB_DECREMENT_AND_WAIT_IF_LESS_THAN)
        __atomic_store_n(word(addr), (uint32_t)(cur - 1), __ATOMIC_SEQ_CST);
    t->wait = WAIT_ADDRESS;
    t->wait_addr = addr;
    uint32_t r = sleep_locked(t, timeout);
    kunlock();
    ret(c, r);
}

/* 0x35 SignalToAddress(addr, type, value, count) */
void svc_signal_to_address(CPU *c) {
    uint64_t addr = c->x[0];
    uint32_t type = (uint32_t)c->x[1];
    int32_t value = (int32_t)c->x[2], count = (int32_t)c->x[3];
    if (bad_word_addr(addr)) { ret(c, R_INVALID_ADDRESS); return; }
    if (type > SIG_MODIFY_BY_WAITING_COUNT_IF_EQUAL) { ret(c, R_INVALID_ENUM); return; }
    klock();
    if (type != SIG_SIGNAL) {
        int32_t new_value;
        if (type == SIG_INCREMENT_IF_EQUAL) {
            new_value = value + 1;
        } else {                 /* tells the game whether waiters remain after this signal */
            int n = count_waiters(WAIT_ADDRESS, addr);
            if (count <= 0) new_value = n > 0 ? value - 2 : value + 1;
            else new_value = n == 0 ? value + 1 : n <= count ? value - 1 : value;
        }
        uint32_t expected = (uint32_t)value;
        if (!__atomic_compare_exchange_n(word(addr), &expected, (uint32_t)new_value, 0,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            kunlock();
            ret(c, R_INVALID_STATE);
            return;
        }
    }
    for (int n = 0; count <= 0 || n < count; n++) {
        KThread *t = first_waiter(WAIT_ADDRESS, addr);
        if (!t) break;
        wake_locked(t, R_SUCCESS);
    }
    kunlock();
    ret(c, R_SUCCESS);
}

/* ================================================================ events and WaitSynchronization */
/* Is the object behind this handle signalled? (Kernel lock held.) */
static int handle_signaled(uint32_t handle, int *valid) {
    KHandle *h = khandle_get(handle);
    *valid = 1;
    if (h && h->type == KOBJ_READABLE_EVENT) return ((KEvent *)h->obj)->signaled;
    if (h && h->type == KOBJ_THREAD) return ((KThread *)h->obj)->exited;
    *valid = 0;
    return 0;
}

void ksync_signal_all(void) {
    klock();
    for (int k = 0; k < nthreads; k++) {
        KThread *t = threads[k];
        if (t->wait != WAIT_SYNC) continue;
        for (int i = 0; i < t->n_wait_handles; i++) {
            int valid;
            if (handle_signaled(t->wait_handles[i], &valid)) {
                t->wait_index = i;
                wake_locked(t, R_SUCCESS);
                break;
            }
        }
    }
    kunlock();
}

/* 0x18 WaitSynchronization(-, handles, count, timeout) -> index of the signalled one */
void svc_wait_synchronization(CPU *c) {
    uint64_t list = c->x[1];
    int32_t n = (int32_t)c->x[2];
    int64_t timeout = (int64_t)c->x[3];
    if (n < 0 || n > MAX_WAIT_HANDLES) { ret(c, R_OUT_OF_RANGE); return; }
    if (list > guest_mem_size - 4u * (uint32_t)n) { ret(c, R_INVALID_ADDRESS); return; }
    KThread *t = kthread_current();
    klock();
    t->n_wait_handles = n;
    for (int i = 0; i < n; i++) {
        t->wait_handles[i] = ld32(c, list + 4u * (uint32_t)i);
        int valid, sig = handle_signaled(t->wait_handles[i], &valid);
        if (!valid) {
            KHandle *h = khandle_get(t->wait_handles[i]);
            kunlock();
            if (h) {   /* a kind of object we can't wait on yet: say which */
                fprintf(stderr, "kernel: WaitSynchronization on handle type %d not supported\n", h->type);
                kernel_dump_state();
                exit(1);
            }
            ret(c, R_INVALID_HANDLE);
            return;
        }
        if (sig) {
            kunlock();
            c->x[1] = (uint64_t)i;
            ret(c, R_SUCCESS);
            return;
        }
    }
    if (t->cancel_pending) {
        t->cancel_pending = 0;
        kunlock();
        ret(c, R_CANCELLED);
        return;
    }
    if (ktrace_wait && timeout != 0) {
        fprintf(stderr, "wait: thread %" PRIu64 " sleeps on", t->id);
        for (int i = 0; i < n; i++) {
            KHandle *h = khandle_get(t->wait_handles[i]);
            fprintf(stderr, " [%s]", h->type == KOBJ_THREAD ? "thread exit" : ((KEvent *)h->obj)->name);
        }
        fprintf(stderr, " timeout %" PRId64 "\n", timeout);
    }
    t->wait = WAIT_SYNC;
    t->wait_addr = 0;
    uint32_t r = sleep_locked(t, timeout);
    kunlock();
    c->x[1] = r == R_SUCCESS ? (uint64_t)t->wait_index : 0;
    ret(c, r);
}

/* 0x19 CancelSynchronization(thread): interrupt its WaitSynchronization */
void svc_cancel_synchronization(CPU *c) {
    klock();
    KThread *t = thread_from_handle((uint32_t)c->x[0]);
    if (t) {
        if (t->wait == WAIT_SYNC) wake_locked(t, R_CANCELLED);
        else t->cancel_pending = 1;
    }
    kunlock();
    ret(c, t ? R_SUCCESS : R_INVALID_HANDLE);
}

/* 0x45 CreateEvent() -> writable handle, readable handle */
void svc_create_event(CPU *c) {
    KEvent *e = calloc(1, sizeof *e);
    if (e) snprintf(e->name, sizeof e->name, "game event");
    uint32_t w = e ? khandle_new(KOBJ_WRITABLE_EVENT, e) : 0;
    uint32_t r = w ? khandle_new(KOBJ_READABLE_EVENT, e) : 0;
    if (!r) { ret(c, R_OUT_OF_RESOURCE); return; }   /* (leaks on this unlikely path) */
    c->x[1] = w;
    c->x[2] = r;
    ret(c, R_SUCCESS);
}

static KEvent *event_from_handle(uint32_t handle, int writable_ok, int readable_ok) {
    KHandle *h = khandle_get(handle);
    if (!h) return NULL;
    if ((h->type == KOBJ_WRITABLE_EVENT && writable_ok) || (h->type == KOBJ_READABLE_EVENT && readable_ok))
        return h->obj;
    return NULL;
}

KEvent *kevent_new(const char *name) {
    KEvent *e = calloc(1, sizeof *e);
    if (!e) { fprintf(stderr, "kernel: out of memory\n"); exit(1); }
    snprintf(e->name, sizeof e->name, "%s", name);
    return e;
}

uint32_t kevent_readable_handle(KEvent *e) { return khandle_new(KOBJ_READABLE_EVENT, e); }

void kevent_signal(KEvent *e) {
    klock();
    e->signaled = 1;
    kunlock();
    ksync_signal_all();
}

void kevent_clear(KEvent *e) {
    klock();
    e->signaled = 0;
    kunlock();
}

/* 0x11 SignalEvent(writable handle) */
void svc_signal_event(CPU *c) {
    KEvent *e = event_from_handle((uint32_t)c->x[0], 1, 0);
    if (!e) { ret(c, R_INVALID_HANDLE); return; }
    klock();
    e->signaled = 1;
    kunlock();
    ksync_signal_all();
    ret(c, R_SUCCESS);
}

/* 0x12 ClearEvent(either handle) */
void svc_clear_event(CPU *c) {
    KEvent *e = event_from_handle((uint32_t)c->x[0], 1, 1);
    if (!e) { ret(c, R_INVALID_HANDLE); return; }
    klock();
    e->signaled = 0;
    kunlock();
    ret(c, R_SUCCESS);
}

/* 0x17 ResetSignal(readable handle): clear it, but fail if it wasn't signalled */
void svc_reset_signal(CPU *c) {
    KEvent *e = event_from_handle((uint32_t)c->x[0], 0, 1);
    if (!e) { ret(c, R_INVALID_HANDLE); return; }
    klock();
    uint32_t r = e->signaled ? R_SUCCESS : R_INVALID_STATE;
    e->signaled = 0;
    kunlock();
    ret(c, r);
}
