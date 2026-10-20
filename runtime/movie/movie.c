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
 * Decoding isn't done yet: every frame comes out with the right timestamp but no picture, so
 * movies play for their real length (with sound) over an unchanged texture.
 */
#include "movie.h"
#include "hle.h"
#include "guest_memory.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

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
} VideoDecoder;

#define MAX_DECODERS 16
static VideoDecoder *decoders[MAX_DECODERS];
static pthread_mutex_t decoders_lock = PTHREAD_MUTEX_INITIALIZER;
static int trace;

/* guest functions we call */
static struct {
    uint64_t signal_event, clear_event;
    uint64_t buffer_set_data, buffer_set_range, buffer_base, buffer_capacity;
    uint64_t md_clear, md_set_string, md_set_int32, md_find_int32;
} fn;

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

/* Configure(MediaData *format): the track's format from the extractor */
static void configure(CPU *c, VideoDecoder *d) {
    uint64_t out = d->in_mem[0];                     /* scratch for FindInt32's result */
    if (hle_call(c, fn.md_find_int32, c->x[1], key.width, out, 0) & 1) d->width = (int32_t)g_ld32(out);
    if (hle_call(c, fn.md_find_int32, c->x[1], key.height, out, 0) & 1) d->height = (int32_t)g_ld32(out);
    fprintf(stderr, "movie: video decoder (type %u) %dx%d\n", d->type, d->width, d->height);
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

/* SendInputBufferForDecode(int index, int offset, int size, int64 time, uint32 flags) */
static void send_input(CPU *c, VideoDecoder *d) {
    int idx = (int)c->x[1], size = (int)c->x[3];
    int64_t time = (int64_t)c->x[4];
    uint32_t flags = (uint32_t)c->x[5];
    int produced = 0;
    pthread_mutex_lock(&d->lock);
    if (size > 0 || (flags & FLAG_EOS)) {
        int slot = -1;
        for (int i = 0; i < NOUT && slot < 0; i++) if (!d->out_busy[i]) slot = i;
        if (slot >= 0) {
            d->out_busy[slot] = 1;
            d->ready[d->nready].slot = slot;
            d->ready[d->nready].time = time;
            d->ready[d->nready].flags = size > 0 ? 0 : FLAG_EOS;
            d->nready++;
            produced = 1;
        }
    }
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

/* GetOutputBuffer(int index, Buffer *): in the game's mode the Buffer holds an NVNtexture *
 * (capacity 8) and the frame is to be written into that texture. Not decoded yet. */
static void get_output_buffer(CPU *c, VideoDecoder *d) {
    (void)d;
    static int logged;
    uint64_t base = hle_call(c, fn.buffer_base, c->x[2], 0, 0, 0);
    uint64_t capacity = hle_call(c, fn.buffer_capacity, c->x[2], 0, 0, 0);
    if (!logged++) fprintf(stderr, "movie: first frame goes to buffer %#" PRIx64 " (capacity %" PRIu64 ")\n", base, capacity);
    c->x[0] = 0;
}

/* ReleaseOutputBufferIndex(int index) */
static void release_output(CPU *c, VideoDecoder *d) {
    int i = (int)c->x[1];
    pthread_mutex_lock(&d->lock);
    if (i >= 0 && i < NOUT) d->out_busy[i] = 0;
    pthread_mutex_unlock(&d->lock);
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
    /* the input buffers' guest memory is kept for the next decoder (see new_decoder) */
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
