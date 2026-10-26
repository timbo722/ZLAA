/*
 * movie.c - video decoding for the game's movie player (the movie:: library in subsdk1).
 *
 * The game plays its WebM movies (VP9 video, Vorbis audio) with movie::Extractor, which reads
 * the file, and movie::Decoder, one per track. The library's audio decoder is plain software
 * and works as is. Its video decoder drives the Switch's hardware decoder and the real NVN
 * driver's internals, neither of which exists here, so we replace the video decoder:
 *   - movie::Decoder's constructor: for video types we take the object over; audio decoders
 *     are built by the original code as before.
 *   - every movie::Decoder method: ours for our objects, the original for the others.
 *     (The game calls them through the vtable, i.e. indirectly, so hle_replace catches them.)
 *
 * The protocol (like Android's MediaCodec): the game asks for a free input buffer, fills it
 * with one compressed frame and sends it with its timestamp; we produce an output frame; the
 * game asks for it, has it written into an NVN texture (GetOutputBuffer), shows it, and
 * releases it. nn::os events tell the game when input space or output frames are available.
 *
 * Frames are decoded with libvpx (desktop) or Android's MediaCodec (usually the hardware
 * decoder) and converted from YUV to RGBA here; without a decoder, frames keep their timing
 * but have no picture. MediaCodec gives frames back a few packets after it gets them, so its
 * finished frames are collected whenever the game sends input or gives back an output slot.
 */
#include "movie.h"
#include "hle.h"
#include "guest_memory.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "nvn/nvn.h"
#ifdef LAA_HAVE_VPX
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#endif
#ifdef __ANDROID__
#include <media/NdkMediaCodec.h>
#endif
#include <time.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#define NIN  4                       /* input buffers */
#define NOUT 6                       /* output frames */
#define IN_SIZE (2u << 20)           /* bytes per input buffer: a compressed 1080p frame fits */
#define FLAG_EOS 4u                  /* end of stream (buffer flag) */

enum { EVENT_INPUT, EVENT_OUTPUT, EVENT_FORMAT, EVENT_ERROR };

typedef struct {
    uint64_t self;                   /* the game's movie::Decoder object */
    uint32_t type;                   /* DecoderType: 1 AVC, 2 VP8, 3 VP9 (4 AAC, 5 Vorbis are not ours) */
    uint64_t events[4];              /* nn::os::EventType * for EVENT_* */
    int32_t  width, height;
    uint64_t in_mem[NIN];            /* guest memory the game copies compressed frames into */
    int      in_busy[NIN];
    int      out_busy[NOUT];
    struct { int slot; int64_t time; uint32_t flags; } ready[NOUT];
    int      nready;
    int      stopped;
    pthread_mutex_t lock;
    uint8_t *rgba[NOUT];             /* decoded picture of each output slot (width * height * 4) */
    int      has_picture[NOUT];
#ifdef LAA_HAVE_VPX
    vpx_codec_ctx_t codec;
    int      codec_open;
#endif
#ifdef __ANDROID__
    AMediaCodec *mc;
    pthread_mutex_t mc_lock;         /* one thread at a time in the codec */
    int32_t  mc_color, mc_stride, mc_slice;   /* output layout (from its output format) */
    int      mc_pending;             /* packets queued whose frames haven't come out yet */
    int      mc_eos;                 /* end of stream queued, its frame not out yet */
#endif
} VideoDecoder;

#define MAX_DECODERS 16
static VideoDecoder *decoders[MAX_DECODERS];
static pthread_mutex_t decoders_lock = PTHREAD_MUTEX_INITIALIZER;
static int trace;

/* LAA_MOVIE_STATS: every 120 frames, how long converting, decoding (with waits) and uploading took */
static int stats_on;
static struct { int frames; double convert, convert_max, send, send_max, upload, upload_max; } stats;
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void stat_add(double *sum, double *max, double ms) { *sum += ms; if (ms > *max) *max = ms; }

/* guest functions we call */
static struct {
    uint64_t signal_event, clear_event;
    uint64_t buffer_set_data, buffer_set_range, buffer_base, buffer_capacity;
    uint64_t md_clear, md_set_string, md_set_int32, md_find_int32;
} fn;

/* YUV 4:2:0 -> RGBA, limited range, BT.709 (HD video) or BT.601 coefficients, in 16.16 fixed point.
 * cstep: bytes from one chroma sample to the next, 1 for separate U and V planes (I420), 2 when
 * they are interleaved (NV12: u = the UV plane, v = u + 1). */
static void yuv_to_rgba(uint8_t *out, int w, int h, const uint8_t *y, int ys, const uint8_t *u, int us,
                        const uint8_t *v, int vs, int cstep, int bt709) {
    const int cy = 76309, crv = bt709 ? 117489 : 104597, cgu = bt709 ? 13975 : 25675, cgv = bt709 ? 34925 : 53279,
              cbu = bt709 ? 138438 : 132201;
    for (int j = 0; j < h; j++) {
        const uint8_t *yr = y + (size_t)j * ys, *ur = u + (size_t)(j / 2) * us, *vr = v + (size_t)(j / 2) * vs;
        uint8_t *o = out + (size_t)j * w * 4;
        int i = 0;
#ifdef __ARM_NEON
        /* 16 pixels at a time with ARM's vector instructions: the same formula, coefficients scaled
         * by 128 in 16-bit lanes. Saturating adds are harmless (past the 16-bit range is far past
         * 0..255, which the final narrowing clamps to); Y is capped at 235 (white either way) so
         * (Y - 16) * 149 fits. */
        const int16_t ky = 149, kr = bt709 ? 229 : 204, kgu = bt709 ? 27 : 50, kgv = bt709 ? 68 : 104,
                      kb = bt709 ? 270 : 258;
        for (; i + 16 <= w; i += 16) {
            uint8x16_t yv = vminq_u8(vld1q_u8(yr + i), vdupq_n_u8(235));
            uint8x8_t u8, v8;
            if (cstep == 2) { uint8x8x2_t uv = vld2_u8(ur + i); u8 = uv.val[0]; v8 = uv.val[1]; }
            else { u8 = vld1_u8(ur + i / 2); v8 = vld1_u8(vr + i / 2); }
            int16x8_t U = vreinterpretq_s16_u16(vsubl_u8(u8, vdup_n_u8(128)));
            int16x8_t V = vreinterpretq_s16_u16(vsubl_u8(v8, vdup_n_u8(128)));
            int16x8_t rc = vmulq_n_s16(V, kr), gc = vmlaq_n_s16(vmulq_n_s16(U, kgu), V, kgv);
            int16x8_t bc = vqaddq_s16(vshlq_n_s16(U, 8), vmulq_n_s16(U, kb - 256));   /* U * kb without overflow */
            int16x8x2_t r2 = vzipq_s16(rc, rc), g2 = vzipq_s16(gc, gc), b2 = vzipq_s16(bc, bc);   /* one per 2 pixels */
            int16x8_t y0 = vmulq_n_s16(vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(yv), vdup_n_u8(16))), ky);
            int16x8_t y1 = vmulq_n_s16(vreinterpretq_s16_u16(vsubl_u8(vget_high_u8(yv), vdup_n_u8(16))), ky);
            uint8x16x4_t px;
            px.val[0] = vcombine_u8(vqrshrun_n_s16(vqaddq_s16(y0, r2.val[0]), 7), vqrshrun_n_s16(vqaddq_s16(y1, r2.val[1]), 7));
            px.val[1] = vcombine_u8(vqrshrun_n_s16(vqsubq_s16(y0, g2.val[0]), 7), vqrshrun_n_s16(vqsubq_s16(y1, g2.val[1]), 7));
            px.val[2] = vcombine_u8(vqrshrun_n_s16(vqaddq_s16(y0, b2.val[0]), 7), vqrshrun_n_s16(vqaddq_s16(y1, b2.val[1]), 7));
            px.val[3] = vdupq_n_u8(255);
            vst4q_u8(o + 4 * i, px);
        }
#endif
        for (; i < w; i++) {
            int Y = (yr[i] - 16) * cy, U = ur[i / 2 * cstep] - 128, V = vr[i / 2 * cstep] - 128;
            int r = (Y + crv * V) >> 16, g = (Y - cgu * U - cgv * V) >> 16, b = (Y + cbu * U) >> 16;
            o[4 * i] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            o[4 * i + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            o[4 * i + 2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
            o[4 * i + 3] = 255;
        }
    }
}

/* Strings handed to guest functions must be in guest memory: copied there once. */
static uint64_t strings, strings_used;
static uint64_t guest_string(const char *s) {
    uint64_t a = strings + strings_used;
    memcpy(guest_mem + a, s, strlen(s) + 1);
    strings_used += strlen(s) + 1;
    return a;
}
static struct { uint64_t mime, video_raw, width, height, y_stride, y_offset, uv_offset, colorspace; } key;

static VideoDecoder *ours(uint64_t self) {
    pthread_mutex_lock(&decoders_lock);
    VideoDecoder *d = NULL;
    for (int i = 0; i < MAX_DECODERS && !d; i++)
        if (decoders[i] && decoders[i]->self == self) d = decoders[i];
    pthread_mutex_unlock(&decoders_lock);
    return d;
}

static void signal_event(CPU *c, VideoDecoder *d, int which) {
    pthread_mutex_lock(&d->lock);
    int stopped = d->stopped;
    pthread_mutex_unlock(&d->lock);
    if (!stopped && d->events[which]) hle_call(c, fn.signal_event, d->events[which], 0, 0, 0);
}

/* ---- the methods, in vtable order ---- */

#ifdef __ANDROID__
static void mc_open(VideoDecoder *d);                /* (MediaCodec, below) */
#endif

/* Configure(MediaData *format): the track's format from the extractor */
static void configure(CPU *c, VideoDecoder *d) {
    uint64_t out = d->in_mem[0];                     /* scratch for FindInt32's result */
    if (hle_call(c, fn.md_find_int32, c->x[1], key.width, out, 0) & 1) d->width = (int32_t)g_ld32(out);
    if (hle_call(c, fn.md_find_int32, c->x[1], key.height, out, 0) & 1) d->height = (int32_t)g_ld32(out);
    fprintf(stderr, "movie: video decoder (type %u) %dx%d\n", d->type, d->width, d->height);
#ifdef LAA_HAVE_VPX
    vpx_codec_dec_cfg_t cfg = { .threads = 4, .w = (unsigned)d->width, .h = (unsigned)d->height };
    vpx_codec_iface_t *iface = d->type == 3 ? vpx_codec_vp9_dx() : d->type == 2 ? vpx_codec_vp8_dx() : NULL;
    if (iface && !d->codec_open) d->codec_open = vpx_codec_dec_init(&d->codec, iface, &cfg, 0) == VPX_CODEC_OK;
    if (!d->codec_open) fprintf(stderr, "movie: no decoder for this video type\n");
#endif
#ifdef __ANDROID__
    if (!d->mc) mc_open(d);
#endif
    c->x[0] = 0;
}

/* RegisterClientEvents(DecoderEvents *): four nn::os::EventType pointers */
static void register_events(CPU *c, VideoDecoder *d) {
    memcpy(d->events, guest_mem + c->x[1], sizeof d->events);
    c->x[0] = 0;
}

/* GetOutputFormat(MediaData *) / GetOutputBufferFormat(int, MediaData *): raw NV12 frames */
static void output_format(CPU *c, VideoDecoder *d, uint64_t md) {
    int32_t stride = (d->width + 255) & ~255, aligned_height = (d->height + 15) & ~15;
    hle_call(c, fn.md_clear, md, 0, 0, 0);
    hle_call(c, fn.md_set_string, md, key.mime, key.video_raw, 0);
    hle_call(c, fn.md_set_int32, md, key.width, (uint32_t)d->width, 0);
    hle_call(c, fn.md_set_int32, md, key.height, (uint32_t)d->height, 0);
    hle_call(c, fn.md_set_int32, md, key.y_stride, (uint32_t)stride, 0);
    hle_call(c, fn.md_set_int32, md, key.y_offset, 0, 0);
    hle_call(c, fn.md_set_int32, md, key.uv_offset, (uint32_t)(stride * aligned_height), 0);
    hle_call(c, fn.md_set_int32, md, key.colorspace, 0, 0);
    c->x[0] = 0;
}

static void start(CPU *c, VideoDecoder *d) {
    pthread_mutex_lock(&d->lock);
    d->stopped = 0;
    pthread_mutex_unlock(&d->lock);
    signal_event(c, d, EVENT_INPUT);                 /* every input buffer is free */
    c->x[0] = 0;
}

/* Stop: no more events after this, and clear the ones still set, or a game thread may wake
 * up and call a decoder that is being deleted. */
static void stop(CPU *c, VideoDecoder *d) {
    pthread_mutex_lock(&d->lock);
    int was_stopped = d->stopped;
    d->stopped = 1;
    pthread_mutex_unlock(&d->lock);
    if (!was_stopped)
        for (int i = 0; i < 4; i++)
            if (d->events[i]) hle_call(c, fn.clear_event, d->events[i], 0, 0, 0);
    c->x[0] = 0;
}

static void flush(CPU *c, VideoDecoder *d) {
#ifdef __ANDROID__
    pthread_mutex_lock(&d->mc_lock);
    if (d->mc) AMediaCodec_flush(d->mc);
    d->mc_pending = d->mc_eos = 0;
    pthread_mutex_unlock(&d->mc_lock);
#endif
    pthread_mutex_lock(&d->lock);
    memset(d->in_busy, 0, sizeof d->in_busy);
    memset(d->out_busy, 0, sizeof d->out_busy);
    d->nready = 0;
    pthread_mutex_unlock(&d->lock);
    c->x[0] = 0;
}

/* AcquireInputBufferIndex(int *index, int *free_after) -> 0, or 1 if none is free. Only
 * hands out input while there is room for the output it will produce. */
static void acquire_input(CPU *c, VideoDecoder *d) {
    pthread_mutex_lock(&d->lock);
    int out_free = 0, idx = -1, more = 0;
    for (int i = 0; i < NOUT; i++) out_free += !d->out_busy[i];
    for (int i = 0; i < NIN && out_free >= 2; i++)
        if (!d->in_busy[i]) { if (idx < 0) idx = i; else more++; }
    if (idx >= 0) d->in_busy[idx] = 1;
    pthread_mutex_unlock(&d->lock);
    if (idx < 0) { c->x[0] = 1; return; }
    g_st32(c->x[1], (uint32_t)idx);
    if (c->x[2]) g_st32(c->x[2], (uint32_t)more);
    c->x[0] = 0;
}

/* GetInputBuffer(int index, Buffer *) */
static void get_input_buffer(CPU *c, VideoDecoder *d) {
    int i = (int)c->x[1];
    if (i < 0 || i >= NIN) { c->x[0] = 1; return; }
    hle_call(c, fn.buffer_set_data, c->x[2], d->in_mem[i], IN_SIZE, 0);
    hle_call(c, fn.buffer_set_range, c->x[2], 0, 0, 0);
    c->x[0] = 0;
}

/* Queue an output frame (with a picture from `rgba`, or none); caller holds the lock */
static int take_slot(VideoDecoder *d) {             /* a free output slot, now busy; -1 if none (lock held) */
    int slot = -1;
    for (int i = 0; i < NOUT && slot < 0; i++) if (!d->out_busy[i]) slot = i;
    if (slot < 0) return -1;
    d->out_busy[slot] = 1;
    d->has_picture[slot] = 0;
    if (!d->rgba[slot]) d->rgba[slot] = malloc((size_t)d->width * d->height * 4);
    return slot;
}
static void queue_slot(VideoDecoder *d, int slot, int64_t time, uint32_t flags) {   /* for the game (lock held) */
    d->ready[d->nready].slot = slot;
    d->ready[d->nready].time = time;
    d->ready[d->nready].flags = flags;
    d->nready++;
}
static int emit(VideoDecoder *d, int64_t time, uint32_t flags, const uint8_t *rgba) {
    int slot = take_slot(d);
    if (slot < 0) return 0;
    if (rgba && d->rgba[slot]) {
        memcpy(d->rgba[slot], rgba, (size_t)d->width * d->height * 4);
        d->has_picture[slot] = 1;
    }
    queue_slot(d, slot, time, flags);
    return 1;
}

#ifdef __ANDROID__
/* ---- Android: MediaCodec ---- */
#define COLOR_I420 19                /* COLOR_FormatYUV420Planar; other formats: NV12-like (Y, then UV pairs) */

static void mc_open(VideoDecoder *d) {
    const char *mime = d->type == 3 ? "video/x-vnd.on2.vp9" : d->type == 2 ? "video/x-vnd.on2.vp8" : "video/avc";
    d->mc = AMediaCodec_createDecoderByType(mime);
    if (!d->mc) { fprintf(stderr, "movie: no MediaCodec decoder for %s\n", mime); return; }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, d->width);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, d->height);
    media_status_t s = AMediaCodec_configure(d->mc, f, NULL, NULL, 0);
    AMediaFormat_delete(f);
    if (s == AMEDIA_OK) s = AMediaCodec_start(d->mc);
    if (s != AMEDIA_OK) {
        fprintf(stderr, "movie: MediaCodec %s: error %d\n", mime, (int)s);
        AMediaCodec_delete(d->mc);
        d->mc = NULL;
        return;
    }
    char *name = NULL;
    if (AMediaCodec_getName(d->mc, &name) == AMEDIA_OK && name) {
        fprintf(stderr, "movie: decoding with %s\n", name);
        AMediaCodec_releaseName(d->mc, name);
    }
}

/* The decoder's output layout: pixel format, bytes per row, rows before the chroma plane */
static void mc_read_format(VideoDecoder *d) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(d->mc);
    if (!f) return;
    if (!AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &d->mc_color)) d->mc_color = 0;
    if (!AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &d->mc_stride) || d->mc_stride < d->width) d->mc_stride = d->width;
    if (!AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &d->mc_slice) || d->mc_slice < d->height) d->mc_slice = d->height;
    AMediaFormat_delete(f);
    fprintf(stderr, "movie: MediaCodec output: color format %#x, stride %d, slice height %d\n", d->mc_color,
            d->mc_stride, d->mc_slice);
}

/* YUV at p (cap bytes) -> RGBA at out; 0 if the frame doesn't fit the layout we know */
static int mc_convert(VideoDecoder *d, const uint8_t *p, size_t cap, uint8_t *out) {
    if (!d->mc_stride) mc_read_format(d);
    size_t ys = (size_t)d->mc_stride, luma = ys * (size_t)d->mc_slice;
    int planar = d->mc_color == COLOR_I420;
    size_t cs = planar ? ys / 2 : ys, chroma = cs * (size_t)((d->height + 1) / 2);
    if (!out || luma + (planar ? 2 * chroma : chroma) > cap) return 0;
    const uint8_t *u = p + luma, *v = planar ? u + cs * (size_t)(d->mc_slice / 2) : u + 1;
    double t0 = now_ms();
    yuv_to_rgba(out, d->width, d->height, p, (int)ys, u, (int)cs, v, (int)cs, planar ? 1 : 2, d->height >= 720);
    stat_add(&stats.convert, &stats.convert_max, now_ms() - t0);
    return 1;
}

/* Hand the game the frames MediaCodec has finished (waiting up to wait_us for the first one).
 * Takes frames only while the game has 2 free output slots (a frame and an end-of-stream mark).
 * Returns whether it queued any. */
static int mc_drain(VideoDecoder *d, int64_t wait_us) {
    int produced = 0;
    pthread_mutex_lock(&d->mc_lock);
    while (d->mc) {
        pthread_mutex_lock(&d->lock);
        int out_free = 0;
        for (int i = 0; i < NOUT; i++) out_free += !d->out_busy[i];
        pthread_mutex_unlock(&d->lock);
        if (out_free < 2) break;
        AMediaCodecBufferInfo info;
        ssize_t i = AMediaCodec_dequeueOutputBuffer(d->mc, &info, produced ? 0 : wait_us);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { mc_read_format(d); continue; }
        if (i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (i < 0) break;                            /* nothing finished yet */
        size_t cap = 0;
        uint8_t *p = AMediaCodec_getOutputBuffer(d->mc, (size_t)i, &cap);
        int eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        pthread_mutex_lock(&d->lock);
        int slot = info.size > 0 ? take_slot(d) : -1;
        pthread_mutex_unlock(&d->lock);
        /* straight into the slot the game will get (it isn't queued yet, so nobody else reads it) */
        if (slot >= 0 && p && info.offset >= 0 && (size_t)info.offset < cap)
            d->has_picture[slot] = mc_convert(d, p + info.offset, cap - (size_t)info.offset, d->rgba[slot]);
        AMediaCodec_releaseOutputBuffer(d->mc, (size_t)i, false);
        pthread_mutex_lock(&d->lock);
        if (slot >= 0) { queue_slot(d, slot, info.presentationTimeUs, 0); produced = 1; }
        if (eos) produced |= emit(d, info.presentationTimeUs, FLAG_EOS, NULL);
        pthread_mutex_unlock(&d->lock);
        if (info.size > 0 && d->mc_pending > 0) d->mc_pending--;
        if (eos) { d->mc_eos = 0; d->mc_pending = 0; break; }
    }
    pthread_mutex_unlock(&d->mc_lock);
    return produced;
}

/* One compressed packet (or just the end of the stream) into the decoder */
static void mc_queue(VideoDecoder *d, const uint8_t *data, int size, int64_t time, int eos) {
    pthread_mutex_lock(&d->mc_lock);
    ssize_t i = d->mc ? AMediaCodec_dequeueInputBuffer(d->mc, 100000) : -1;   /* waits 0.1 s at most */
    if (i >= 0) {
        size_t cap = 0;
        uint8_t *p = AMediaCodec_getInputBuffer(d->mc, (size_t)i, &cap);
        if ((size_t)size > cap) { fprintf(stderr, "movie: packet of %d bytes cut to %zu\n", size, cap); size = (int)cap; }
        if (p && size > 0) memcpy(p, data, (size_t)size);
        AMediaCodec_queueInputBuffer(d->mc, (size_t)i, 0, (size_t)size, (uint64_t)time,
                                     eos ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
        if (size > 0) d->mc_pending++;
        if (eos) d->mc_eos = 1;
    } else if (d->mc) {
        fprintf(stderr, "movie: MediaCodec has no free input buffer: packet dropped\n");
    }
    pthread_mutex_unlock(&d->mc_lock);
}

/* Send a packet; returns whether frames came out for the game */
static int mc_send(VideoDecoder *d, const uint8_t *data, int size, int64_t time, int eos) {
    int produced = mc_drain(d, 0);                   /* (frees the codec's output space first) */
    mc_queue(d, data, size, time, eos);
    /* A hardware decoder holds a few packets before its first frame: once several wait, give
     * it a moment. At the end of the stream, collect everything (1 s at most); if the decoder
     * never says "end", we say it, or the game would wait for the movie forever. */
    produced |= mc_drain(d, d->mc_pending >= 3 ? 8000 : 0);
    if (eos) {
        struct timespec t0, t;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        do {
            produced |= mc_drain(d, 20000);
            clock_gettime(CLOCK_MONOTONIC, &t);
        } while (d->mc_eos && (t.tv_sec - t0.tv_sec) * 1000000000LL + (t.tv_nsec - t0.tv_nsec) < 1000000000LL);
        if (d->mc_eos) {
            pthread_mutex_lock(&d->lock);
            produced |= emit(d, time, FLAG_EOS, NULL);
            pthread_mutex_unlock(&d->lock);
            d->mc_eos = 0;
        }
    }
    return produced;
}
#endif

/* SendInputBufferForDecode(int index, int offset, int size, int64 time, uint32 flags) */
static void send_input(CPU *c, VideoDecoder *d) {
    int idx = (int)c->x[1], off = (int)c->x[2], size = (int)c->x[3];
    int64_t time = (int64_t)c->x[4];
    uint32_t flags = (uint32_t)c->x[5];
    int produced = 0;
#ifdef LAA_HAVE_VPX
    static uint8_t *picture;                         /* (one converted frame at a time) */
    if (size > 0 && idx >= 0 && idx < NIN && d->codec_open) {
        if (vpx_codec_decode(&d->codec, guest_mem + d->in_mem[idx] + off, (unsigned)size, NULL, 0) != VPX_CODEC_OK)
            fprintf(stderr, "movie: VP9 decode error: %s\n", vpx_codec_error(&d->codec));
        vpx_codec_iter_t it = NULL;
        vpx_image_t *img;
        while ((img = vpx_codec_get_frame(&d->codec, &it))) {
            if ((int)img->d_w != d->width || (int)img->d_h != d->height || img->fmt != VPX_IMG_FMT_I420) continue;
            if (!picture) picture = malloc((size_t)d->width * d->height * 4);
            yuv_to_rgba(picture, d->width, d->height, img->planes[0], img->stride[0], img->planes[1], img->stride[1],
                        img->planes[2], img->stride[2], 1, img->cs != VPX_CS_BT_601);
            pthread_mutex_lock(&d->lock);
            produced |= emit(d, time, 0, picture);
            pthread_mutex_unlock(&d->lock);
        }
        size = 0;                                    /* (frames emitted above) */
    }
#endif
#ifdef __ANDROID__
    if (d->mc && idx >= 0 && idx < NIN && (size > 0 || (flags & FLAG_EOS))) {
        double t0 = now_ms();
        produced |= mc_send(d, guest_mem + d->in_mem[idx] + off, size > 0 ? size : 0, time, (flags & FLAG_EOS) != 0);
        stat_add(&stats.send, &stats.send_max, now_ms() - t0);
        size = 0;                                    /* (frames and the end were emitted by mc_send) */
        flags &= ~FLAG_EOS;
    }
#endif
    pthread_mutex_lock(&d->lock);
    if (size > 0) produced |= emit(d, time, 0, NULL);  /* no decoder: a frame without a picture */
    if (flags & FLAG_EOS) produced |= emit(d, time, FLAG_EOS, NULL);
    if (idx >= 0 && idx < NIN) d->in_busy[idx] = 0;
    pthread_mutex_unlock(&d->lock);
    if (trace) fprintf(stderr, "movie: video packet %d bytes at %" PRId64 " us%s\n", size, time, flags & FLAG_EOS ? " (end)" : "");
    signal_event(c, d, EVENT_INPUT);
    if (produced) signal_event(c, d, EVENT_OUTPUT);
    c->x[0] = 0;
}

/* AcquireOutputBufferIndex(int *index, int64 *time, uint32 *flags, int *remaining) -> 0, or 1 if none */
static void acquire_output(CPU *c, VideoDecoder *d) {
    pthread_mutex_lock(&d->lock);
    if (!d->nready) { pthread_mutex_unlock(&d->lock); c->x[0] = 1; return; }
    int slot = d->ready[0].slot;
    int64_t time = d->ready[0].time;
    uint32_t flags = d->ready[0].flags;
    memmove(d->ready, d->ready + 1, (size_t)--d->nready * sizeof d->ready[0]);
    int remaining = d->nready;
    pthread_mutex_unlock(&d->lock);
    g_st32(c->x[1], (uint32_t)slot);
    if (c->x[2]) g_st64(c->x[2], (uint64_t)time);
    if (c->x[3]) g_st32(c->x[3], flags);
    if (c->x[4]) g_st32(c->x[4], (uint32_t)remaining);
    c->x[0] = 0;
}

/* GetOutputBuffer(int index, Buffer *): in the game's mode the Buffer's data is the NVNtexture
 * (capacity 8) to put the frame into. */
static void get_output_buffer(CPU *c, VideoDecoder *d) {
    int i = (int)c->x[1];
    uint64_t base = hle_call(c, fn.buffer_base, c->x[2], 0, 0, 0);
    uint64_t capacity = hle_call(c, fn.buffer_capacity, c->x[2], 0, 0, 0);
    if (trace) fprintf(stderr, "movie: output %d -> buffer %#" PRIx64 " (storage %#" PRIx64 ") capacity %" PRIu64 " picture %d\n", i, base,
                       g_ld64(base + 0x48 + 0x10), capacity, i >= 0 && i < NOUT ? d->has_picture[i] : -1);
    if (i >= 0 && i < NOUT && capacity == 8 && d->has_picture[i]) {
        double t0 = now_ms();
        nvn_write_texture(base, d->rgba[i], (uint32_t)d->width, (uint32_t)d->height);
        stat_add(&stats.upload, &stats.upload_max, now_ms() - t0);
        if (stats_on && ++stats.frames == 120) {
            fprintf(stderr, "movie: 120 frames: convert %.1f ms (max %.1f), send %.1f ms (max %.1f), upload %.1f ms (max %.1f)\n",
                    stats.convert / 120, stats.convert_max, stats.send / 120, stats.send_max, stats.upload / 120, stats.upload_max);
            memset(&stats, 0, sizeof stats);
        }
    }
    c->x[0] = 0;
}

/* ReleaseOutputBufferIndex(int index) */
static void release_output(CPU *c, VideoDecoder *d) {
    int i = (int)c->x[1];
    pthread_mutex_lock(&d->lock);
    if (i >= 0 && i < NOUT) d->out_busy[i] = 0;
    pthread_mutex_unlock(&d->lock);
#ifdef __ANDROID__
    if (mc_drain(d, 0)) signal_event(c, d, EVENT_OUTPUT);   /* frames that waited for a free slot */
#endif
    signal_event(c, d, EVENT_INPUT);                 /* there may be room for input again */
    c->x[0] = 0;
}

/* GetOutputBufferSize(int *, int *) */
static void output_buffer_size(CPU *c, VideoDecoder *d) {
    (void)d;
    if (c->x[1]) g_st32(c->x[1], 0);
    if (c->x[2]) g_st32(c->x[2], 0);
    c->x[0] = 0;
}

static void destroy(CPU *c, VideoDecoder *d) {
    stop(c, d);
    pthread_mutex_lock(&decoders_lock);
    for (int i = 0; i < MAX_DECODERS; i++) if (decoders[i] == d) decoders[i] = NULL;
    pthread_mutex_unlock(&decoders_lock);
    /* the input buffers' guest memory is kept for the next decoder (see decoder_ctor) */
#ifdef LAA_HAVE_VPX
    if (d->codec_open) vpx_codec_destroy(&d->codec);
    d->codec_open = 0;
#endif
#ifdef __ANDROID__
    pthread_mutex_lock(&d->mc_lock);
    if (d->mc) { AMediaCodec_stop(d->mc); AMediaCodec_delete(d->mc); }
    d->mc = NULL;
    pthread_mutex_unlock(&d->mc_lock);
#endif
    for (int i = 0; i < NOUT; i++) { free(d->rgba[i]); d->rgba[i] = NULL; }
    c->x[0] = 0;
}

static void ok(CPU *c, VideoDecoder *d) { (void)d; c->x[0] = 0; }
static void get_output_format(CPU *c, VideoDecoder *d) { output_format(c, d, c->x[1]); }
static void get_output_buffer_format(CPU *c, VideoDecoder *d) { output_format(c, d, c->x[2]); }

/* ---- dispatch: each movie::Decoder method goes to ours or to the original ---- */

typedef void (*Method)(CPU *c, VideoDecoder *d);
static const struct { const char *symbol; Method method; } methods[] = {
    { "_ZN5movie7DecoderD1Ev", destroy },
    { "_ZN5movie7DecoderD0Ev", destroy },
    { "_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE", configure },
    { "_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", register_events },
    { "_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE", get_output_format },
    { "_ZN5movie7Decoder5StartEv", start },
    { "_ZN5movie7Decoder4StopEv", stop },
    { "_ZN5movie7Decoder5ResetEv", flush },
    { "_ZN5movie7Decoder5FlushEv", flush },
    { "_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", acquire_input },
    { "_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE", get_input_buffer },
    { "_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", send_input },
    { "_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_", acquire_output },
    { "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", get_output_buffer },
    { "_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_", output_buffer_size },
    { "_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", release_output },
    { "_ZN5movie7Decoder12GetLastErrorEv", ok },
    { "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", get_output_buffer },
    { "_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE", get_output_buffer_format },
};
#define NMETHODS (int)(sizeof methods / sizeof *methods)
static uint64_t original[NMETHODS + 1];               /* [NMETHODS] = the constructor */

static void dispatch(CPU *c, int k) {
    VideoDecoder *d = ours(c->x[0]);
    if (trace && d) fprintf(stderr, "movie: %s(%#" PRIx64 ", %#" PRIx64 ")\n", methods[k].symbol, c->x[1], c->x[2]);
    if (d) methods[k].method(c, d);
    else recomp_lookup(original[k])(c);
}
#define D(k) static void dispatch_##k(CPU *c) { dispatch(c, k); }
D(0) D(1) D(2) D(3) D(4) D(5) D(6) D(7) D(8) D(9) D(10) D(11) D(12) D(13) D(14) D(15) D(16) D(17) D(18)
static const HleFunc dispatchers[] = { dispatch_0, dispatch_1, dispatch_2, dispatch_3, dispatch_4, dispatch_5,
    dispatch_6, dispatch_7, dispatch_8, dispatch_9, dispatch_10, dispatch_11, dispatch_12, dispatch_13,
    dispatch_14, dispatch_15, dispatch_16, dispatch_17, dispatch_18 };
_Static_assert(sizeof dispatchers / sizeof *dispatchers == NMETHODS, "one dispatcher per method");

/* Input buffers are reused by later decoders: the title screen replays its movie forever. */
static uint64_t spare_in_mem[NIN];
static uint64_t vtable;                               /* movie::Decoder's vtable */

static void decoder_ctor(CPU *c) {
    uint32_t type = (uint32_t)c->x[1];
    if (type < 1 || type > 3) {                      /* audio: the library's own decoder */
        recomp_lookup(original[NMETHODS])(c);
        return;
    }
    VideoDecoder *d = calloc(1, sizeof *d);
    d->self = c->x[0];
    d->type = type;
    pthread_mutex_init(&d->lock, NULL);
#ifdef __ANDROID__
    pthread_mutex_init(&d->mc_lock, NULL);
#endif
    for (int i = 0; i < NIN; i++) {
        d->in_mem[i] = spare_in_mem[i] ? spare_in_mem[i] : hle_alloc(IN_SIZE);
        spare_in_mem[i] = d->in_mem[i];
    }
    /* The object gets the real vtable: the game calls through it, and we catch those calls */
    pthread_mutex_lock(&decoders_lock);
    int placed = 0;
    for (int i = 0; i < MAX_DECODERS && !placed; i++) if (!decoders[i]) { decoders[i] = d; placed = 1; }
    pthread_mutex_unlock(&decoders_lock);
    if (!placed) { fprintf(stderr, "movie: too many video decoders\n"); exit(1); }
    g_st64(d->self, vtable + 16);                    /* (a vtable symbol points 16 bytes before slot 0) */
}

static uint64_t symbol(const GameImage *img, const char *name) {
    uint64_t a = nso_find_symbol(img, name);
    if (!a) { fprintf(stderr, "movie: symbol %s not found\n", name); exit(1); }
    return a;
}

void movie_init(const GameImage *img) {
    trace = getenv("LAA_TRACE_MOVIE") != NULL;
    stats_on = getenv("LAA_MOVIE_STATS") != NULL;
    fn.signal_event = symbol(img, "_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    fn.clear_event = symbol(img, "_ZN2nn2os10ClearEventEPNS0_9EventTypeE");
    fn.buffer_set_data = symbol(img, "_ZN5movie6Buffer18SetDataAndCapacityEPvm");
    fn.buffer_set_range = symbol(img, "_ZN5movie6Buffer8SetRangeEii");
    fn.buffer_base = symbol(img, "_ZNK5movie6Buffer4BaseEv");
    fn.buffer_capacity = symbol(img, "_ZNK5movie6Buffer8CapacityEv");
    fn.md_clear = symbol(img, "_ZN5movie9MediaData5ClearEv");
    fn.md_set_string = symbol(img, "_ZN5movie9MediaData9SetStringEPKcS2_");
    fn.md_set_int32 = symbol(img, "_ZN5movie9MediaData8SetInt32EPKci");
    fn.md_find_int32 = symbol(img, "_ZNK5movie9MediaData9FindInt32EPKcPi");

    strings = hle_alloc(GUEST_PAGE);
    key.mime = guest_string("mime");
    key.video_raw = guest_string("video/raw");
    key.width = guest_string("width");
    key.height = guest_string("height");
    key.y_stride = guest_string("nv12-y-stride");
    key.y_offset = guest_string("nv12-y-offset");
    key.uv_offset = guest_string("nv12-uv-offset");
    key.colorspace = guest_string("nv12-colorspace");

    for (int k = 0; k < NMETHODS; k++) {
        original[k] = symbol(img, methods[k].symbol);
        hle_replace(original[k], dispatchers[k]);
    }
    original[NMETHODS] = symbol(img, "_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi");
    hle_replace(original[NMETHODS], decoder_ctor);
    vtable = symbol(img, "_ZTVN5movie7DecoderE");
}
