/*
 * audio.c - "audren:u", the audio renderer, and the IAudioDevice it hands out; "audout:u",
 * plain audio output (a queue of sample buffers; used e.g. for movie soundtracks).
 *
 * Every audio frame (5 ms: 240 samples at 48 kHz) the game's audio thread waits for the
 * renderer's system event, then sends RequestUpdate: the state of all its voices, memory
 * pools, effects, mixes and sinks. The reply reports each one's status, in this order:
 *
 *   header (0x40)  revision, size of each section below, total size
 *   memory pools   0x10 each: state (RequestAttach -> Attached, RequestDetach -> Detached)
 *   voices         0x10 each: samples played, wave buffers played
 *   effects        0x10 each (0x90 from REV9): enabled / disabled
 *   sinks          0x20 each
 *   performance    0x10
 *   error info     0xB0: 10 error slots and a count
 *   renderer info  0x10 (REV5+): frames rendered
 *
 * There is no sound yet, but voices still "play" in real time: each one consumes its wave
 * buffers (blocks of samples the game queues, 4 slots per voice) at its sample rate x pitch,
 * and reports the samples and buffers played. Games use this as a clock (the movie player
 * shows the frame matching its soundtrack's position). Real playback (SDL3) is phase 5.
 * Layouts follow Ryujinx (Ryujinx.Audio/Renderer, StateUpdater.cs); commands from
 * switchbrew.org/wiki/Audio_services.
 */
#include "services.h"
#include "kernel.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REV_MAGIC 0x00564552u                /* "REV" + digit in the top byte */
#define REVISION(r) ((int)(((r) >> 24) - '0'))

enum { POOL_REQUEST_DETACH = 2, POOL_DETACHED = 3, POOL_REQUEST_ATTACH = 4, POOL_ATTACHED = 5 };
enum { EFFECT_ENABLED = 3, EFFECT_DISABLED = 4 };

/* AudioRendererParameter, as the game sends it (0x34 bytes) */
typedef struct {
    uint32_t sample_rate, sample_count, mix_buffers, sub_mixes, voices, sinks, effects, perf_frames;
    uint8_t voice_drop, reserved, device, execution_mode;
    uint32_t splitters, splitter_destinations, external_context_size;
    uint32_t revision;
} RendererParams;

/* The server side of one voice: which of its 4 wave buffer slots hold unplayed samples,
 * the slot being played and how far into it we are. */
enum { VOICE_STOPPED, VOICE_STARTED, VOICE_PAUSED };
typedef struct {
    uint8_t valid[4], loop[4], end_of_stream[4];
    uint32_t length[4];                     /* samples in each slot */
    int state, index;
    uint64_t offset, played;
    uint32_t consumed;
    double fraction;                        /* part of a sample left over from the last advance */
} Voice;

typedef struct {
    RendererParams p;
    uint32_t pools;
    uint32_t *pool_state;
    Voice *voices;
    uint64_t elapsed_frames, updated_frames; /* frames rendered; value at the last update */
    int running;
    KEvent *event;
} Renderer;

/* ---- audio outputs (audout:u): buffers are "played" in real time, then released ---- */
#define OUT_QUEUE 32
typedef struct {
    uint32_t sample_rate, channels;
    int running;
    struct { uint64_t tag, frames; } queue[OUT_QUEUE];   /* appended, not yet played */
    int queued;
    uint64_t released[OUT_QUEUE];                         /* played, not yet collected */
    int n_released;
    uint64_t played_frames, position;                     /* position: in the front buffer */
    KEvent *event;
} AudioOut;

/* ---- the 5 ms frame timer: signals every running renderer's event, plays audio outs ---- */
#define MAX_RENDERERS 4
#define MAX_OUTS 4
static Renderer *renderers[MAX_RENDERERS];
static AudioOut *outs[MAX_OUTS];
static pthread_mutex_t renderers_lock = PTHREAD_MUTEX_INITIALIZER;

static void play_out(AudioOut *o, uint64_t frames) {    /* advance by `frames` sample frames */
    int released = 0;
    while (o->queued && frames) {
        uint64_t left = o->queue[0].frames - o->position, n = frames < left ? frames : left;
        o->position += n;
        o->played_frames += n;
        frames -= n;
        if (o->position == o->queue[0].frames) {
            if (o->n_released < OUT_QUEUE) o->released[o->n_released++] = o->queue[0].tag;
            memmove(o->queue, o->queue + 1, --o->queued * sizeof o->queue[0]);
            o->position = 0;
            released = 1;
        }
    }
    if (released) kevent_signal(o->event);
}

static void *frame_thread(void *arg) {
    (void)arg;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += 5000000;
        if (next.tv_nsec >= 1000000000) { next.tv_sec++; next.tv_nsec -= 1000000000; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        pthread_mutex_lock(&renderers_lock);
        for (int k = 0; k < MAX_RENDERERS; k++) {
            if (renderers[k] && renderers[k]->running) {
                renderers[k]->elapsed_frames++;
                kevent_signal(renderers[k]->event);
            }
        }
        for (int k = 0; k < MAX_OUTS; k++)
            if (outs[k] && outs[k]->running) play_out(outs[k], outs[k]->sample_rate / 200);   /* 5 ms */
        pthread_mutex_unlock(&renderers_lock);
    }
    return NULL;
}

static void read_params(const IpcCall *call, RendererParams *p) {
    memset(p, 0, sizeof *p);
    memcpy(p, call->in, call->in_size < sizeof *p ? call->in_size : sizeof *p);
}

/* The work buffer the game must lend us. We don't use it; this is a simplified form of
 * Ryujinx's estimate, so the game sets aside a realistic amount. */
static uint64_t work_buffer_size(const RendererParams *p) {
    uint64_t mixes = p->sub_mixes + 1, pools = p->effects + 4ull * p->voices;
    uint64_t size = 4ull * p->sample_count * (6 + p->mix_buffers);            /* mix buffers */
    size += 4ull * 160 * (6 + p->mix_buffers) * (p->sinks + p->sub_mixes);    /* upsampler */
    size += 0x400ull * p->voices + 0x940ull * mixes + 0x20ull * pools;        /* voices, mixes, pools */
    size += 0x100ull * p->splitters + 0xE0ull * p->splitter_destinations;
    size += 0x1000ull * (p->perf_frames + 1);                                 /* performance */
    return (size + 0x10000 + 0xFFF) & ~0xFFFull;
}

/* ---- IAudioRenderer ---- */
static void renderer_destroy(ServiceObject *self) {
    Renderer *r = self->state;
    pthread_mutex_lock(&renderers_lock);
    for (int k = 0; k < MAX_RENDERERS; k++) if (renderers[k] == r) renderers[k] = NULL;
    pthread_mutex_unlock(&renderers_lock);
    free(r->pool_state);
    free(r->voices);
    free(r);
}

/* VoiceInParameter1 (0x170 bytes), the fields we use */
#define VOICE_IN_SIZE 0x170
enum { VIN_IS_NEW = 0x8, VIN_IN_USE = 0x9, VIN_PLAY_STATE = 0xA, VIN_SAMPLE_RATE = 0xC, VIN_PITCH = 0x1C,
       VIN_WAVE_BUFFERS = 0x60, VIN_FLUSH_COUNT = 0x159 };
/* its 4 wave buffers, 0x38 bytes each */
enum { WB_SIZE = 0x38, WB_START = 0x10, WB_END = 0x14, WB_LOOP = 0x18, WB_END_OF_STREAM = 0x19,
       WB_SENT_TO_SERVER = 0x1A };

static void voice_finish_buffer(Voice *v) {          /* the current slot is done: move to the next */
    if (v->end_of_stream[v->index]) v->played = 0;
    v->valid[v->index] = 0;
    v->index = (v->index + 1) % 4;
    v->consumed++;
    v->offset = 0;
}

/* Take in one voice's parameters, then play it for `frames` audio frames (5 ms each). */
static void voice_update(Voice *v, const uint8_t *in, uint64_t frames) {
    if (in[VIN_IS_NEW]) memset(v, 0, sizeof *v);
    for (int i = 0; i < 4; i++) {                     /* buffers the game appended since last time */
        const uint8_t *wb = in + VIN_WAVE_BUFFERS + WB_SIZE * i;
        if (wb[WB_SENT_TO_SERVER]) continue;
        uint32_t start, end;
        memcpy(&start, wb + WB_START, 4);
        memcpy(&end, wb + WB_END, 4);
        v->valid[i] = 1;
        v->length[i] = end > start ? end - start : 0;
        v->loop[i] = wb[WB_LOOP];
        v->end_of_stream[i] = wb[WB_END_OF_STREAM];
    }
    for (int n = in[VIN_FLUSH_COUNT]; n > 0 && v->valid[v->index]; n--) voice_finish_buffer(v);

    switch (in[VIN_PLAY_STATE]) {                     /* 0 Start, 1 Stop, 2 Pause */
    case 0: v->state = VOICE_STARTED; break;
    case 2: v->state = VOICE_PAUSED; break;
    case 1:
        if (v->state != VOICE_STOPPED) {              /* stopping drops every queued buffer */
            for (int i = 0; i < 4; i++)
                if (v->valid[i]) { v->valid[i] = 0; v->index = (v->index + 1) % 4; v->consumed++; }
            v->offset = v->played = 0;
            v->fraction = 0;
            v->state = VOICE_STOPPED;
        }
        break;
    }
    if (v->state != VOICE_STARTED) return;

    uint32_t rate;
    float pitch;
    memcpy(&rate, in + VIN_SAMPLE_RATE, 4);
    memcpy(&pitch, in + VIN_PITCH, 4);
    if (!(pitch > 0 && pitch < 16)) pitch = 1;        /* also catches NaN */
    double samples = v->fraction + (double)frames * rate * pitch / 200;
    uint64_t left = (uint64_t)samples;
    v->fraction = samples - left;
    while (left && v->valid[v->index]) {
        uint64_t len = v->length[v->index], n = len - v->offset < left ? len - v->offset : left;
        v->offset += n;
        v->played += n;
        left -= n;
        if (v->offset >= len) {
            if (v->loop[v->index] && len) v->offset = 0;
            else voice_finish_buffer(v);
        }
    }
    if (!v->valid[v->index]) v->fraction = 0;         /* starved: nothing to carry over */
}

static void request_update(Renderer *r, IpcCall *call) {
    IpcBuffer ib = ipc_in_buffer(call, 0), ob = ipc_out_buffer(call, 0);
    const uint8_t *in = ipc_guest_ptr(ib.addr, ib.size);
    uint8_t *out = ipc_guest_ptr(ob.addr, ob.size);
    if (!in || !out || ib.size < 0x40) { call->result = MAKE_RESULT(153, 41); return; }
    uint32_t hdr[16];
    memcpy(hdr, in, 0x40);                 /* revision, behaviour, pools, voices, resources, effects... */
    uint32_t revision = hdr[0];
    int rev = REVISION(revision);

    /* inputs we look at: memory pool states, effect enable flags */
    uint64_t at = 0x40 + hdr[1];           /* skip header, behaviour */
    const uint8_t *pools_in = in + at;
    const uint8_t *voices_in = in + at + hdr[2] + hdr[4];
    at += hdr[2] + hdr[4] + hdr[3];        /* pools, voice channel resources, voices */
    const uint8_t *effects_in = in + at;
    uint32_t effect_in_size = r->p.effects ? hdr[5] / r->p.effects : 0;

    uint32_t effect_out_size = rev >= 9 ? 0x90 : 0x10;
    uint64_t total = 0x40 + 0x10ull * r->pools + 0x10ull * r->p.voices + (uint64_t)effect_out_size * r->p.effects +
                     0x20ull * r->p.sinks + 0x10 + 0xB0 + (rev >= 5 ? 0x10 : 0);
    if (total > ob.size || 0x40ull + hdr[1] + hdr[2] + hdr[4] + hdr[3] + hdr[5] > ib.size) {
        fprintf(stderr, "audren: update buffers too small (need 0x%llx, have 0x%llx)\n",
                (unsigned long long)total, (unsigned long long)ob.size);
        call->result = MAKE_RESULT(153, 41);
        return;
    }
    memset(out, 0, total);
    uint8_t *o = out + 0x40;
    for (uint32_t k = 0; k < r->pools; k++, o += 0x10) {       /* memory pools */
        uint32_t state;
        memcpy(&state, pools_in + 0x20 * k + 0x10, 4);
        if (state == POOL_REQUEST_ATTACH) r->pool_state[k] = POOL_ATTACHED;
        else if (state == POOL_REQUEST_DETACH) r->pool_state[k] = POOL_DETACHED;
        memcpy(o, &r->pool_state[k], 4);
    }
    pthread_mutex_lock(&renderers_lock);
    uint64_t frames = r->elapsed_frames - r->updated_frames;   /* rendered since the last update */
    r->updated_frames = r->elapsed_frames;
    pthread_mutex_unlock(&renderers_lock);
    uint32_t nvoices = hdr[3] / VOICE_IN_SIZE < r->p.voices ? hdr[3] / VOICE_IN_SIZE : r->p.voices;
    for (uint32_t k = 0; k < nvoices; k++) {                   /* voices: samples, buffers played */
        const uint8_t *vin = voices_in + (uint64_t)VOICE_IN_SIZE * k;
        if (!vin[VIN_IN_USE]) continue;
        Voice *v = &r->voices[k];
        voice_update(v, vin, frames);
        if (!vin[VIN_IS_NEW]) {
            memcpy(o + 0x10 * k, &v->played, 8);
            memcpy(o + 0x10 * k + 8, &v->consumed, 4);
        }
    }
    o += 0x10ull * r->p.voices;
    for (uint32_t k = 0; k < r->p.effects; k++, o += effect_out_size)
        o[0] = effects_in[(uint64_t)effect_in_size * k + 2] ? EFFECT_ENABLED : EFFECT_DISABLED;
    o += 0x20ull * r->p.sinks + 0x10 + 0xB0;                   /* sinks, performance, error info */
    if (rev >= 5) memcpy(o, &r->elapsed_frames, 8);

    uint32_t out_hdr[16] = { 0 };
    out_hdr[0] = revision;
    out_hdr[1] = 0xB0;                                         /* behaviour = error info */
    out_hdr[2] = 0x10 * r->pools;
    out_hdr[3] = 0x10 * r->p.voices;
    out_hdr[5] = effect_out_size * r->p.effects;
    out_hdr[7] = 0x20 * r->p.sinks;
    out_hdr[8] = 0x10;                                         /* performance */
    out_hdr[10] = rev >= 5 ? 0x10 : 0;                         /* renderer info */
    out_hdr[15] = (uint32_t)total;
    memcpy(out, out_hdr, 0x40);
    kevent_clear(r->event);
}

static void renderer_dispatch(ServiceObject *self, IpcCall *call) {
    Renderer *r = self->state;
    switch (call->cmd) {
    case 0: ipc_out32(call, r->p.sample_rate); break;          /* GetSampleRate */
    case 1: ipc_out32(call, r->p.sample_count); break;         /* GetSampleCount */
    case 2: ipc_out32(call, r->p.mix_buffers); break;          /* GetMixBufferCount */
    case 3: ipc_out32(call, r->running ? 0 : 1); break;        /* GetState: 0 started, 1 stopped */
    case 4: case 10:                                           /* RequestUpdate(Auto) */
        request_update(r, call);
        break;
    case 5: r->running = 1; break;                             /* Start */
    case 6: r->running = 0; break;                             /* Stop */
    case 7:                                                    /* QuerySystemEvent */
        ipc_out_handle(call, kevent_readable_handle(r->event), 0);
        break;
    case 8: break;                                             /* SetRenderingTimeLimit */
    case 9: ipc_out32(call, 100); break;                       /* GetRenderingTimeLimit */
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType renderer_type = { "audren:IAudioRenderer", renderer_dispatch, renderer_destroy };

/* ---- IAudioDevice: the output device ("speaker") ---- */
static void device_name(IpcCall *call) {
    IpcBuffer b = ipc_out_buffer(call, 0);
    char *p = ipc_guest_ptr(b.addr, b.size);
    if (p && b.size >= 0x100) { memset(p, 0, 0x100); strcpy(p, "AudioTvOutput"); }
}

static void device_dispatch(ServiceObject *self, IpcCall *call) {
    float one = 1.0f;
    switch (call->cmd) {
    case 0: case 6:                                            /* ListAudioDeviceName(Auto) -> count */
        device_name(call);
        ipc_out32(call, 1);
        break;
    case 1: case 7:                                            /* SetAudioDeviceOutputVolume(Auto) */
        break;
    case 2: case 8:                                            /* GetAudioDeviceOutputVolume(Auto) */
        ipc_out_bytes(call, &one, 4);
        break;
    case 3: case 10:                                           /* GetActiveAudioDeviceName(Auto) */
        device_name(call);
        break;
    case 4: case 11: case 12:                                  /* Query...Event */
        ipc_out_handle(call, kevent_readable_handle(kevent_new("audren: device event")), 0);
        break;
    case 5:                                                    /* GetActiveChannelCount */
        ipc_out32(call, 2);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType device_type = { "audren:IAudioDevice", device_dispatch, NULL };

/* ---- audren:u ---- */
static void audren_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0: {                                                  /* OpenAudioRenderer(params, ...) */
        Renderer *r = calloc(1, sizeof *r);
        if (!r) exit(1);
        read_params(call, &r->p);
        r->pools = r->p.effects + 4 * r->p.voices;
        r->pool_state = calloc(r->pools + 1, sizeof *r->pool_state);
        r->voices = calloc(r->p.voices + 1, sizeof *r->voices);
        r->event = kevent_new("audren: renderer frame");
        if (!r->pool_state || !r->voices) exit(1);
        pthread_mutex_lock(&renderers_lock);
        int k = 0;
        while (k < MAX_RENDERERS && renderers[k]) k++;
        if (k < MAX_RENDERERS) renderers[k] = r;
        pthread_mutex_unlock(&renderers_lock);
        if (k == MAX_RENDERERS) { fprintf(stderr, "audren: too many renderers\n"); exit(1); }
        if (ipc_trace)
            fprintf(stderr, "audren: open REV%d, %u Hz, %u voices, %u effects, %u sinks, %u mix buffers\n",
                    REVISION(r->p.revision), r->p.sample_rate, r->p.voices, r->p.effects, r->p.sinks, r->p.mix_buffers);
        ipc_out_object(call, ipc_new_object(&renderer_type, r));
        break;
    }
    case 1: {                                                  /* GetWorkBufferSize(params) -> u64 */
        RendererParams p;
        read_params(call, &p);
        ipc_out64(call, work_buffer_size(&p));
        break;
    }
    case 2: case 4:                                            /* GetAudioDeviceService(WithRevisionInfo) */
        ipc_out_object(call, ipc_new_object(&device_type, NULL));
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType audren_type = { "audren:u", audren_dispatch, NULL };

ServiceObject *service_audren_create(void) {
    static int started;
    if (!started) {
        started = 1;
        pthread_t t;
        pthread_create(&t, NULL, frame_thread, NULL);
        pthread_detach(t);
    }
    return ipc_new_object(&audren_type, NULL);
}

/* ---- IAudioOut ---- */
static void out_destroy(ServiceObject *self) {
    pthread_mutex_lock(&renderers_lock);
    for (int k = 0; k < MAX_OUTS; k++) if (outs[k] == self->state) outs[k] = NULL;
    pthread_mutex_unlock(&renderers_lock);
    free(self->state);
}

static void out_dispatch(ServiceObject *self, IpcCall *call) {
    AudioOut *o = self->state;
    pthread_mutex_lock(&renderers_lock);
    switch (call->cmd) {
    case 0: ipc_out32(call, o->running ? 0 : 1); break;      /* GetAudioOutState: 0 started, 1 stopped */
    case 1: o->running = 1; break;                           /* Start */
    case 2: o->running = 0; break;                           /* Stop */
    case 3: case 7: {                                        /* AppendAudioOutBuffer(Auto)(tag, buffer info) */
        /* AudioOutBuffer: next, sample buffer, capacity, size (bytes), offset */
        IpcBuffer b = ipc_in_buffer(call, 0);
        const uint8_t *p = ipc_guest_ptr(b.addr, 0x28);
        uint64_t size = 0;
        if (p && b.size >= 0x28) memcpy(&size, p + 0x18, 8);
        if (o->queued == OUT_QUEUE) { call->result = MAKE_RESULT(153, 8); break; }
        o->queue[o->queued].tag = ipc_in64(call, 0);
        o->queue[o->queued].frames = size / (2 * (o->channels ? o->channels : 2));   /* 16-bit samples */
        if (o->queue[o->queued].frames == 0) o->queue[o->queued].frames = 1;
        o->queued++;
        break;
    }
    case 4:                                                  /* RegisterBufferEvent */
        ipc_out_handle(call, kevent_readable_handle(o->event), 0);
        break;
    case 5: case 8: {                                        /* GetReleasedAudioOutBuffers(Auto) -> count, tags */
        IpcBuffer b = ipc_out_buffer(call, 0);
        uint8_t *p = ipc_guest_ptr(b.addr, b.size);
        uint32_t n = 0;
        while (p && n < (uint32_t)o->n_released && (n + 1) * 8 <= b.size) { memcpy(p + 8 * n, &o->released[n], 8); n++; }
        memmove(o->released, o->released + n, (o->n_released - n) * sizeof *o->released);
        o->n_released -= (int)n;
        if (p && (n + 1) * 8 <= b.size) memset(p + 8 * n, 0, 8);
        ipc_out32(call, n);
        if (!o->n_released) kevent_clear(o->event);
        break;
    }
    case 6: {                                                /* ContainsAudioOutBuffer(tag) -> bool */
        uint64_t tag = ipc_in64(call, 0);
        uint32_t found = 0;
        for (int k = 0; k < o->queued; k++) found |= o->queue[k].tag == tag;
        ipc_out32(call, found);
        break;
    }
    case 9: ipc_out32(call, (uint32_t)o->queued); break;     /* GetAudioOutBufferCount */
    case 10: ipc_out64(call, o->played_frames); break;       /* GetAudioOutPlayedSampleCount */
    case 11:                                                 /* FlushAudioOutBuffers -> bool */
        play_out(o, ~0ull >> 1);
        ipc_out32(call, 1);
        break;
    case 12: break;                                          /* SetAudioOutVolume */
    case 13: { float one = 1.0f; ipc_out_bytes(call, &one, 4); break; }   /* GetAudioOutVolume */
    default:
        pthread_mutex_unlock(&renderers_lock);
        ipc_unknown_command(self, call);
    }
    pthread_mutex_unlock(&renderers_lock);
}
static const ServiceType out_type = { "audout:IAudioOut", out_dispatch, out_destroy };

/* ---- audout:u ---- */
static void audout_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0: case 2: {                                        /* ListAudioOuts(Auto) -> count, names */
        IpcBuffer b = ipc_out_buffer(call, 0);
        char *p = ipc_guest_ptr(b.addr, b.size);
        if (p && b.size >= 0x100) { memset(p, 0, 0x100); strcpy(p, "DeviceOut"); }
        ipc_out32(call, 1);
        break;
    }
    case 1: case 3: {                                        /* OpenAudioOut(Auto)(rate, channels, aruid; name) */
        AudioOut *o = calloc(1, sizeof *o);
        if (!o) exit(1);
        o->sample_rate = ipc_in32(call, 0) ? ipc_in32(call, 0) : 48000;
        o->channels = (ipc_in32(call, 4) & 0xFFFF) ? (ipc_in32(call, 4) & 0xFFFF) : 2;
        o->event = kevent_new("audout: buffer released");
        pthread_mutex_lock(&renderers_lock);
        int k = 0;
        while (k < MAX_OUTS && outs[k]) k++;
        if (k < MAX_OUTS) outs[k] = o;
        pthread_mutex_unlock(&renderers_lock);
        if (k == MAX_OUTS) { fprintf(stderr, "audout: too many outputs\n"); exit(1); }
        IpcBuffer b = ipc_out_buffer(call, 0);               /* the device name actually opened */
        char *p = ipc_guest_ptr(b.addr, b.size);
        if (p && b.size >= 0x100) { memset(p, 0, 0x100); strcpy(p, "DeviceOut"); }
        ipc_out32(call, o->sample_rate);                     /* AudioOutParams: rate, channels, */
        ipc_out32(call, o->channels);                        /* format (2 = PCM 16-bit), state */
        ipc_out32(call, 2);
        ipc_out32(call, 1);
        ipc_out_object(call, ipc_new_object(&out_type, o));
        break;
    }
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType audout_type = { "audout:u", audout_dispatch, NULL };

ServiceObject *service_audout_create(void) {
    service_audren_create();                                 /* (starts the 5 ms timer if needed) */
    return ipc_new_object(&audout_type, NULL);
}
