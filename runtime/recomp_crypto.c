/*
 * recomp_crypto.c - the ARMv8 Cryptographic Extension instructions (AESE/AESD/AESMC/AESIMC,
 * SHA1C/P/M/H/SU0/SU1, SHA256H/H2/SU0/SU1), following the Arm pseudocode.
 */
#include "recomp_runtime.h"

static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static uint8_t inv_sbox(uint8_t v) {
    static uint8_t inv[256];
    static int ready;
    if (!ready) { for (int i = 0; i < 256; i++) inv[sbox[i]] = (uint8_t)i; ready = 1; }
    return inv[v];
}

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xtime(a); b >>= 1; }
    return r;
}

void recomp_aes(V128 *d, const V128 *n, int op) {
    V128 in = *n, out;
    if (op <= 1) {                               /* AESE / AESD: AddRoundKey, (Inv)ShiftRows, (Inv)SubBytes */
        for (int i = 0; i < 16; i++) in.b[i] ^= d->b[i];
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 4; row++) {
                if (op == 0) out.b[row + 4 * col] = sbox[in.b[row + 4 * ((col + row) % 4)]];
                else out.b[row + 4 * ((col + row) % 4)] = inv_sbox(in.b[row + 4 * col]);
            }
    } else {                                     /* AESMC / AESIMC: (Inv)MixColumns */
        static const uint8_t mc[4] = { 2, 3, 1, 1 }, imc[4] = { 14, 11, 13, 9 };
        const uint8_t *m = op == 2 ? mc : imc;
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 4; row++) {
                uint8_t v = 0;
                for (int k = 0; k < 4; k++) v ^= gmul(in.b[4 * col + k], m[(k - row + 4) % 4]);
                out.b[4 * col + row] = v;
            }
    }
    *d = out;
}

static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
static uint32_t ror(uint32_t x, int s) { return (x >> s) | (x << (32 - s)); }
static uint32_t choose(uint32_t x, uint32_t y, uint32_t z) { return ((y ^ z) & x) ^ z; }
static uint32_t parity(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static uint32_t majority(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | ((x | y) & z); }

static void sha256hash(uint32_t X[4], uint32_t Y[4], const uint32_t W[4]) {
    for (int e = 0; e < 4; e++) {
        uint32_t chs = choose(Y[0], Y[1], Y[2]), maj = majority(X[0], X[1], X[2]);
        uint32_t t = Y[3] + (ror(Y[0], 6) ^ ror(Y[0], 11) ^ ror(Y[0], 25)) + chs + W[e];
        X[3] = t + X[3];
        Y[3] = t + (ror(X[0], 2) ^ ror(X[0], 13) ^ ror(X[0], 22)) + maj;
        /* <Y, X> = ROL(Y:X, 32) */
        uint32_t y3 = Y[3], x3 = X[3];
        X[3] = X[2]; X[2] = X[1]; X[1] = X[0]; X[0] = y3;
        Y[3] = Y[2]; Y[2] = Y[1]; Y[1] = Y[0]; Y[0] = x3;
    }
}

void recomp_sha3(V128 *d, const V128 *n, const V128 *m, int op) {
    V128 r;
    if (op <= 2) {                               /* SHA1C / SHA1P / SHA1M  Qd, Sn, Vm.4S */
        uint32_t X[4] = { d->s[0], d->s[1], d->s[2], d->s[3] }, Y = n->s[0];
        for (int e = 0; e < 4; e++) {
            uint32_t t = op == 0 ? choose(X[1], X[2], X[3]) : op == 1 ? parity(X[1], X[2], X[3])
                                                                 : majority(X[1], X[2], X[3]);
            Y = Y + rol(X[0], 5) + t + m->s[e];
            X[1] = rol(X[1], 30);
            uint32_t x3 = X[3];
            X[3] = X[2]; X[2] = X[1]; X[1] = X[0]; X[0] = Y; Y = x3;
        }
        for (int k = 0; k < 4; k++) r.s[k] = X[k];
    } else if (op == 3) {                        /* SHA1SU0 */
        r.s[0] = d->s[2]; r.s[1] = d->s[3]; r.s[2] = n->s[0]; r.s[3] = n->s[1];
        for (int k = 0; k < 4; k++) r.s[k] ^= d->s[k] ^ m->s[k];
    } else if (op == 4 || op == 5) {             /* SHA256H / SHA256H2 */
        uint32_t X[4], Y[4], W[4];
        for (int k = 0; k < 4; k++) { X[k] = d->s[k]; Y[k] = n->s[k]; W[k] = m->s[k]; }
        if (op == 4) { sha256hash(X, Y, W); for (int k = 0; k < 4; k++) r.s[k] = X[k]; }
        else { sha256hash(Y, X, W); for (int k = 0; k < 4; k++) r.s[k] = X[k]; }
    } else {                                     /* SHA256SU1 */
        uint32_t T0[4] = { n->s[1], n->s[2], n->s[3], m->s[0] };
        for (int e = 0; e < 4; e++) {
            uint32_t elt = e < 2 ? m->s[2 + e] : r.s[e - 2];
            elt = ror(elt, 17) ^ ror(elt, 19) ^ (elt >> 10);
            r.s[e] = elt + d->s[e] + T0[e];
        }
    }
    *d = r;
}

void recomp_sha2(V128 *d, const V128 *n, int op) {
    V128 r;
    memset(&r, 0, sizeof r);
    if (op == 0) {                               /* SHA1H Sd, Sn */
        r.s[0] = rol(n->s[0], 30);
    } else if (op == 1) {                        /* SHA1SU1 */
        uint32_t T[4] = { d->s[0] ^ n->s[1], d->s[1] ^ n->s[2], d->s[2] ^ n->s[3], d->s[3] };
        r.s[0] = rol(T[0], 1); r.s[1] = rol(T[1], 1); r.s[2] = rol(T[2], 1);
        r.s[3] = rol(T[3], 1) ^ rol(T[0], 2);
    } else {                                     /* SHA256SU0 */
        uint32_t T[4] = { d->s[1], d->s[2], d->s[3], n->s[0] };
        for (int e = 0; e < 4; e++) r.s[e] = (ror(T[e], 7) ^ ror(T[e], 18) ^ (T[e] >> 3)) + d->s[e];
    }
    *d = r;
}
