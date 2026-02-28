#define _POSIX_C_SOURCE 200809L

#include "tdmm.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef EXPECT
#define EXPECT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)
#endif

typedef struct {
    size_t bytes_from_os;
    size_t cur_inuse_bytes;
    size_t peak_inuse_bytes;
    double util_sum;
    size_t num_util;
} tdmm_metrics_t;

extern const tdmm_metrics_t *t_metrics_ptr(void);
extern size_t t_overhead_bytes(void);

static void reset_and_init(alloc_strat_e strat) {
    t_reset();
    t_init(strat);
    const tdmm_metrics_t *m = t_metrics_ptr();
    EXPECT(m != NULL);
    EXPECT(m->bytes_from_os > 0);
    EXPECT(m->cur_inuse_bytes == 0);
}

static void test_alignment(alloc_strat_e strat) {
    reset_and_init(strat);
    for (size_t sz = 1; sz <= 256; sz++) {
        void *p = t_malloc(sz);
        EXPECT(p != NULL);
        EXPECT(((uintptr_t)p % 4u) == 0u);
        t_free(p);
    }
}

static void test_non_overlap_simple(alloc_strat_e strat) {
    reset_and_init(strat);

    void *a = t_malloc(64);
    void *b = t_malloc(64);
    EXPECT(a && b);
    EXPECT(a != b);

    memset(a, 0xAA, 64);
    memset(b, 0xBB, 64);

    EXPECT(((unsigned char*)a)[0] == 0xAA);
    EXPECT(((unsigned char*)b)[0] == 0xBB);

    t_free(a);
    t_free(b);
}

static void test_split_and_reuse(alloc_strat_e strat) {
    reset_and_init(strat);

    void *a = t_malloc(1024);
    EXPECT(a);

    t_free(a);
    void *b = t_malloc(128);
    EXPECT(b);
    EXPECT(b == a);

    t_free(b);
}

static void test_coalesce_all(alloc_strat_e strat) {
    reset_and_init(strat);

    size_t before = t_overhead_bytes();

    void *a = t_malloc(256);
    void *b = t_malloc(256);
    void *c = t_malloc(256);
    EXPECT(a && b && c);

    size_t during = t_overhead_bytes();
    EXPECT(during >= before);

    t_free(a);
    t_free(b);
    t_free(c);
    size_t after = t_overhead_bytes();
    EXPECT(after <= during);
}

static void test_double_free_safe(alloc_strat_e strat) {
    reset_and_init(strat);

    void *p = t_malloc(128);
    EXPECT(p);

    t_free(p);
    t_free(p);

    void *q = t_malloc(128);
    EXPECT(q);
    t_free(q);
}

static void test_invalid_free_safe(alloc_strat_e strat) {
    reset_and_init(strat);

    int x = 123;
    t_free(&x);
    t_free((void*)(uintptr_t)0x12345);

    void *p = t_malloc(64);
    EXPECT(p);
    t_free(p);
}

static void test_inuse_bookkeeping(alloc_strat_e strat) {
    reset_and_init(strat);
    const tdmm_metrics_t *m = t_metrics_ptr();

    void *a = t_malloc(10);
    void *b = t_malloc(10);
    EXPECT(a && b);

    EXPECT(m->cur_inuse_bytes > 0);
    EXPECT(m->cur_inuse_bytes <= m->bytes_from_os);

    t_free(a);
    t_free(b);

    EXPECT(m->cur_inuse_bytes == 0);
    EXPECT(m->peak_inuse_bytes > 0);
}

static void test_out_of_memory_returns_null(alloc_strat_e strat) {
    reset_and_init(strat);
    const tdmm_metrics_t *m = t_metrics_ptr();
    size_t too_big = m->bytes_from_os;

    void *p = t_malloc(too_big);
    EXPECT(p == NULL);
    void *q = t_malloc(64);
    EXPECT(q != NULL);
    t_free(q);
}

static void run_all_for_policy(alloc_strat_e strat) {
    printf("== Running unit tests for %d ==\n", (int)strat);

    test_alignment(strat);
    test_non_overlap_simple(strat);
    test_split_and_reuse(strat);
    test_coalesce_all(strat);
    test_double_free_safe(strat);
    test_invalid_free_safe(strat);
    test_inuse_bookkeeping(strat);
    test_out_of_memory_returns_null(strat);

    printf("PASS: policy %d\n\n", (int)strat);
}

int main(void) {
    run_all_for_policy(FIRST_FIT);
    run_all_for_policy(BEST_FIT);
    run_all_for_policy(WORST_FIT);

    printf("ALL TESTS PASSED\n");
    return 0;
}