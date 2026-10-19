/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The direct memory pool: the region sceKernelGetDirectMemorySize reports, kept as free and live
   physical ranges. No host memory is touched here; sceKernelMapDirectMemory is what gives a range
   to the guest (runtime/syslib/dmem.c), and this file stays free of operating system calls so the
   bookkeeping can be tested on any host (test/test_dmem_pool.c).

   A title's life cycle is allocate, map, release. The console's kernel manages direct memory as
   physical pages; here the pool is a first-fit free list sorted by address and coalesced on
   release, so a title that allocates and frees in any order reuses the holes it made instead of
   failing early, and a run of a few hundred allocations leaves the region as one block again once
   they are released. */
#include "runtime/dmem.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct range {
    uint64_t start, len;
    struct range *next;
} range;

static range *free_ranges; /* sorted by start, adjacent ranges already merged */
static range *live_ranges; /* sorted by start */
static uint64_t region_size, region_reserved;

static void drop_ranges(range **list)
{
    while (*list) {
        range *next = (*list)->next;
        free(*list);
        *list = next;
    }
}

void rt_dmem_pool_init(uint64_t size, uint64_t reserved)
{
    drop_ranges(&free_ranges);
    drop_ranges(&live_ranges);
    region_size = size;
    region_reserved = reserved < size ? reserved : size;
    if (region_size == region_reserved) return;
    free_ranges = malloc(sizeof *free_ranges);
    if (!free_ranges) {
        /* Without bookkeeping the region is unusable; the first allocation then reports ENOMEM,
           which is the truth about what the pool can hand out. */
        region_size = region_reserved;
        return;
    }
    free_ranges->start = region_reserved;
    free_ranges->len = region_size - region_reserved;
    free_ranges->next = NULL;
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

/* Puts [start, start + len) into the live list, keeping it sorted. */
static range *remember(uint64_t start, uint64_t len)
{
    range *node = malloc(sizeof *node);
    if (!node) return NULL;
    node->start = start;
    node->len = len;
    range **at = &live_ranges;
    while (*at && (*at)->start < start) at = &(*at)->next;
    node->next = *at;
    *at = node;
    return node;
}

static void forget(const range *node)
{
    range **at = &live_ranges;
    while (*at && *at != node) at = &(*at)->next;
    if (*at) *at = node->next;
}

int rt_dmem_pool_allocate(uint64_t search_start, uint64_t search_end, uint64_t len, uint64_t alignment,
                          uint64_t *phys)
{
    if (!len || !alignment || (alignment & (alignment - 1))) return RT_DMEM_EINVAL;
    if (search_start > search_end || search_end > region_size) return RT_DMEM_EINVAL;
    if (search_start < region_reserved) search_start = region_reserved;
    /* The window is checked before the free list so that a request larger than the window is
       refused without walking anything, and so that no addition below can overflow. */
    if (len > search_end - search_start) return RT_DMEM_ENOMEM;

    for (range *block = free_ranges; block; block = block->next) {
        const uint64_t block_end = block->start + block->len;
        const uint64_t begin = block->start > search_start ? block->start : search_start;
        const uint64_t start = align_up(begin, alignment);
        if (start >= block_end || len > block_end - start) continue;
        /* Blocks are sorted, so if this one starts the range too high every later one does too. */
        if (start + len > search_end) break;
        range *live = remember(start, len);
        if (!live) return RT_DMEM_ENOMEM;

        /* The block keeps the room before the allocation; the room after it becomes its own
           range. The next range needs no merging: the allocation now separates the two. */
        const uint64_t tail_start = start + len;
        const uint64_t tail_len = block_end - tail_start;
        range *tail = NULL;
        if (tail_len) {
            tail = malloc(sizeof *tail);
            if (!tail) {
                forget(live);
                return RT_DMEM_ENOMEM;
            }
        }
        if (start == block->start) {
            if (tail) {
                *block = (range){tail_start, tail_len, block->next};
            } else {
                range **at = &free_ranges;
                while (*at != block) at = &(*at)->next;
                *at = block->next;
                free(block);
            }
        } else {
            block->len = start - block->start;
            if (tail) {
                tail->start = tail_start;
                tail->len = tail_len;
                tail->next = block->next;
                block->next = tail;
            }
        }
        *phys = start;
        return RT_DMEM_OK;
    }
    return RT_DMEM_ENOMEM;
}

int rt_dmem_pool_release(uint64_t start, uint64_t len)
{
    if (!len) return RT_DMEM_EINVAL;
    range *node = NULL;
    for (range *live = live_ranges; live; live = live->next)
        if (live->start == start && live->len == len) {
            node = live;
            break;
        }
    if (!node) return RT_DMEM_EINVAL; /* never handed out, already released, or only part of one */
    forget(node);

    range *prev = NULL, *next = free_ranges;
    while (next && next->start < start) {
        prev = next;
        next = next->next;
    }
    node->start = start;
    node->len = len;
    node->next = next;
    if (prev) prev->next = node;
    else free_ranges = node;

    if (next && start + len == next->start) {
        node->len += next->len;
        node->next = next->next;
        free(next);
    }
    if (prev && prev->start + prev->len == start) {
        prev->len += node->len;
        prev->next = node->next;
        free(node);
    }
    return RT_DMEM_OK;
}

int rt_dmem_pool_owns(uint64_t phys, uint64_t len)
{
    if (!len) return 0;
    for (const range *live = live_ranges; live && live->start <= phys; live = live->next)
        if (phys < live->start + live->len) return len <= live->start + live->len - phys;
    return 0;
}

uint64_t rt_dmem_pool_size(void)
{
    return region_size;
}

uint64_t rt_dmem_pool_free(void)
{
    uint64_t total = 0;
    for (const range *block = free_ranges; block; block = block->next) total += block->len;
    return total;
}
