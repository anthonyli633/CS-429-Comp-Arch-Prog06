#include "tdmm.h"

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/mman.h>

#define TDMM_HEAP_BYTES (64u * 1024u * 1024u) // 64 MiB baseline

typedef struct block_hdr {
    size_t size;
    uint8_t free;
    uint8_t _pad[3];  // ensures 4-byte alignment
    struct block_hdr *prev;
    struct block_hdr *next;
} block_hdr_t;

static void *g_heap_base = NULL;
static size_t g_heap_size = 0;
static block_hdr_t *g_head = NULL;
static alloc_strat_e g_strat = FIRST_FIT;

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
}

void *t_malloc(size_t size) {
    if (size == 0) return NULL;
    if (!g_heap_base) t_init(g_strat);
    if (!g_heap_base) return NULL;

    size_t need = ALIGN4(size);
    block_hdr_t *b = find_block(need);
    if (!b) return NULL;

    split_block(b, need);
    b->free = 0;

    void *p = payload_from_hdr(b);
    if ((uintptr_t)p % 4 != 0) return NULL;
    return p;
}

void t_free(void *ptr) {
    if (!ptr) return;
    if (!ptr_in_heap(ptr)) return;

    block_hdr_t *b = hdr_from_payload(ptr);
    if (!ptr_in_heap(b)) return;
    if (b->free) return;

    b->free = 1;
    merge(b);
}