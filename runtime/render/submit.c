/*
 * submit.c - recording and submitting GPU work.
 *
 * Work is recorded into the command buffer of the current "context" and sent to the GPU with
 * render_submit(), which doesn't wait: the CPU goes on (the game's next frame) while the GPU
 * works. There are NCTX contexts in turn, each with its own command buffer, part of the staging
 * buffer, uniform ring and descriptor pool (draw.c), so recording never overwrites what the GPU
 * may still be reading; a context is reused once the GPU has finished its last submission.
 *
 * Every submission gets a number (its serial) and signals a timeline semaphore with it, so
 * "has the GPU finished submission N" is a counter check (render_serial_done), and NVN syncs
 * wait for the serial they were fenced at. render_flush() submits and waits for everything:
 * for the few places that read GPU results right away (read-backs, CPU fallbacks).
 */
#include "render_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 8, not 2: the game submits many command lists a frame, and with 2 contexts recording each
 * one waited for the GPU to finish the one before last: CPU and GPU took turns */
#define NCTX RENDER_CONTEXTS
#define STAGING_SIZE (512ull << 20)                /* split between the contexts */
#define UNIFORM_RING (8u << 20)                    /* per context */

typedef struct {
    VkCommandPool pool;                            /* its own, reset as a whole: resetting single command
                                                      buffers was 6% of the game thread on the Thor */
    VkCommandBuffer cmd;
    uint64_t serial;                               /* of its last submission (0: none) */
    uint64_t staging_used, uniform_used;
    int clean;                                     /* reset and free to record into (the recycler) */
} Context;

static Context ctx[NCTX];
static int cur;                                    /* the context being recorded into */
static int recording, ready;
static uint64_t submitted;                         /* serial of the last submission */
static VkSemaphore timeline;
static VkBuffer staging, uniform_buffer;
static uint8_t *staging_ptr, *uniform_ptr;
double render_gpu_wait_ms;                         /* time spent waiting for the GPU (LAA_FRAME_STATS) */
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static void *host_buffer(uint64_t size, VkBufferUsageFlags usage, VkBuffer *out) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
    VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, out));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.device, *out, &req);
    /* CPU-cached if the GPU has such memory: the CPU writes it several times faster (the Thor:
     * uncached memory is write-combined) */
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int type = vk_find_memory_type(req.memoryTypeBits, want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    VkMemoryAllocateInfo mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = type >= 0 ? (uint32_t)type : vk_memory_type(req.memoryTypeBits, want) };
    VkDeviceMemory mem;
    VK_CHECK(vkAllocateMemory(vk.device, &mi, NULL, &mem));
    VK_CHECK(vkBindBufferMemory(vk.device, *out, mem, 0));
    void *p;
    VK_CHECK(vkMapMemory(vk.device, mem, 0, VK_WHOLE_SIZE, 0, &p));
    return p;
}

/* ---- GPU timestamps the game asks for (nvnCommandBufferReportCounter, TIMESTAMP) ----
 * The game times each frame on the GPU with two and picks its rendering resolution by that
 * (dynamic resolution, like on the Switch: 720p outdoors when busy). They used to be the CPU's
 * clock when the command list was read, so the frame seemed to cost nothing and the game kept
 * 1080p. Now: a timestamp query where the command is, read back once its submission is done
 * (render_timestamps_update: after waits, at present, before a context is reused) and stored
 * in the game's memory as NVN ticks (625/384 ns) on the CPU clock's scale. The game waits for
 * the frame's fence before it reads them. */
#define STAMPS_PER_CTX 32
static VkQueryPool game_stamps;
static int stamps_used[NCTX];
static struct { uint64_t serial, address; uint32_t query; } pending_stamps[NCTX * STAMPS_PER_CTX];
static int npending_stamps;
static pthread_mutex_t stamps_lock = PTHREAD_MUTEX_INITIALIZER;
static double stamp_offset_ns;                     /* CPU clock - GPU clock, from the first one read */
static int stamp_offset_set;

static void init(void);
static void *recycler(void *arg);

int render_guest_timestamp(uint64_t address) {
    VkCommandBuffer cmd = render_cmd();
    if (!game_stamps || !vk.limits.timestampComputeAndGraphics || stamps_used[cur] >= STAMPS_PER_CTX) return -1;
    uint32_t q = (uint32_t)cur * STAMPS_PER_CTX + (uint32_t)stamps_used[cur]++;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, game_stamps, q);
    pthread_mutex_lock(&stamps_lock);
    pending_stamps[npending_stamps++] = (typeof(pending_stamps[0])){ submitted + 1, address, q };
    pthread_mutex_unlock(&stamps_lock);
    return 0;
}

void render_timestamps_update(void) {
    if (!npending_stamps) return;
    pthread_mutex_lock(&stamps_lock);
    uint64_t done = render_serial_completed();
    int kept = 0;
    for (int i = 0; i < npending_stamps; i++) {
        if (pending_stamps[i].serial > done) { pending_stamps[kept++] = pending_stamps[i]; continue; }
        uint64_t v = 0;
        if (vkGetQueryPoolResults(vk.device, game_stamps, pending_stamps[i].query, 1, 8, &v, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            continue;
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        double gpu_ns = (double)v * vk.limits.timestampPeriod;
        if (!stamp_offset_set) { stamp_offset_ns = (double)t.tv_sec * 1e9 + t.tv_nsec - gpu_ns; stamp_offset_set = 1; }
        uint64_t ticks = (uint64_t)((gpu_ns + stamp_offset_ns) * 384.0 / 625.0);
        uint64_t report[2] = { 0, ticks };
        memcpy(gptr(pending_stamps[i].address), report, 16);
    }
    npending_stamps = kept;
    pthread_mutex_unlock(&stamps_lock);
}

static void init(void) {
    for (int i = 0; i < NCTX; i++) {
        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = vk.queue_family };
        VK_CHECK(vkCreateCommandPool(vk.device, &pci, NULL, &ctx[i].pool));
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                           .commandPool = ctx[i].pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                           .commandBufferCount = 1 };
        VK_CHECK(vkAllocateCommandBuffers(vk.device, &ai, &ctx[i].cmd));
        ctx[i].clean = 1;
    }
    pthread_t t;
    pthread_create(&t, NULL, recycler, NULL);
    pthread_detach(t);
    VkSemaphoreTypeCreateInfo ti = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                     .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0 };
    VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti };
    VK_CHECK(vkCreateSemaphore(vk.device, &si, NULL, &timeline));
    staging_ptr = host_buffer(STAGING_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &staging);
    uniform_ptr = host_buffer((uint64_t)UNIFORM_RING * NCTX, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &uniform_buffer);
    VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                 .queryCount = NCTX * STAMPS_PER_CTX };
    if (vk.limits.timestampComputeAndGraphics) VK_CHECK(vkCreateQueryPool(vk.device, &qi, NULL, &game_stamps));
    ready = 1;
}

/* ---- serials ---- */

uint64_t render_serial_completed(void) {
    uint64_t v = 0;
    if (ready) VK_CHECK(vkGetSemaphoreCounterValue(vk.device, timeline, &v));
    return v;
}

int render_serial_done(uint64_t serial) {
    return serial <= __atomic_load_n(&submitted, __ATOMIC_ACQUIRE) && render_serial_completed() >= serial;
}

/* Wait until the GPU has finished submission `serial` (at most timeout_ns); 0 if it has */
int render_wait_serial(uint64_t serial, uint64_t timeout_ns) {
    if (!ready || serial == 0) return 0;
    /* fenced in a submission another thread is still recording (under the GPU lock): it is
     * submitted at the end of that nvn_execute */
    for (uint64_t waited = 0; serial > __atomic_load_n(&submitted, __ATOMIC_ACQUIRE); waited += 100000) {
        if (waited >= timeout_ns) return -1;
        struct timespec t = { 0, 100000 };
        nanosleep(&t, NULL);
    }
    VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                               .pSemaphores = &timeline, .pValues = &serial };
    double t0 = now_ms();
    VkResult r = vkWaitSemaphores(vk.device, &wi, timeout_ns);
    render_gpu_wait_ms += now_ms() - t0;
    if (r == VK_TIMEOUT) return -1;
    VK_CHECK(r);
    render_timestamps_update();
    return 0;
}

/* The serial that work recorded now will be finished with: this submission's when one is being
 * recorded, else the last one */
uint64_t render_fence_serial(void) { return recording ? submitted + 1 : submitted; }

/* ---- LAA_GPU_TIMING_AT=frame: how long the GPU took for each command of that frame ----
 * A timestamp after each command (render_timestamp); the time between two is the later
 * command's. Every command is followed by a full barrier here, so they run one after another.
 * While timing, each submission is waited for (render_submit), so the results can be read. */
#define MAX_STAMPS 4096
int render_gpu_timing;                           /* the frame being recorded is timed */
static VkQueryPool stamp_pool;
static int nstamps;
static char stamp_label[MAX_STAMPS][48];
static struct { char label[48]; double ms; int count; } timing[512];
static int ntiming;

static void start_stamps(VkCommandBuffer cmd) {
    if (!stamp_pool) {
        VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                     .queryCount = MAX_STAMPS };
        VK_CHECK(vkCreateQueryPool(vk.device, &qi, NULL, &stamp_pool));
    }
    vkCmdResetQueryPool(cmd, stamp_pool, 0, MAX_STAMPS);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stamp_pool, 0);
    snprintf(stamp_label[0], sizeof stamp_label[0], "(start)");
    nstamps = 1;
}

void render_timestamp(const char *label) {
    if (!render_gpu_timing || !recording || nstamps >= MAX_STAMPS) return;
    vkCmdWriteTimestamp(ctx[cur].cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stamp_pool, (uint32_t)nstamps);
    snprintf(stamp_label[nstamps], sizeof stamp_label[0], "%s", label);
    nstamps++;
}

static void collect_stamps(void) {                /* (after the GPU finished the submission) */
    if (nstamps < 2) { nstamps = 0; return; }
    static uint64_t t[MAX_STAMPS];
    VK_CHECK(vkGetQueryPoolResults(vk.device, stamp_pool, 0, (uint32_t)nstamps, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT));
    for (int i = 1; i < nstamps; i++) {
        double ms = (double)(t[i] - t[i - 1]) * vk.limits.timestampPeriod / 1e6;
        int k = 0;
        while (k < ntiming && strcmp(timing[k].label, stamp_label[i])) k++;
        if (k == ntiming && ntiming < 512) snprintf(timing[ntiming++].label, sizeof timing[0].label, "%s", stamp_label[i]);
        if (k < ntiming) { timing[k].ms += ms; timing[k].count++; }
    }
    nstamps = 0;
}

static int by_time(const void *a, const void *b) {
    double d = ((const typeof(timing[0]) *)b)->ms - ((const typeof(timing[0]) *)a)->ms;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

void render_gpu_timing_frame(uint64_t frame) {    /* (render_present, after the frame's last submission) */
    static int64_t at = -2;
    static int passes_before;
    if (at == -2) at = getenv("LAA_GPU_TIMING_AT") ? strtoll(getenv("LAA_GPU_TIMING_AT"), NULL, 0) : -1;
    if (render_gpu_timing) {
        render_gpu_timing = 0;
        double total = 0;
        for (int k = 0; k < ntiming; k++) total += timing[k].ms;
        qsort(timing, (size_t)ntiming, sizeof timing[0], by_time);
        fprintf(stderr, "render: GPU time of frame %llu: %.2f ms, %d render passes\n", (unsigned long long)frame, total,
                render_passes - passes_before);
        for (int k = 0; k < ntiming && k < 25; k++)
            fprintf(stderr, "render:   %7.3f ms  %3dx  %s\n", timing[k].ms, timing[k].count, timing[k].label);
        ntiming = 0;
    }
    if (at >= 0 && frame + 1 == (uint64_t)at) { render_gpu_timing = 1; passes_before = render_passes; }
}

/* ---- recording and submitting ---- */

/* ---- the recycler: once the GPU has finished a context's submission, a thread of its own
 * resets that context's command and descriptor pools (resetting them was ~6% of the game
 * thread, and with the work submitted in pieces it happens several times a frame) ---- */
static struct { int ctx; uint64_t serial; } recycle_queue[NCTX];
static unsigned recycle_head, recycle_tail;
static pthread_mutex_t recycle_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t recycle_cond = PTHREAD_COND_INITIALIZER;

static void *recycler(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&recycle_lock);
        while (recycle_head == recycle_tail) pthread_cond_wait(&recycle_cond, &recycle_lock);
        int i = recycle_queue[recycle_tail % NCTX].ctx;
        uint64_t serial = recycle_queue[recycle_tail % NCTX].serial;
        recycle_tail++;
        pthread_mutex_unlock(&recycle_lock);
        VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                   .pSemaphores = &timeline, .pValues = &serial };
        VK_CHECK(vkWaitSemaphores(vk.device, &wi, UINT64_MAX));
        VK_CHECK(vkResetCommandPool(vk.device, ctx[i].pool, 0));
        render_submission_done(i);
        pthread_mutex_lock(&recycle_lock);
        ctx[i].clean = 1;
        pthread_cond_broadcast(&recycle_cond);
        pthread_mutex_unlock(&recycle_lock);
    }
    return NULL;
}

VkCommandBuffer render_cmd(void) {
    if (!ready) init();
    Context *c = &ctx[cur];
    if (!recording) {
        /* this context's last submission must be finished (and its pools reset) before reuse */
        pthread_mutex_lock(&recycle_lock);
        if (!c->clean) {
            double t0 = now_ms();
            while (!c->clean) pthread_cond_wait(&recycle_cond, &recycle_lock);
            render_gpu_wait_ms += now_ms() - t0;
        }
        c->clean = 0;
        pthread_mutex_unlock(&recycle_lock);
        render_timestamps_update();                /* (its timestamps are read before the reset below) */
        c->staging_used = c->uniform_used = 0;
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        VK_CHECK(vkBeginCommandBuffer(c->cmd, &bi));
        recording = 1;
        if (game_stamps) vkCmdResetQueryPool(c->cmd, game_stamps, (uint32_t)cur * STAMPS_PER_CTX, STAMPS_PER_CTX);
        stamps_used[cur] = 0;
        if (render_gpu_timing) start_stamps(c->cmd);
    }
    return c->cmd;
}

int render_context(void) { return cur; }

static uint32_t work;                              /* draws and dispatches recorded since the last submission */
void render_count_work(void) { work++; }
uint32_t render_recorded_work(void) { return recording ? work : 0; }

void render_submit(void) {
    if (!recording) return;
    work = 0;
    render_end_rendering();
    Context *c = &ctx[cur];
    render_barrier(c->cmd);                        /* the next submission's work comes after all of this */
    VK_CHECK(vkEndCommandBuffer(c->cmd));
    recording = 0;
    uint64_t serial = submitted + 1;
    VkTimelineSemaphoreSubmitInfo ts = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                         .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &serial };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts, .commandBufferCount = 1,
                        .pCommandBuffers = &c->cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &timeline };
    pthread_mutex_lock(&render_queue_lock);
    VK_CHECK(vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE));
    pthread_mutex_unlock(&render_queue_lock);
    c->serial = serial;
    __atomic_store_n(&submitted, serial, __ATOMIC_RELEASE);
    pthread_mutex_lock(&recycle_lock);             /* (reset it once the GPU is done with it) */
    recycle_queue[recycle_head % NCTX].ctx = cur;
    recycle_queue[recycle_head % NCTX].serial = serial;
    recycle_head++;
    pthread_cond_signal(&recycle_cond);
    pthread_mutex_unlock(&recycle_lock);
    cur = (cur + 1) % NCTX;
    if (nstamps) {                                 /* (timing: read the timestamps right away) */
        render_wait_serial(serial, UINT64_MAX);
        collect_stamps();
    }
}

void render_flush(void) {
    render_submit();
    render_wait_serial(submitted, UINT64_MAX);
}

void render_barrier(VkCommandBuffer c) {
    VkMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         1, &b, 0, NULL, 0, NULL);
}

/* Space in the current context's part of the staging buffer. When it is full, the work so far
 * is submitted and recording goes on in the next context (whose part is free once its last
 * submission is done). */
void *render_staging(uint64_t size, uint64_t *offset) {
    const uint64_t part = STAGING_SIZE / NCTX;
    size = (size + 255) & ~255ull;
    if (size > part) { fprintf(stderr, "render: %llu bytes don't fit the staging buffer\n", (unsigned long long)size); abort(); }
    render_cmd();
    if (ctx[cur].staging_used + size > part) { render_submit(); render_cmd(); }
    *offset = (uint64_t)cur * part + ctx[cur].staging_used;
    ctx[cur].staging_used += size;
    return staging_ptr + *offset;
}

VkBuffer render_staging_buffer(void) { return staging; }

/* A ring of host-visible memory for small uniform buffers made per draw (per context) */
void *render_uniforms(uint32_t size, VkBuffer *buffer, VkDeviceSize *offset) {
    uint64_t align = vk.limits.minUniformBufferOffsetAlignment ? vk.limits.minUniformBufferOffsetAlignment : 256;
    render_cmd();
    uint64_t at = (ctx[cur].uniform_used + align - 1) / align * align;
    if (at + size > UNIFORM_RING) { render_submit(); render_cmd(); at = 0; }
    ctx[cur].uniform_used = at + size;
    *buffer = uniform_buffer;
    *offset = (uint64_t)cur * UNIFORM_RING + at;
    return uniform_ptr + *offset;
}
