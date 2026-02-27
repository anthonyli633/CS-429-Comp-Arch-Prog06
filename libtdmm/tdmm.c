#include "tdmm.h"

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdio.h>
#include <linux/time.h>

#define TDMM_HEAP_BYTES (64u * 1024u * 1024u)

typedef struct block_hdr {
    size_t size;
    uint8_t free;
    uint8_t _pad[3];  // ensures 4-byte alignment
    struct block_hdr *prev;
    struct block_hdr *next;
} block_hdr_t;

typedef struct {
    // OS memory
    size_t bytes_from_os;
    // Memory usage
    size_t cur_inuse_bytes;
    size_t peak_inuse_bytes;
    // Utilization
    double util_sum;
    size_t num_util;
    // Time
    uint64_t malloc_ns_total, free_ns_total;
} tdmm_metrics_t;

static uint64_t now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *g_heap_base = NULL;
static size_t g_heap_size = 0;
static block_hdr_t *g_head = NULL;
static alloc_strat_e g_strat = FIRST_FIT;
static tdmm_metrics_t g_metrics = {0};

static size_t ALIGN4(size_t x) {
    return (x + 3) / 4 * 4;
}
static size_t page_round_up(size_t n) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    size_t p = (size_t)ps;
    return (n + p - 1) / p * p;
}
static size_t hdr_size(void) {
    return ALIGN4(sizeof(block_hdr_t));
}

static void *payload_from_hdr(block_hdr_t *h) {
    return (void *)((uint8_t *)h + hdr_size());
}

static block_hdr_t *hdr_from_payload(void *p) {
    return (block_hdr_t *)((uint8_t *)p - hdr_size());
}

static int ptr_in_heap(const void *p) {
    if (!g_heap_base || g_heap_size == 0) return 0;
    uintptr_t x = (uintptr_t)p;
    uintptr_t b = (uintptr_t)g_heap_base;
    return (x >= b) && (x < b + g_heap_size);
}

static void merge(block_hdr_t *b) {
    if (!b) return;
    while (b->prev && b->prev->free) b = b->prev;
    while (b->next && b->next->free) {
        block_hdr_t *n = b->next;
        b->size += hdr_size() + n->size;
        b->next = n->next;
        if (b->next) b->next->prev = b;
    }
}

static block_hdr_t *find_block(size_t need) {
    block_hdr_t *choice = NULL;
    if (g_strat == FIRST_FIT) {
        for (block_hdr_t *cur = g_head; cur; cur = cur->next) {
            if (cur->free && cur->size >= need) return cur;
        }
        return NULL;
    }
    if (g_strat == BEST_FIT) {
        for (block_hdr_t *cur = g_head; cur; cur = cur->next) {
            if (!cur->free || cur->size < need) continue;
            if (!choice || cur->size < choice->size) choice = cur;
        }
        return choice;
    }
    if (g_strat == WORST_FIT) {
        for (block_hdr_t *cur = g_head; cur; cur = cur->next) {
            if (!cur->free || cur->size < need) continue;
            if (!choice || cur->size > choice->size) choice = cur;
        }
        return choice;
    }
    return NULL;
}

static void split_block(block_hdr_t *b, size_t need) {
    if (!b || b->size < need) return;

    size_t hsz = hdr_size();
    size_t remaining = b->size - need;

    // Only split if leftover can hold a header + at least 4 bytes payload
    if (remaining < hsz + 4) return;

    uint8_t *new_addr = (uint8_t *)payload_from_hdr(b) + need;
    block_hdr_t *n = (block_hdr_t *)new_addr;

    n->size = remaining - hsz;
    n->free = 1;
    n->prev = b;
    n->next = b->next;

    if (b->next) b->next->prev = n;
    b->next = n;
    b->size = need;
}

typedef enum {
    METRIC_INIT = 0,
    METRIC_MALLOC,
    METRIC_FREE,
} metric_event_t;


static void update_metrics(metric_event_t ev, size_t req_bytes, size_t actual_bytes, uint64_t t0_ns, uint64_t t1_ns) {
    g_metrics.bytes_from_os = g_heap_size;

    uint64_t dt = (t1_ns >= t0_ns) ? (t1_ns - t0_ns) : 0;
    if (ev == METRIC_MALLOC) g_metrics.malloc_ns_total += dt;
    else if (ev == METRIC_FREE) g_metrics.free_ns_total += dt;

    if (ev == METRIC_MALLOC) {
        g_metrics.cur_inuse_bytes += actual_bytes;
        g_metrics.peak_inuse_bytes = max(g_metrics.peak_inuse_bytes, g_metrics.cur_inuse_bytes);
    } else if (ev == METRIC_FREE) {
        g_metrics.cur_inuse_bytes = max(0, (ssize_t)g_metrics.cur_inuse_bytes - (ssize_t)actual_bytes);
    }

    if (g_metrics.bytes_from_os > 0) {
        double u = (double)g_metrics.cur_inuse_bytes / (double)g_metrics.bytes_from_os;
        g_metrics.util_sum += u;
        g_metrics.num_util += 1;
    }
}

void display_metrics(void) {
    printf("\n===== TDMM METRICS =====\n");

    printf("OS bytes (mmap):        %zu\n", g_metrics.bytes_from_os);
    printf("Current in-use bytes:   %zu\n", g_metrics.cur_inuse_bytes);
    printf("Peak in-use bytes:      %zu\n", g_metrics.peak_inuse_bytes);

    double peak_util = 0.0;
    if (g_metrics.bytes_from_os > 0)
        peak_util = (double)g_metrics.peak_inuse_bytes /
                    (double)g_metrics.bytes_from_os;

    double avg_util = 0.0;
    if (g_metrics.num_util > 0)
        avg_util = g_metrics.util_sum /
                   (double)g_metrics.num_util;

    printf("Peak utilization:       %.6f\n", peak_util);
    printf("Average utilization:    %.6f\n", avg_util);

    printf("Total malloc time (ns): %llu\n",
           (unsigned long long)g_metrics.malloc_ns_total);
    printf("Total free time (ns):   %llu\n",
           (unsigned long long)g_metrics.free_ns_total);

    if (g_metrics.num_util > 0) {
        printf("Samples taken:          %zu\n", g_metrics.num_util);
    }

    printf("========================\n\n");
}

void t_init(alloc_strat_e strat) {
    g_strat = strat;

    size_t req = page_round_up((size_t)TDMM_HEAP_BYTES);
    void *mem = mmap(NULL, req, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        g_heap_base = NULL;
        g_heap_size = 0;
        g_head = NULL;
        return;
    }

    g_heap_base = mem;
    g_heap_size = req;

    g_head = (block_hdr_t *)g_heap_base;
    g_head->size = g_heap_size - hdr_size();
    g_head->free = 1;
    g_head->prev = NULL;
    g_head->next = NULL;

    update_metrics(METRIC_INIT, 0, 0, 0, 0);
}

void *t_malloc(size_t size) {
    uint64_t t0 = now();

    if (size == 0) { update_metrics(METRIC_MALLOC, 0, 0, t0, now()); return NULL; }
    if (!g_heap_base) t_init(g_strat);
    if (!g_heap_base) { update_metrics(METRIC_MALLOC, size, 0, t0, now()); return NULL; }

    size_t need = ALIGN4(size);
    block_hdr_t *b = find_block(need);
    if (!b) { update_metrics(METRIC_MALLOC, size, 0, t0, now()); return NULL; }

    split_block(b, need);
    b->free = 0;

    void *p = payload_from_hdr(b);
    if ((uintptr_t)p % 4 != 0) { update_metrics(METRIC_MALLOC, size, 0, t0, now()); return NULL; }

    update_metrics(METRIC_MALLOC, size, need, t0, now());
    return p;
}

void t_free(void *ptr) {
    uint64_t t0 = now();

    if (!ptr) { update_metrics(METRIC_FREE, 0, 0, t0, now()); return; }
    if (!ptr_in_heap(ptr)) { update_metrics(METRIC_FREE, 0, 0, t0, now()); return; }

    block_hdr_t *b = hdr_from_payload(ptr);
    if (!ptr_in_heap(b)) { update_metrics(METRIC_FREE, 0, 0, t0, now()); return; }
    if (b->free) { update_metrics(METRIC_FREE, 0, 0, t0, now()); return; }

    size_t freed = b->size;
    b->free = 1;
    merge(b);
    update_metrics(METRIC_FREE, 0, freed, t0, now());
}