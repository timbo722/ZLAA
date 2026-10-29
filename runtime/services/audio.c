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
 * Between updates the renderer mixes one frame every 5 ms, like the real one minus effects
 * and splitters (this game doesn't need them):
 *   voices     their wave buffers (blocks of samples the game queues, 4 slots per voice:
 *              DSP-ADPCM, PCM16 or float) decoded and resampled by sample rate x pitch, times
 *              the voice's volume, added to its mix's buffers by its channel resources' volumes
 *   sub-mixes  added to their destination mix: volume x a [from buffer][to buffer] matrix
 *   final mix  (mix 0) x its volume -> the device sink, which picks 2 of its buffers for the
 *              speakers (or downmixes 6)
 * Each voice reports the samples and buffers played; games use this as a clock too (the movie
 * player shows the frame matching its soundtrack). Audio outs (audout:u) are added on top, and
 * the result goes to SDL3 (LAA_AUDIO_WAV=file.wav: also saved).
 * Layouts follow Ryujinx (Ryujinx.Audio/Renderer, StateUpdater.cs); commands from
 * switchbrew.org/wiki/Audio_services.
 */
#include "services.h"
#include "kernel.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef LAA_HAVE_SDL
#include <SDL3/SDL.h>
#endif

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

#define MAX_CHANNELS 6
#define MIX_BUFFERS 24                      /* most buffers a mix can have */
#define MAX_SAMPLES 1024                    /* most samples per audio frame we accept */
#define UNUSED_MIX 0x7FFFFFFF
enum { FORMAT_PCM16 = 2, FORMAT_FLOAT = 5, FORMAT_ADPCM = 6 };
enum { SINK_DEVICE = 1 };

/* one wave buffer slot, as the game sent it */
typedef struct {
    uint64_t address, size, context;        /* context: the ADPCM decoder state at its start */
    uint32_t start, end, loop_start, loop_end;   /* in samples */
    int32_t loop_count;                     /* -1: forever */
    uint8_t loop, end_of_stream;
} WaveBuffer;

/* The server side of one voice: its parameters from the last update, which of its 4 wave
 * buffer slots hold unplayed samples, the slot being played and how far into it we are. */
enum { VOICE_STOPPED, VOICE_STARTED, VOICE_PAUSED };
typedef struct {
    uint8_t format;
    uint32_t channels, rate, mix;
    float pitch, volume, previous_volume;   /* (the volume ramps from the previous one over a frame) */
    uint64_t coefficients;                  /* ADPCM: 8 pairs of predictor coefficients */
    int32_t resources[MAX_CHANNELS];        /* each channel's channel resource */
    WaveBuffer wb[4];
    uint8_t valid[4];
    int state, index, loops;                /* loops: times the current slot has looped */
    uint32_t offset;                        /* samples into the current slot */
    uint64_t played;
    uint32_t consumed;
    uint8_t pred_scale;                     /* ADPCM decoder: predictor << 4 | scale, last 2 samples */
    int16_t history[2];
    float last[2][MAX_CHANNELS];            /* the two source samples being interpolated between */
    double fraction;                        /* how far between them */
    struct { uint8_t on; float b[3], a[2]; } biquad[2];    /* the voice's 2 filters (EQ: games quieten */
    float biquad_state[2][MAX_CHANNELS][2];                /* and muffle distant sounds with them) */
} Voice;

typedef struct {
    int used;
    uint32_t buffers;
    uint32_t destination;
    float volume;
    float matrix[MIX_BUFFERS * MIX_BUFFERS];      /* [from * 24 + to] */
} Mix;

typedef struct {
    RendererParams p;
    uint32_t pools;
    uint32_t *pool_state;
    Voice *voices;
    float (*resources)[MIX_BUFFERS];        /* voice channel resources: volume into each mix buffer */
    Mix *mixes;                             /* p.sub_mixes + 1; mix 0 is the final mix */
    uint32_t nmixes;
    float *buffers;                         /* every mix's buffers: 24 x sample_count each */
    uint8_t sink_inputs[MAX_CHANNELS];      /* the device sink: which final mix buffers to play */
    uint32_t sink_input_count;
    int downmix;                            /* the game's own 5.1 -> stereo coefficients */
    float downmix_coefficients[4];
    uint64_t elapsed_frames;
    int running;
    KEvent *event;
} Renderer;

/* ---- audio outputs (audout:u): queued buffers of PCM16 samples, played, then released ---- */
#define OUT_QUEUE 32
typedef struct {
    uint32_t sample_rate, channels;
    int running;
    struct { uint64_t tag, data, frames; } queue[OUT_QUEUE];   /* appended, not yet played */
    int queued;
    uint64_t released[OUT_QUEUE];                         /* played, not yet collected */
    int n_released;
    uint64_t played_frames, position;                     /* position: in the front buffer */
    KEvent *event;
} AudioOut;

#define MAX_RENDERERS 4
#define MAX_OUTS 4
static Renderer *renderers[MAX_RENDERERS];
static AudioOut *outs[MAX_OUTS];
static pthread_mutex_t renderers_lock = PTHREAD_MUTEX_INITIALIZER;

/* Advance by `frames` sample frames; with `mix`, also add the samples to it (stereo, 48 kHz). */
static void play_out(AudioOut *o, uint64_t frames, float (*mix)[2]) {
    int released = 0;
    uint32_t channels = o->channels ? o->channels : 2;
    if (o->sample_rate != 48000) mix = NULL;             /* (only movies use this, at 48 kHz) */
    while (o->queued && frames) {
        uint64_t left = o->queue[0].frames - o->position, n = frames < left ? frames : left;
        const int16_t *s = mix ? ipc_guest_ptr(o->queue[0].data, o->queue[0].frames * channels * 2) : NULL;
        for (uint64_t i = 0; s && i < n; i++, mix++) {
            const int16_t *f = s + (o->position + i) * channels;
            (*mix)[0] += f[0];
            (*mix)[1] += f[channels > 1];
        }
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

/* ---- the speakers: SDL3, and LAA_AUDIO_WAV ---- */
#define OUT_FRAMES 240                      /* 5 ms at 48 kHz */
static FILE *wav;
static uint32_t wav_bytes;

static void wav_header(void) {
    uint32_t h[11] = { 0x46464952, 36 + wav_bytes, 0x45564157, 0x20746D66, 16, 0x00020001, 48000, 48000 * 4,
                       0x00100004, 0x61746164, wav_bytes };      /* RIFF, WAVE, fmt: PCM stereo 16-bit, data */
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 4, 11, wav);
    fseek(wav, 0, SEEK_END);
}

#ifdef LAA_HAVE_SDL
static SDL_AudioStream *volatile stream;                  /* set once the device is open */

/* Opening the device can take long or never finish (WSL's audio server hung it): on its own
 * thread, so the 5 ms clock, which the game's timing and movies follow, never waits for it */
static void *open_device(void *arg) {
    (void)arg;
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, 48000 };
    SDL_AudioStream *s = NULL;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO) || !(s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL))) {
        fprintf(stderr, "audio: no sound output: %s\n", SDL_GetError());
        return NULL;
    }
    static const int16_t silence[2 * 48000 / 25];        /* 40 ms head start against timer jitter */
    SDL_PutAudioStreamData(s, silence, sizeof silence);
    SDL_ResumeAudioStreamDevice(s);
    __atomic_store_n(&stream, s, __ATOMIC_RELEASE);
    return NULL;
}
#endif

static void output_open(void) {
    const char *path = getenv("LAA_AUDIO_WAV");
    if (path && (wav = fopen(path, "wb"))) wav_header();
#ifdef LAA_HAVE_SDL
    pthread_t t;
    if (pthread_create(&t, NULL, open_device, NULL) == 0) pthread_detach(t);
#endif
}

/* The game's mix is quiet (music around -33 dBFS RMS, peaks near -15 dB), so it is amplified
 * by LAA_VOLUME (default 3, about +10 dB); above 3/4 of full scale a soft limiter rounds peaks
 * off instead of clipping them. */
static void output_write(float (*mix)[2]) {
    static float gain = -1;
    if (gain < 0) gain = getenv("LAA_VOLUME") ? strtof(getenv("LAA_VOLUME"), NULL) : 3.0f;
    if (!(gain >= 0 && gain <= 100)) gain = 3.0f;
    int16_t pcm[OUT_FRAMES * 2];
    for (int i = 0; i < OUT_FRAMES * 2; i++) {
        float v = mix[i / 2][i % 2] * gain / 32768.0f, a = fabsf(v);
        if (a > 0.75f) v = copysignf(0.75f + 0.25f * tanhf((a - 0.75f) / 0.25f), v);
        pcm[i] = (int16_t)(v * 32767.0f);
    }
    if (wav) {
        fwrite(pcm, sizeof pcm, 1, wav);
        if ((wav_bytes += sizeof pcm) % (48000 * 4) == 0) { wav_header(); fflush(wav); }   /* every second */
    }
#ifdef LAA_HAVE_SDL
    /* more than 100 ms waiting: the timer ran ahead of the device; drop this frame */
    SDL_AudioStream *s = __atomic_load_n(&stream, __ATOMIC_ACQUIRE);
    if (s && SDL_GetAudioStreamQueued(s) < 48000 * 4 / 10) SDL_PutAudioStreamData(s, pcm, sizeof pcm);
#endif
}

static void renderer_render(Renderer *r, float (*mix)[2]);

/* ---- the 5 ms frame timer: renders every running renderer, plays audio outs, signals ---- */
static void *frame_thread(void *arg) {
    (void)arg;
    output_open();
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += 5000000;
        if (next.tv_nsec >= 1000000000) { next.tv_sec++; next.tv_nsec -= 1000000000; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        float mix[OUT_FRAMES][2] = { { 0 } };
        static int stats = -1;                       /* LAA_AUDIO_STATS=1: mixing time and voices, every 2 s */
        static double busy_us;
        static unsigned frames, voices, mixes, with_data, audible;
        if (stats < 0) stats = getenv("LAA_AUDIO_STATS") != NULL;
        struct timespec t0, t1;
        if (stats) clock_gettime(CLOCK_MONOTONIC, &t0);
        pthread_mutex_lock(&renderers_lock);
        for (int k = 0; k < MAX_RENDERERS; k++) {
            if (renderers[k] && renderers[k]->running) {
                renderers[k]->elapsed_frames++;
                renderer_render(renderers[k], mix);
                kevent_signal(renderers[k]->event);
                if (stats) {
                    for (uint32_t v = 0; v < renderers[k]->p.voices; v++) {
                        const Voice *x = &renderers[k]->voices[v];
                        if (x->state != VOICE_STARTED) continue;
                        voices++;
                        with_data += x->valid[x->index];
                        audible += x->valid[x->index] && (x->volume != 0 || x->previous_volume != 0);
                    }
                    mixes += renderers[k]->nmixes;
                }
            }
        }
        if (stats) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            busy_us += (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
            if (++frames == 400) {
                fprintf(stderr, "audio: %.0f us mixing per 5 ms frame (%.1f%% of a core), %.1f voices started, %.1f with "
                        "data, %.1f of them not silent, %u mixes\n", busy_us / frames, busy_us / frames / 50, (double)voices / frames,
                        (double)with_data / frames, (double)audible / frames, mixes / frames);
                busy_us = 0; frames = voices = mixes = with_data = audible = 0;
            }
        }
        for (int k = 0; k < MAX_OUTS; k++)
            if (outs[k] && outs[k]->running) play_out(outs[k], outs[k]->sample_rate / 200, mix);   /* 5 ms */
        pthread_mutex_unlock(&renderers_lock);
        output_write(mix);
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
    free(r->resources);
    free(r->mixes);
    free(r->buffers);
    free(r);
}

/* VoiceInParameter1 (0x170 bytes), the fields we use */
#define VOICE_IN_SIZE 0x170
enum { VIN_IS_NEW = 0x8, VIN_IN_USE = 0x9, VIN_PLAY_STATE = 0xA, VIN_SAMPLE_FORMAT = 0xB, VIN_SAMPLE_RATE = 0xC,
       VIN_CHANNELS = 0x18, VIN_PITCH = 0x1C, VIN_VOLUME = 0x20, VIN_BIQUADS = 0x24, VIN_DATA_SOURCE = 0x48, VIN_MIX_ID = 0x58,
       VIN_SPLITTER_ID = 0x5C, VIN_WAVE_BUFFERS = 0x60, VIN_CHANNEL_RESOURCES = 0x140, VIN_FLUSH_COUNT = 0x159 };
/* its 4 wave buffers, 0x38 bytes each */
enum { WB_SIZE = 0x38, WB_ADDRESS = 0x0, WB_BYTES = 0x8, WB_START = 0x10, WB_END = 0x14, WB_LOOP = 0x18,
       WB_END_OF_STREAM = 0x19, WB_SENT_TO_SERVER = 0x1A, WB_LOOP_COUNT = 0x1C, WB_CONTEXT = 0x20,
       WB_LOOP_START = 0x30, WB_LOOP_END = 0x34 };
/* VoiceChannelResourceInParameter (0x70): id, volume into each mix buffer, in use */
enum { RES_SIZE = 0x70, RES_MIX = 0x4, RES_IN_USE = 0x64 };
/* MixParameter (0x930); from REV7 only the changed mixes come, after a 0x20 header with their count */
enum { MIX_SIZE = 0x930, MIX_VOLUME = 0x0, MIX_BUFFER_COUNT = 0x8, MIX_IN_USE = 0xC, MIX_ID = 0x10,
       MIX_MATRIX = 0x24, MIX_DESTINATION = 0x924 };
/* SinkInParameter (0x140); a device sink: which final mix buffers it plays, its downmix */
enum { SINK_SIZE = 0x140, SINK_TYPE = 0x0, SINK_IN_USE = 0x1, SINK_INPUT_COUNT = 0x120, SINK_INPUTS = 0x124,
       SINK_DOWNMIX_ENABLED = 0x12B, SINK_DOWNMIX = 0x12C };

static void voice_finish_buffer(Voice *v) {          /* the current slot is done: move to the next */
    if (v->wb[v->index].end_of_stream) v->played = 0;
    v->valid[v->index] = 0;
    v->index = (v->index + 1) % 4;
    v->consumed++;
    v->offset = 0;
    v->loops = 0;
}

/* Take in one voice's parameters and newly appended wave buffers. */
static void voice_update(Voice *v, const uint8_t *in) {
    if (in[VIN_IS_NEW]) memset(v, 0, sizeof *v);
    v->format = in[VIN_SAMPLE_FORMAT];
    memcpy(&v->rate, in + VIN_SAMPLE_RATE, 4);
    memcpy(&v->channels, in + VIN_CHANNELS, 4);
    memcpy(&v->pitch, in + VIN_PITCH, 4);
    memcpy(&v->volume, in + VIN_VOLUME, 4);
    memcpy(&v->coefficients, in + VIN_DATA_SOURCE, 8);
    memcpy(&v->mix, in + VIN_MIX_ID, 4);
    memcpy(v->resources, in + VIN_CHANNEL_RESOURCES, sizeof v->resources);
    if (v->channels < 1 || v->channels > MAX_CHANNELS) v->channels = 1;
    if (!(v->pitch > 0 && v->pitch < 16)) v->pitch = 1;                 /* also catches NaN */
    if (!(v->volume >= 0 && v->volume < 16)) v->volume = 0;
    /* BiquadFilterParameter (0xC): enable, -, s16 numerator[3], s16 denominator[2], Q14 fixed point */
    for (int f = 0; f < 2; f++) {
        const uint8_t *q = in + VIN_BIQUADS + 0xC * f;
        int16_t num[3], den[2];
        memcpy(num, q + 2, 6);
        memcpy(den, q + 8, 4);
        if (q[0] && !v->biquad[f].on) memset(v->biquad_state[f], 0, sizeof v->biquad_state[f]);
        v->biquad[f].on = q[0];
        for (int k = 0; k < 3; k++) v->biquad[f].b[k] = num[k] / 16384.0f;
        for (int k = 0; k < 2; k++) v->biquad[f].a[k] = den[k] / 16384.0f;
    }
    for (int i = 0; i < 4; i++) {                     /* buffers the game appended since last time */
        const uint8_t *wb = in + VIN_WAVE_BUFFERS + WB_SIZE * i;
        if (wb[WB_SENT_TO_SERVER]) continue;
        WaveBuffer *b = &v->wb[i];
        memcpy(&b->address, wb + WB_ADDRESS, 8);
        memcpy(&b->size, wb + WB_BYTES, 8);
        memcpy(&b->start, wb + WB_START, 4);
        memcpy(&b->end, wb + WB_END, 4);
        memcpy(&b->loop_count, wb + WB_LOOP_COUNT, 4);
        memcpy(&b->context, wb + WB_CONTEXT, 8);
        memcpy(&b->loop_start, wb + WB_LOOP_START, 4);
        memcpy(&b->loop_end, wb + WB_LOOP_END, 4);
        b->loop = wb[WB_LOOP];
        b->end_of_stream = wb[WB_END_OF_STREAM];
        v->valid[i] = 1;
    }
    for (int n = in[VIN_FLUSH_COUNT]; n > 0 && v->valid[v->index]; n--) voice_finish_buffer(v);

    switch (in[VIN_PLAY_STATE]) {                     /* 0 Start, 1 Stop, 2 Pause */
    case 0: v->state = VOICE_STARTED; break;
    case 2: v->state = VOICE_PAUSED; break;
    case 1:
        if (v->state != VOICE_STOPPED) {              /* stopping drops every queued buffer */
            for (int i = 0; i < 4; i++)
                if (v->valid[i]) { v->valid[i] = 0; v->index = (v->index + 1) % 4; v->consumed++; }
            v->offset = 0;
            v->played = 0;
            v->loops = 0;
            v->fraction = 0;
            v->previous_volume = 0;
            memset(v->last, 0, sizeof v->last);
            v->state = VOICE_STOPPED;
        }
        break;
    }
}

/* Decode sample `s` of a wave buffer, every channel, as 16-bit values in floats. ADPCM samples
 * come in frames of 8 bytes: a header (predictor << 4 | scale), then 14 4-bit samples; each
 * sample is nibble << scale plus a prediction from the two before it (fixed point, 11 bits). */
static int decode(Voice *v, const WaveBuffer *b, uint32_t s, float *out) {
    switch (v->format) {
    case FORMAT_ADPCM: {
        uint64_t frame = s / 14, k = s % 14, at = frame * 8 + 1 + k / 2;
        const uint8_t *data = ipc_guest_ptr(b->address, b->size);
        const int16_t *coefficients = ipc_guest_ptr(v->coefficients, 32);
        if (!data || !coefficients || at >= b->size) return 0;
        if (k == 0) v->pred_scale = data[frame * 8];
        int predictor = v->pred_scale >> 4, scale = v->pred_scale & 15;
        int64_t c0 = predictor < 8 ? coefficients[2 * predictor] : 0, c1 = predictor < 8 ? coefficients[2 * predictor + 1] : 0;
        int nibble = k & 1 ? (int8_t)(data[at] << 4) >> 4 : (int8_t)data[at] >> 4;
        int64_t value = ((int64_t)nibble * (2048 << scale) + c0 * v->history[0] + c1 * v->history[1] + 1024) >> 11;
        int16_t sample = (int16_t)(value < -32768 ? -32768 : value > 32767 ? 32767 : value);
        v->history[1] = v->history[0];
        v->history[0] = sample;
        out[0] = sample;
        return 1;
    }
    case FORMAT_PCM16: {
        const int16_t *data = ipc_guest_ptr(b->address, b->size);
        if (!data || ((uint64_t)s + 1) * v->channels * 2 > b->size) return 0;
        for (uint32_t c = 0; c < v->channels; c++) out[c] = data[(uint64_t)s * v->channels + c];
        return 1;
    }
    case FORMAT_FLOAT: {
        const float *data = ipc_guest_ptr(b->address, b->size);
        if (!data || ((uint64_t)s + 1) * v->channels * 4 > b->size) return 0;
        for (uint32_t c = 0; c < v->channels; c++) out[c] = data[(uint64_t)s * v->channels + c] * 32767.0f;
        return 1;
    }
    default:
        return 0;
    }
}

/* The voice's next source sample (every channel) from its wave buffers; 0 when it has run out. */
static int voice_next(Voice *v, float *out) {
    for (int tries = 0; tries < 4; tries++) {
        if (!v->valid[v->index]) return 0;
        WaveBuffer *b = &v->wb[v->index];
        uint32_t start = b->start, end = b->end;
        if (v->loops > 0 && b->loop_start && b->loop_end && b->loop_start <= b->loop_end) {   /* (later loops) */
            start = b->loop_start;
            end = b->loop_end;
        }
        if (v->offset == 0 && v->format == FORMAT_ADPCM && b->context) {      /* the decoder's state here */
            const int16_t *context = ipc_guest_ptr(b->context, 6);
            if (context) {
                v->pred_scale = (uint8_t)context[0];
                v->history[0] = context[1];
                v->history[1] = context[2];
            }
        }
        int ok = end > start && decode(v, b, start + v->offset, out);
        if (ok) { v->offset++; v->played++; }
        if (!ok || v->offset >= end - start) {
            v->offset = 0;
            if (ok && b->loop) {
                v->loops++;
                if (b->loop_count >= 0 && v->loops > b->loop_count) voice_finish_buffer(v);
            } else {
                voice_finish_buffer(v);
            }
        }
        if (ok) return 1;
    }
    return 0;
}

static float *mix_buffer(Renderer *r, uint32_t mix, uint32_t buffer) {
    return r->buffers + ((size_t)mix * MIX_BUFFERS + buffer) * r->p.sample_count;
}

/* Play one frame of a voice into its mix: resampled (linear) from rate x pitch to the
 * renderer's rate, the volume ramping from last frame's. */
/* A started voice with nothing queued whose last samples and filters are all zero makes only
 * silence: skipped (the game keeps ~96 voices started, ~13 with something to play; resampling,
 * filtering and mixing the rest was most of the mixer's time) */
static int voice_silent(const Voice *v) {
    if (v->valid[v->index]) return 0;
    for (uint32_t c = 0; c < v->channels && c < MAX_CHANNELS; c++) {
        if (v->last[0][c] != 0 || v->last[1][c] != 0) return 0;
        for (int f = 0; f < 2; f++) if (v->biquad_state[f][c][0] != 0 || v->biquad_state[f][c][1] != 0) return 0;
    }
    return 1;
}

static void voice_render(Renderer *r, Voice *v) {
    if (v->state != VOICE_STARTED || !v->rate) return;
    if (voice_silent(v)) { v->previous_volume = v->volume; return; }
    uint32_t n = r->p.sample_count, channels = v->channels;
    static float in[MAX_CHANNELS][MAX_SAMPLES];
    double step = (double)v->rate * v->pitch / r->p.sample_rate;
    for (uint32_t i = 0; i < n; i++) {
        while (v->fraction >= 1) {
            v->fraction -= 1;
            memcpy(v->last[0], v->last[1], sizeof v->last[0]);
            if (!voice_next(v, v->last[1])) memset(v->last[1], 0, sizeof v->last[1]);
        }
        float t = (float)v->fraction;
        for (uint32_t c = 0; c < channels; c++) in[c][i] = v->last[0][c] + (v->last[1][c] - v->last[0][c]) * t;
        v->fraction += step;
    }
    for (int f = 0; f < 2; f++) {                     /* the filters (as Ryujinx's BiquadFilterHelper) */
        if (!v->biquad[f].on) continue;
        const float *b = v->biquad[f].b, *a = v->biquad[f].a;
        for (uint32_t c = 0; c < channels; c++) {
            float *z = v->biquad_state[f][c];
            for (uint32_t i = 0; i < n; i++) {
                float x = in[c][i], y = x * b[0] + z[0];
                z[0] = x * b[1] + y * a[0] + z[1];
                z[1] = x * b[2] + y * a[1];
                in[c][i] = y;
            }
        }
    }
    float from = v->previous_volume, to = v->volume;
    v->previous_volume = to;
    if (v->mix >= r->nmixes || !r->mixes[v->mix].used) return;
    const Mix *m = &r->mixes[v->mix];
    for (uint32_t c = 0; c < channels; c++) {
        if (v->resources[c] < 0 || (uint32_t)v->resources[c] >= r->p.voices) continue;
        const float *volumes = r->resources[v->resources[c]];
        for (uint32_t j = 0; j < m->buffers; j++) {
            if (volumes[j] == 0) continue;
            float *out = mix_buffer(r, v->mix, j), a = from * volumes[j], d = (to - from) * volumes[j] / (float)n;
            for (uint32_t i = 0; i < n; i++) out[i] += in[c][i] * (a + d * (float)i);
        }
    }
}

/* One audio frame of a renderer, added to `out` (stereo, 48 kHz). */
static void renderer_render(Renderer *r, float (*out)[2]) {
    uint32_t n = r->p.sample_count;
    memset(r->buffers, 0, (size_t)r->nmixes * MIX_BUFFERS * n * sizeof *r->buffers);
    for (uint32_t k = 0; k < r->p.voices; k++) voice_render(r, &r->voices[k]);
    /* sub-mixes into their destinations, the ones furthest from the final mix first */
    int depth[r->nmixes], deepest = 0;
    for (uint32_t k = 0; k < r->nmixes; k++) {
        uint32_t m = k;
        depth[k] = 0;
        while (m != 0 && m < r->nmixes && r->mixes[m].used && depth[k] <= (int)r->nmixes) { m = r->mixes[m].destination; depth[k]++; }
        if (m != 0) depth[k] = -1;                   /* (goes nowhere) */
        if (depth[k] > deepest) deepest = depth[k];
    }
    for (int d = deepest; d >= 1; d--) {
        for (uint32_t k = 0; k < r->nmixes; k++) {
            if (depth[k] != d) continue;
            const Mix *m = &r->mixes[k], *to = &r->mixes[m->destination];
            for (uint32_t i = 0; i < m->buffers; i++)
                for (uint32_t j = 0; j < to->buffers; j++) {
                    float volume = m->volume * m->matrix[i * MIX_BUFFERS + j];
                    if (volume == 0) continue;
                    const float *src = mix_buffer(r, k, i);
                    float *dst = mix_buffer(r, m->destination, j);
                    for (uint32_t s = 0; s < n; s++) dst[s] += src[s] * volume;
                }
        }
    }
    if (!r->mixes[0].used || r->p.sample_rate != 48000 || n != OUT_FRAMES) return;   /* (other rates: silent) */
    /* the device sink: final mix buffers -> speakers */
    float volume = r->mixes[0].volume;
    const uint8_t *input = r->sink_inputs;
    uint32_t count = r->sink_input_count ? r->sink_input_count : 2;
    static const uint8_t stereo[MAX_CHANNELS] = { 0, 1, 2, 3, 4, 5 };
    if (!r->sink_input_count) input = stereo;
    const float *buf[MAX_CHANNELS];
    for (uint32_t c = 0; c < MAX_CHANNELS; c++)
        buf[c] = mix_buffer(r, 0, input[c < count ? c : 0] < r->mixes[0].buffers ? input[c < count ? c : 0] : 0);
    if (count >= 6) {                                /* 5.1 -> stereo: front, centre, LFE, back */
        static const float standard[4] = { 1.0f, 0.707f, 0.251f, 0.707f };
        const float *k = r->downmix ? r->downmix_coefficients : standard;
        for (uint32_t s = 0; s < n; s++) {
            float shared = k[1] * buf[2][s] + k[2] * buf[3][s];
            out[s][0] += volume * (k[0] * buf[0][s] + shared + k[3] * buf[4][s]);
            out[s][1] += volume * (k[0] * buf[1][s] + shared + k[3] * buf[5][s]);
        }
    } else {
        for (uint32_t s = 0; s < n; s++) {
            out[s][0] += volume * buf[0][s];
            out[s][1] += volume * buf[count > 1][s];
        }
    }
}

/* Mix and sink parameters (the parts of the update only the mixing needs) */
static void update_mixes(Renderer *r, const uint8_t *in, uint32_t size) {
    uint32_t count = size / MIX_SIZE, dirty_only = 0;
    if (size >= 0x20 && (size - 0x20) % MIX_SIZE == 0) {
        uint32_t c;
        memcpy(&c, in + 4, 4);
        if (c == (size - 0x20) / MIX_SIZE) { dirty_only = 1; count = c; in += 0x20; }
    }
    for (uint32_t k = 0; k < count; k++) {
        const uint8_t *m = in + (uint64_t)MIX_SIZE * k;
        uint32_t id = k;
        if (dirty_only) memcpy(&id, m + MIX_ID, 4);
        if (id >= r->nmixes) continue;
        Mix *x = &r->mixes[id];
        x->used = m[MIX_IN_USE];
        memcpy(&x->volume, m + MIX_VOLUME, 4);
        memcpy(&x->buffers, m + MIX_BUFFER_COUNT, 4);
        memcpy(&x->destination, m + MIX_DESTINATION, 4);
        memcpy(x->matrix, m + MIX_MATRIX, sizeof x->matrix);
        if (x->buffers > MIX_BUFFERS) x->buffers = MIX_BUFFERS;
        if (!(x->volume >= 0 && x->volume < 16)) x->volume = 0;
    }
}

static void update_sinks(Renderer *r, const uint8_t *in, uint32_t size) {
    for (uint32_t k = 0; k < size / SINK_SIZE; k++) {
        const uint8_t *s = in + (uint64_t)SINK_SIZE * k;
        if (s[SINK_TYPE] != SINK_DEVICE || !s[SINK_IN_USE]) continue;
        memcpy(&r->sink_input_count, s + SINK_INPUT_COUNT, 4);
        if (r->sink_input_count > MAX_CHANNELS) r->sink_input_count = MAX_CHANNELS;
        memcpy(r->sink_inputs, s + SINK_INPUTS, MAX_CHANNELS);
        r->downmix = s[SINK_DOWNMIX_ENABLED];
        memcpy(r->downmix_coefficients, s + SINK_DOWNMIX, 16);
        return;
    }
}

static void request_update(Renderer *r, IpcCall *call) {
    IpcBuffer ib = ipc_in_buffer(call, 0), ob = ipc_out_buffer(call, 0);
    const uint8_t *in = ipc_guest_ptr(ib.addr, ib.size);
    uint8_t *out = ipc_guest_ptr(ob.addr, ob.size);
    if (!in || !out || ib.size < 0x40) { call->result = MAKE_RESULT(153, 41); return; }
    /* UpdateDataHeader: revision, then the size of each section: behaviour, pools, voices, voice
     * channel resources, effects, mixes, sinks, performance, splitters, renderer info... total.
     * The sections come in the order behaviour, pools, resources, voices, effects, splitters,
     * mixes, sinks, performance. */
    uint32_t hdr[16];
    memcpy(hdr, in, 0x40);
    uint32_t revision = hdr[0];
    int rev = REVISION(revision);
    uint64_t at = 0x40 + hdr[1];           /* skip header, behaviour */
    const uint8_t *pools_in = in + at;
    const uint8_t *resources_in = in + at + hdr[2];
    const uint8_t *voices_in = resources_in + hdr[4];
    const uint8_t *effects_in = voices_in + hdr[3];
    const uint8_t *mixes_in = effects_in + hdr[5] + hdr[9];
    const uint8_t *sinks_in = mixes_in + hdr[6];
    uint32_t effect_in_size = r->p.effects ? hdr[5] / r->p.effects : 0;

    uint32_t effect_out_size = rev >= 9 ? 0x90 : 0x10;
    uint64_t total = 0x40 + 0x10ull * r->pools + 0x10ull * r->p.voices + (uint64_t)effect_out_size * r->p.effects +
                     0x20ull * r->p.sinks + 0x10 + 0xB0 + (rev >= 5 ? 0x10 : 0);
    uint64_t in_total = 0x40ull + hdr[1] + hdr[2] + hdr[3] + hdr[4] + hdr[5] + hdr[6] + hdr[7] + hdr[9];
    if (total > ob.size || in_total > ib.size) {
        fprintf(stderr, "audren: update buffers too small (need 0x%llx/0x%llx, have 0x%llx/0x%llx)\n",
                (unsigned long long)in_total, (unsigned long long)total, (unsigned long long)ib.size,
                (unsigned long long)ob.size);
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
    for (uint32_t k = 0; k < hdr[4] / RES_SIZE; k++) {          /* voice channel resources */
        const uint8_t *res = resources_in + (uint64_t)RES_SIZE * k;
        uint32_t id;
        memcpy(&id, res, 4);
        if (id < r->p.voices && res[RES_IN_USE]) memcpy(r->resources[id], res + RES_MIX, sizeof r->resources[id]);
    }
    update_mixes(r, mixes_in, hdr[6]);
    update_sinks(r, sinks_in, hdr[7]);
    uint32_t nvoices = hdr[3] / VOICE_IN_SIZE < r->p.voices ? hdr[3] / VOICE_IN_SIZE : r->p.voices;
    static int trace = -1, traced;                             /* LAA_TRACE_AUDIO: a look at the voices */
    if (trace < 0) trace = getenv("LAA_TRACE_AUDIO") != NULL;
    int dump = trace && traced < 40 && r->elapsed_frames > 2000 && r->elapsed_frames % 400 == 0;
    if (dump) {
        traced++;
        fprintf(stderr, "audren: update: header %x %x %x %x %x %x %x %x %x %x %x, in %#llx bytes\n", hdr[1], hdr[2], hdr[3],
                hdr[4], hdr[5], hdr[6], hdr[7], hdr[8], hdr[9], hdr[10], hdr[15], (unsigned long long)ib.size);
        for (uint32_t k = 0; k < r->nmixes; k++)
            if (r->mixes[k].used)
                fprintf(stderr, "audren:   mix %u: %u buffers, volume %g, to mix %d\n", k, r->mixes[k].buffers,
                        r->mixes[k].volume, (int)r->mixes[k].destination);
        for (uint32_t k = 0; k < r->p.effects && effect_in_size >= 8; k++) {
            const uint8_t *e = effects_in + (uint64_t)effect_in_size * k;
            int32_t mix;
            memcpy(&mix, e + 4, 4);
            if (e[0]) fprintf(stderr, "audren:   effect %u: type %u enabled %u mix %d\n", k, e[0], e[2], mix);
            if (e[0] == 4) {                                       /* reverb: (fixed point) gains */
                int32_t g[16];
                memcpy(g, e + 0x20 + 0x10, sizeof g);
                fprintf(stderr, "audren:     reverb: in %u,%u out %u,%u channels %u rate %d early %d/%d predelay %d late %d/%d decay %d hf %d col %d reverb %d out %d dry %d\n",
                        e[0x20], e[0x21], e[0x26], e[0x27], e[0x2E], g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8],
                        g[9], g[10], g[11]);
            }
        }
        for (uint32_t k = 1; k < r->nmixes; k++) {
            if (!r->mixes[k].used) continue;
            char line[1024];
            int len = 0;
            for (uint32_t i = 0; i < r->mixes[k].buffers; i++)
                for (uint32_t j = 0; j < MIX_BUFFERS && len < 1000; j++)
                    if (r->mixes[k].matrix[i * MIX_BUFFERS + j] != 0)
                        len += snprintf(line + len, sizeof line - len, " %u->%u:%g", i, j, r->mixes[k].matrix[i * MIX_BUFFERS + j]);
            fprintf(stderr, "audren:   mix %u matrix:%s\n", k, len ? line : " (empty)");
        }
        fprintf(stderr, "audren:   sink: %u inputs %u,%u,%u,%u,%u,%u downmix %d\n", r->sink_input_count, r->sink_inputs[0],
                r->sink_inputs[1], r->sink_inputs[2], r->sink_inputs[3], r->sink_inputs[4], r->sink_inputs[5], r->downmix);
    }
    for (uint32_t k = 0; k < nvoices; k++) {                   /* voices: samples, buffers played */
        const uint8_t *vin = voices_in + (uint64_t)VOICE_IN_SIZE * k;
        if (!vin[VIN_IN_USE]) continue;
        Voice *v = &r->voices[k];
        voice_update(v, vin);
        if (dump && v->state == VOICE_STARTED) {
            const float *res = v->resources[0] >= 0 && (uint32_t)v->resources[0] < r->p.voices ? r->resources[v->resources[0]] : NULL;
            char sends[512];
            int len = 0;
            for (int j = 0; res && j < MIX_BUFFERS && len < 480; j++)   /* the mix buffers it feeds */
                if (res[j] != 0) len += snprintf(sends + len, sizeof sends - len, " %d:%.3g", j, res[j]);
            sends[len] = 0;
            fprintf(stderr, "audren:   voice %u: format %u ch %u rate %u pitch %g vol %g mix %d res %d ->%s, slot %d valid %d%d%d%d",
                    k, v->format, v->channels, v->rate, v->pitch, v->volume, (int)v->mix, v->resources[0],
                    sends, v->index, v->valid[0], v->valid[1], v->valid[2], v->valid[3]);
            for (int f = 0; f < 2; f++)
                if (v->biquad[f].on)
                    fprintf(stderr, " biquad%d b %.3f %.3f %.3f a %.3f %.3f", f, v->biquad[f].b[0], v->biquad[f].b[1],
                            v->biquad[f].b[2], v->biquad[f].a[0], v->biquad[f].a[1]);
            fprintf(stderr, "\n");
        }
        if (!vin[VIN_IS_NEW]) {
            memcpy(o + 0x10 * k, &v->played, 8);
            memcpy(o + 0x10 * k + 8, &v->consumed, 4);
        }
    }
    pthread_mutex_unlock(&renderers_lock);
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
        r->resources = calloc(r->p.voices + 1, sizeof *r->resources);
        r->nmixes = r->p.sub_mixes + 1;
        r->mixes = calloc(r->nmixes, sizeof *r->mixes);
        if (r->p.sample_count > MAX_SAMPLES) r->p.sample_count = MAX_SAMPLES;
        if (!r->p.sample_rate) r->p.sample_rate = 48000;
        r->buffers = calloc((size_t)r->nmixes * MIX_BUFFERS * (r->p.sample_count + 1), sizeof *r->buffers);
        if (!r->pool_state || !r->voices || !r->resources || !r->mixes || !r->buffers) exit(1);
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
        uint64_t size = 0, data = 0;
        if (p && b.size >= 0x28) { memcpy(&data, p + 0x8, 8); memcpy(&size, p + 0x18, 8); }
        if (o->queued == OUT_QUEUE) { call->result = MAKE_RESULT(153, 8); break; }
        o->queue[o->queued].tag = ipc_in64(call, 0);
        o->queue[o->queued].data = data;
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
        play_out(o, ~0ull >> 1, NULL);
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
