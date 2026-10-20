/*
 * lm.c - "lm", the log manager: where the game's own log messages go (nn::diag).
 * We print the text of each message. switchbrew.org/wiki/Log_services
 */
#include "services.h"
#include "guest_memory.h"
#include <stdio.h>

/* A log packet: a 0x18-byte header, then chunks of (key, length, data), where key and
 * length are ULEB128 numbers (7 bits per byte, high bit = "more bytes follow"). */
enum { LOG_TEXT = 2, LOG_LINE = 3, LOG_FILE = 4, LOG_FUNCTION = 5 };

static uint64_t uleb(const uint8_t *p, uint64_t n, uint64_t *at) {
    uint64_t v = 0;
    for (int shift = 0; *at < n && shift < 64; shift += 7) {
        uint8_t b = p[(*at)++];
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
    }
    return v;
}

static void print_packet(const uint8_t *p, uint64_t n) {
    if (n < 0x18) return;
    uint8_t severity = p[0x12];
    static const char *const sev[] = { "trace", "info", "warn", "error", "fatal" };
    const uint8_t *text = NULL, *func = NULL;
    uint64_t text_len = 0, func_len = 0;
    for (uint64_t at = 0x18; at < n;) {
        uint64_t key = uleb(p, n, &at), len = uleb(p, n, &at);
        if (len > n - at) break;
        if (key == LOG_TEXT) { text = p + at; text_len = len; }
        if (key == LOG_FUNCTION) { func = p + at; func_len = len; }
        at += len;
    }
    if (!text) return;   /* a multi-packet message's other parts, or session begin/end */
    while (text_len && (text[text_len - 1] == '\n' || text[text_len - 1] == 0)) text_len--;
    const char *s = severity < 5 ? sev[severity] : "?";
    if (func)
        fprintf(stderr, "[lm %s] %.*s  (in %.*s)\n", s, (int)text_len, text, (int)func_len, func);
    else
        fprintf(stderr, "[lm %s] %.*s\n", s, (int)text_len, text);
}

static void logger_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0: {                   /* Log(X buffer: packet) */
        IpcBuffer b = ipc_in_buffer(call, 0);
        if (b.size && b.addr < guest_mem_size && b.size <= guest_mem_size - b.addr)
            print_packet(guest_mem + b.addr, b.size);
        break;
    }
    case 1:                     /* SetDestination(flags) */
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType logger_type = { "lm:ILogger", logger_dispatch, NULL };

static void lm_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0:                     /* OpenLogger(pid) -> ILogger */
        ipc_out_object(call, ipc_new_object(&logger_type, NULL));
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType lm_type = { "lm", lm_dispatch, NULL };

ServiceObject *service_lm_create(void) { return ipc_new_object(&lm_type, NULL); }
