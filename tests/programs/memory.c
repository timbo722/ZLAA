/* Stack arrays, globals (.data/.bss/.rodata), structs, every load/store width and signedness. */
#include <stdint.h>

static uint32_t gtable[64] = {
    1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987, 1597,
    0xdeadbeef, 0xcafebabe, 0x80000000, 0x7fffffff, 0, 0xffffffff, 12, 13,
};
static uint8_t gbuf[256];
static const int16_t ctab[16] = { -1, 300, -32768, 32767, 5, -5, 1000, -1000,
                                  7, 77, 777, 7777, -7, -77, -777, -7777 };

struct S { uint8_t a; int16_t b; uint32_t c; uint64_t d; int8_t e; };

uint64_t test_globals(uint64_t a, uint64_t b) {
    gtable[a & 63] += (uint32_t)b;
    gbuf[b & 255] = (uint8_t)a;
    return gtable[(a >> 6) & 63] + gbuf[a & 255] + (uint64_t)(int64_t)ctab[b & 15];
}

uint64_t test_sort(uint64_t a, uint64_t b) {
    uint64_t arr[32];
    uint64_t x = a ^ 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 32; i++) {   /* xorshift fill */
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        arr[i] = x % (b | 1);
    }
    for (int i = 1; i < 32; i++) {   /* insertion sort */
        uint64_t v = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > v) { arr[j + 1] = arr[j]; j--; }
        arr[j + 1] = v;
    }
    uint64_t h = 0;
    for (int i = 0; i < 32; i++) h = h * 31 + arr[i];
    return h;
}

uint64_t test_struct(uint64_t a, uint64_t b) {
    struct S s[8];
    for (int i = 0; i < 8; i++) {
        s[i].a = (uint8_t)(a >> i);
        s[i].b = (int16_t)(b >> (i * 2));
        s[i].c = (uint32_t)(a * (uint64_t)(i + 1));
        s[i].d = b ^ ((uint64_t)i << 40);
        s[i].e = (int8_t)(a >> (i * 3));
    }
    struct S t = s[a & 7];           /* struct copy */
    s[b & 7] = t;
    int64_t sum = 0;
    for (int i = 0; i < 8; i++) sum += s[i].a + s[i].b + (int64_t)s[i].c + (int64_t)s[i].d + s[i].e;
    return (uint64_t)sum;
}

static uint64_t my_strlen(const char *p) { const char *q = p; while (*q) q++; return (uint64_t)(q - p); }

uint64_t test_string(uint64_t a, uint64_t b) {
    char buf[32];
    int n = 0;
    uint64_t v = a;
    do { buf[n++] = (char)('0' + v % 10); v /= 10; } while (v);  /* number to decimal string */
    buf[n] = 0;
    for (int i = 0, j = n - 1; i < j; i++, j--) { char t = buf[i]; buf[i] = buf[j]; buf[j] = t; }
    uint64_t h = 5381;
    for (int i = 0; buf[i]; i++) h = h * 33 + (unsigned char)buf[i];
    return h ^ my_strlen(buf) ^ (b & 0xff);
}

uint64_t test_widths(uint64_t a, uint64_t b) {
    uint8_t bytes[24];
    __builtin_memcpy(bytes, &a, 8);
    __builtin_memcpy(bytes + 8, &b, 8);
    __builtin_memset(bytes + 16, 0xA5, 8);
    uint16_t h; uint32_t w; uint64_t d;
    __builtin_memcpy(&h, bytes + 3, 2);   /* unaligned loads */
    __builtin_memcpy(&w, bytes + 5, 4);
    __builtin_memcpy(&d, bytes + 7, 8);
    int8_t sb = (int8_t)bytes[(b & 15)];
    int16_t sh = (int16_t)h;
    int32_t sw = (int32_t)w;
    volatile int64_t mix = (int64_t)sb + sh + sw;   /* volatile: force a real store + load */
    return (uint64_t)mix ^ d ^ bytes[a & 15] ^ ((uint64_t)bytes[20] << 32);
}

/* Real calls to memset/memcpy/memmove (variable sizes, so the compiler can't inline them).
 * In the NSO tests these live in a separate "sdk" module, so this crosses modules. */
void *memset(void *dst, int v, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);
void *memmove(void *dst, const void *src, unsigned long n);

uint64_t test_libcalls(uint64_t a, uint64_t b) {
    uint8_t src[64], dst[64];
    unsigned long n = a & 63, m = (b >> 8) & 31;
    memset(src, (int)(b & 0xff), sizeof src);
    for (unsigned long i = 0; i < n; i++) src[i] = (uint8_t)(a >> (i & 7) * 8) + (uint8_t)i;
    memset(dst, 0x5a, sizeof dst);
    memcpy(dst, src, n);
    memmove(dst + 1, dst, m);   /* overlapping */
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 64; i++) h = (h ^ dst[i]) * 1099511628211ull;
    return h;
}
