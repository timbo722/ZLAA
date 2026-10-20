/* Calls, recursion, loops, switch jump tables, function pointers, tail calls, stack args. */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

NOINLINE static uint64_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

NOINLINE static uint64_t gcd(uint64_t a, uint64_t b) {
    while (b) { uint64_t t = a % b; a = b; b = t; }
    return a;
}

uint64_t test_recursion(uint64_t a, uint64_t b) {
    return fib((uint32_t)(a & 15)) * 1000 + fib((uint32_t)(b & 7)) + gcd(a, b);
}

uint64_t test_loops(uint64_t a, uint64_t b) {
    uint64_t n = a & 255, sum = 0;
    for (uint64_t i = 0; i < n; i++) sum += i * b + (i ^ a);
    for (unsigned i = 0; i < (b & 7); i++)
        for (unsigned j = 0; j < (a & 7); j++) sum ^= (uint64_t)i * 31 + j;
    uint64_t x = a | 1;
    unsigned steps = 0;
    while (x != 1 && steps < 200) { x = (x & 1) ? 3 * x + 1 : x >> 1; steps++; }  /* Collatz */
    do { sum += steps; steps >>= 1; } while (steps);
    return sum ^ x;
}

uint64_t test_switch(uint64_t a, uint64_t b) {
    uint64_t r;
    switch (a & 15) {   /* dense: becomes a jump table (BR) */
    case 0: r = b; break;
    case 1: r = b + 1; break;
    case 2: r = b * 3; break;
    case 3: r = a ^ b; break;
    case 4: r = 99; break;
    case 5: r = a - 1; break;
    case 6: r = b << 2; break;
    case 7: r = b >> 3; break;
    case 8: r = ~b; break;
    case 9: r = a & b; break;
    case 10: r = 12345; break;
    case 11: r = a | b; break;
    default: r = a + b;
    }
    switch ((uint32_t)b) {  /* sparse: becomes a compare tree */
    case 1: r += 10; break;
    case 1000: r += 20; break;
    case 0xffffffffu: r += 30; break;
    case 77777: r += 40; break;
    default: break;
    }
    return r;
}

typedef uint64_t (*op_fn)(uint64_t, uint64_t);
NOINLINE static uint64_t op_add(uint64_t a, uint64_t b) { return a + b; }
NOINLINE static uint64_t op_sub(uint64_t a, uint64_t b) { return a - b; }
NOINLINE static uint64_t op_xor(uint64_t a, uint64_t b) { return a ^ b; }
NOINLINE static uint64_t op_mul(uint64_t a, uint64_t b) { return a * b; }
NOINLINE static uint64_t op_fib(uint64_t a, uint64_t b) { return fib((uint32_t)((a + b) & 7)); }
static op_fn ops[8] = { op_add, op_sub, op_xor, op_mul, op_fib, op_add, op_xor, op_sub };

uint64_t test_fnptr(uint64_t a, uint64_t b) {   /* BLR through a table in .data */
    return ops[a & 7](a, b) + ops[(b >> 3) & 7](b, a);
}

NOINLINE static uint64_t many_args(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
                                   uint64_t f, uint64_t g, uint64_t h, uint64_t i, uint64_t j,
                                   uint32_t k, uint8_t l) {
    /* arguments 9+ are passed on the stack */
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j + 11 * k + l;
}

uint64_t test_manyargs(uint64_t a, uint64_t b) {
    return many_args(a, b, a + 1, b + 2, a ^ b, a * 3, b - 7, 42, a >> 3, b << 2, (uint32_t)a, (uint8_t)b);
}

NOINLINE uint64_t tail_a(uint64_t x) { return x * 7 + 1; }
NOINLINE uint64_t tail_b(uint64_t x, uint64_t y) { return (x ^ y) + 3; }

uint64_t test_tailcall(uint64_t a, uint64_t b) {
    if (a & 1) return tail_a(a ^ b);   /* compiled as B to another function */
    return tail_b(b, a);
}

static int32_t counter;  /* global state, changes across calls */
uint64_t test_state(uint64_t a, uint64_t b) {
    counter += (int32_t)(a & 0xff) - (int32_t)(b & 0x7f);
    return (uint64_t)(int64_t)counter;
}
