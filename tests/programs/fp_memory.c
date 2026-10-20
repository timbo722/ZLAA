/* Floating point in memory: S/D/Q register loads and stores, pairs, globals, struct copies,
 * and some realistic loops (game-style vector maths). */
#include <stdint.h>
#include "testlib.h"

typedef struct { float x, y, z; } Vec3;
typedef struct { Vec3 pos, vel; double t; float scale; } Body;

static float gtable[8] = { 1.0f, -2.5f, 3.25f, 1e-3f, 100.0f, -0.0f, 7.0f, 0.1f };
static double gd[4] = { 3.141592653589793, -1e300, 2.5e-310, 42.0 };
static Body bodies[4];

static float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

uint64_t test_vec3(uint64_t a, uint64_t b) {
    Vec3 u = { (float)(int16_t)a, (float)(int16_t)(a >> 16), (float)(int16_t)(a >> 32) };
    Vec3 v = { (float)(int8_t)b * 0.5f, gtable[b & 7], gtable[(b >> 3) & 7] };
    Vec3 cross = { u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
    float len2 = dot(cross, cross);
    float inv = len2 > 0 ? 1.0f / __builtin_sqrtf(len2) : 0.0f;
    uint64_t h = mix(fbits(dot(u, v)), fbits(len2));
    h = mix(h, fbits(cross.x * inv)); h = mix(h, fbits(cross.y * inv)); h = mix(h, fbits(cross.z * inv));
    return h;
}

uint64_t test_physics(uint64_t a, uint64_t b) {
    Body *bd = &bodies[a & 3];
    bd->pos.x += (float)(int8_t)b; bd->vel.y = (float)(int16_t)a * 0.01f;
    bd->scale = 1.0f + (float)(b & 15);
    for (int step = 0; step < 16; step++) {                  /* simple integrator */
        bd->vel.y -= 9.81f * (1.0f / 60.0f);
        bd->pos.x += bd->vel.x * (1.0f / 60.0f);
        bd->pos.y += bd->vel.y * (1.0f / 60.0f);
        if (bd->pos.y < 0.0f) { bd->pos.y = -bd->pos.y; bd->vel.y *= -0.8f; }
        bd->t += 1.0 / 60.0;
    }
    Body copy = *bd;                                          /* struct copy: LDP/STP Q */
    bodies[(a >> 2) & 3] = copy;
    return mix(mix(fbits(copy.pos.x), fbits(copy.pos.y)), mix(dbits(copy.t), fbits(copy.vel.y * copy.scale)));
}

uint64_t test_fp_arrays(uint64_t a, uint64_t b) {
    float arr[16];
    double darr[8];
    uint64_t s = a | 1;
    for (int i = 0; i < 16; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; arr[i] = (float)(int32_t)s / 1000.0f; }
    for (int i = 0; i < 8; i++) darr[i] = (double)arr[i] * gd[i & 3] + (double)(int64_t)b;
    for (int i = 1; i < 16; i++) {                            /* insertion sort on floats */
        float v = arr[i]; int j = i - 1;
        while (j >= 0 && arr[j] > v) { arr[j + 1] = arr[j]; j--; }
        arr[j + 1] = v;
    }
    double sum = 0, poly = 0;
    for (int i = 0; i < 8; i++) sum += darr[i];
    for (int i = 0; i < 16; i++) poly = poly * 0.5 + arr[i];   /* Horner */
    gtable[a & 7] = arr[b & 15];                               /* store to a global */
    uint64_t h = mix(dbits(sum), dbits(poly));
    for (int i = 0; i < 16; i += 5) h = mix(h, fbits(arr[i]));
    return mix(h, fbits(gtable[(a >> 3) & 7]));
}

uint64_t test_fp_bits(uint64_t a, uint64_t b) {
    /* moving raw bits between integer and FP registers, and 16-byte copies */
    float f = asf(a);
    double d = asd(b);
    uint64_t buf[4] = { a, b, a ^ b, a + b }, out[4];
    __builtin_memcpy(out, buf, 32);                            /* likely LDP/STP Q */
    double dd[2]; __builtin_memcpy(dd, &buf[1], 16);
    return mix(mix(fbits(f), dbits(d)), mix(out[2] + out[3], dbits(dd[0] + dd[1])));
}
