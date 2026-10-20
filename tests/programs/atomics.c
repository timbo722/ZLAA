/* Atomic operations. On ARMv8.0 (the Switch's CPU) these compile to LDXR/STXR loops
 * (load-exclusive / store-exclusive), LDAR/STLR (acquire loads / release stores) and CLREX.
 * Single-threaded here: this checks the values are right, not thread safety. */
#include <stdint.h>

static uint64_t g64 = 0x1111;
static uint32_t g32 = 7;
static uint16_t g16 = 300;
static uint8_t g8 = 9;

uint64_t test_atomic_rmw(uint64_t a, uint64_t b) {
    uint64_t r = __atomic_fetch_add(&g64, a, __ATOMIC_SEQ_CST);
    r ^= __atomic_fetch_sub(&g32, (uint32_t)b, __ATOMIC_ACQ_REL);
    r += __atomic_fetch_or(&g16, (uint16_t)a, __ATOMIC_RELAXED);
    r ^= __atomic_fetch_and(&g8, (uint8_t)(b | 0x81), __ATOMIC_ACQUIRE);
    r += __atomic_exchange_n(&g64, b ^ r, __ATOMIC_SEQ_CST);
    r ^= __atomic_fetch_xor(&g32, (uint32_t)(a >> 32), __ATOMIC_RELEASE);
    r += __atomic_add_fetch(&g8, (uint8_t)a, __ATOMIC_SEQ_CST);
    return r + g64 + g32 + g16 + g8;
}

uint64_t test_atomic_cas(uint64_t a, uint64_t b) {
    uint64_t expected = (a & 1) ? __atomic_load_n(&g64, __ATOMIC_RELAXED) : a;
    int ok = __atomic_compare_exchange_n(&g64, &expected, b, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    uint32_t e32 = (b & 2) ? g32 : (uint32_t)b;
    int ok32 = __atomic_compare_exchange_n(&g32, &e32, (uint32_t)a, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    uint16_t e16 = g16;
    while (!__atomic_compare_exchange_n(&g16, &e16, (uint16_t)(e16 * 3 + 1), 1,   /* weak CAS loop */
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    return (uint64_t)ok | ((uint64_t)ok32 << 1) | (expected << 2) ^ ((uint64_t)e32 << 20) ^ e16;
}

uint64_t test_atomic_loadstore(uint64_t a, uint64_t b) {
    __atomic_store_n(&g32, (uint32_t)a, __ATOMIC_RELEASE);     /* STLR */
    __atomic_store_n(&g16, (uint16_t)b, __ATOMIC_RELEASE);     /* STLRH */
    __atomic_store_n(&g8, (uint8_t)(a >> 8), __ATOMIC_SEQ_CST);
    uint64_t r = __atomic_load_n(&g64, __ATOMIC_ACQUIRE);     /* LDAR */
    r ^= __atomic_load_n(&g32, __ATOMIC_ACQUIRE);
    r += __atomic_load_n(&g16, __ATOMIC_SEQ_CST);
    r ^= (uint64_t)__atomic_load_n(&g8, __ATOMIC_ACQUIRE) << 40; /* LDARB */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);                     /* DMB */
    return r;
}

static uint8_t lock;
static uint64_t counter;
uint64_t test_spinlock(uint64_t a, uint64_t b) {
    for (unsigned i = 0; i < (a & 15) + 1; i++) {
        while (__atomic_test_and_set(&lock, __ATOMIC_ACQUIRE)) {}
        counter += b + i;
        __atomic_clear(&lock, __ATOMIC_RELEASE);
    }
    return counter;
}
