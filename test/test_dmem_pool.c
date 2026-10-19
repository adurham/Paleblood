/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The direct memory pool (runtime/syslib/dmem_pool.c), tested on the host: alignment, the search
   window, reuse after release, refusal of overlaps and double releases, and the fragmentation
   behaviour a title that allocates a few hundred direct memory ranges depends on.

   The pool has no operating system calls, so this test runs anywhere the project builds; the
   system calls over it (allocate/map/release, runtime/syslib/dmem.c) are covered through the
   RT_SYSLIB registry in test/test_syslib.c. */
#include "runtime/dmem.h"

#include <stdint.h>
#include <stdio.h>

#define MiB UINT64_C(0x100000)

static int failures;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                             \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* Where the executable sits: the boot model reserves the image at the bottom of the region (see
   runtime/syslib/dmem.c), 116 MiB here, the size of the first direct memory request. */
static const uint64_t reserved_prefix = 0x7400000;
static const uint64_t region = RT_DMEM_SIZE;

static uint64_t allocate(uint64_t start, uint64_t end, uint64_t len, uint64_t alignment, int *result)
{
    uint64_t phys = ~(uint64_t)0;
    const int r = rt_dmem_pool_allocate(start, end, len, alignment, &phys);
    if (result) *result = r;
    return r == RT_DMEM_OK ? phys : 0;
}

static int disjoint(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return a + alen <= b || b + blen <= a;
}

int main(void)
{
    /* An empty pool covers the region except the executable's image. */
    rt_dmem_pool_init(region, reserved_prefix);
    CHECK(rt_dmem_pool_size() == region);
    CHECK(rt_dmem_pool_free() == region - reserved_prefix);

    /* The boot's own request: 116 MiB at 2 MiB alignment, searched from 0 to the region's end. It
       lands at the end of the reserved prefix, which is where the region's allocatable part starts,
       and the reservation is what keeps address 0 out of circulation. */
    int result = -1;
    uint64_t boot = allocate(0, region, 0x7400000, 0x200000, &result);
    CHECK(result == RT_DMEM_OK);
    CHECK(boot == reserved_prefix);
    CHECK(boot % 0x200000 == 0);
    CHECK(rt_dmem_pool_owns(boot, 0x7400000));
    CHECK(rt_dmem_pool_free() == region - 2 * reserved_prefix);

    /* A second allocation cannot overlap the first, and is aligned as asked. */
    uint64_t small = allocate(0, region, 0x20000, 0x10000, &result);
    CHECK(result == RT_DMEM_OK);
    CHECK(small % 0x10000 == 0);
    CHECK(disjoint(boot, 0x7400000, small, 0x20000));
    CHECK(rt_dmem_pool_owns(small, 0x20000));

    /* Ownership is exact: a range that runs past the allocation is not the allocation's. */
    CHECK(!rt_dmem_pool_owns(boot, 0x7400001));
    CHECK(!rt_dmem_pool_owns(boot - 1, 1));
    CHECK(!rt_dmem_pool_owns(small + 0x20000, 1));

    /* A release returns the range, and the next allocation of the same size reuses it. */
    CHECK(rt_dmem_pool_release(boot, 0x7400000) == RT_DMEM_OK);
    CHECK(!rt_dmem_pool_owns(boot, 0x7400000));
    uint64_t again = allocate(0, region, 0x7400000, 0x200000, &result);
    CHECK(result == RT_DMEM_OK && again == boot);

    /* Overlaps and double releases are refused, as are partial releases: only a range that is
       exactly one live allocation goes back to the pool. */
    CHECK(rt_dmem_pool_release(again, 0x7400000) == RT_DMEM_OK);
    CHECK(rt_dmem_pool_release(again, 0x7400000) == RT_DMEM_EINVAL);       /* twice */
    CHECK(rt_dmem_pool_release(again, 0x4000000) == RT_DMEM_EINVAL);       /* part of one */
    CHECK(rt_dmem_pool_release(again + 0x200000, 0x7200000) == RT_DMEM_EINVAL);
    CHECK(rt_dmem_pool_release(region - MiB, MiB) == RT_DMEM_EINVAL);      /* never handed out */
    CHECK(rt_dmem_pool_release(small, 0) == RT_DMEM_EINVAL);
    CHECK(rt_dmem_pool_owns(small, 0x20000)); /* the refusals changed nothing */
    CHECK(rt_dmem_pool_release(small, 0x20000) == RT_DMEM_OK);

    /* Arguments the region cannot satisfy are refused instead of being rounded into something
       that fits: a size of zero, an alignment that is not a power of two, a window outside the
       region or one that ends before it starts. */
    CHECK(allocate(0, region, 0, 0x1000, &result) == 0 && result == RT_DMEM_EINVAL);
    CHECK(allocate(0, region, 0x1000, 0, &result) == 0 && result == RT_DMEM_EINVAL);
    CHECK(allocate(0, region, 0x1000, 3, &result) == 0 && result == RT_DMEM_EINVAL);
    CHECK(allocate(region, region + 1, 0x1000, 0x1000, &result) == 0 && result == RT_DMEM_EINVAL);
    CHECK(allocate(0x2000, 0x1000, 0x1000, 0x1000, &result) == 0 && result == RT_DMEM_EINVAL);

    /* The search window is honoured even when the region has room elsewhere. */
    rt_dmem_pool_init(region, 0);
    CHECK(rt_dmem_pool_free() == region);
    uint64_t windowed = allocate(0x20000000, 0x20100000, 0x100000, 0x100000, &result);
    CHECK(result == RT_DMEM_OK);
    CHECK(windowed == 0x20000000);
    CHECK(allocate(0x20000000, 0x20100000, 0x200000, 0x100000, &result) == 0 && result == RT_DMEM_ENOMEM);
    CHECK(rt_dmem_pool_release(windowed, 0x100000) == RT_DMEM_OK);

    /* A request larger than the region, or than the window it names, fails with ENOMEM rather than
       wrapping. Asking for the whole region is not an error: it fits. */
    CHECK(allocate(0, region, region + MiB, 0x1000, &result) == 0 && result == RT_DMEM_ENOMEM);
    CHECK(allocate(0, region, UINT64_MAX, 0x1000, &result) == 0 && result == RT_DMEM_ENOMEM);
    CHECK(allocate(0, region / 2, region, 0x1000, &result) == 0 && result == RT_DMEM_ENOMEM);

    /* Fill the region, check that the failure is the pool's own answer, then take it all back:
       the region has to be one free range again. */
    uint64_t filled = 0, phys;
    while (rt_dmem_pool_allocate(0, region, MiB, MiB, &phys) == RT_DMEM_OK) filled += MiB;
    CHECK(filled == region);
    CHECK(rt_dmem_pool_free() == 0);
    rt_dmem_pool_init(region, reserved_prefix);
    CHECK(rt_dmem_pool_free() == region - reserved_prefix);

    /* A hole left by a released allocation is reused before the region's end is consumed: 300
       allocations of different sizes and alignments, all disjoint, then all released, leave the
       region free again and the same 300 can be made a second time. */
    enum { COUNT = 300 };
    uint64_t phys_addr[COUNT], len[COUNT];
    uint64_t state = 12345;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < COUNT; i++) {
            state = state * 6364136223846793005u + 1442695040888963407u;
            len[i] = (1 + (state >> 33) % 8) * MiB;
            const uint64_t alignment = (uint64_t)1 << (12 + (state >> 45) % 10);
            phys_addr[i] = allocate(0, region, len[i], alignment, &result);
            CHECK(result == RT_DMEM_OK);
            if (result != RT_DMEM_OK) return 1;
            CHECK(phys_addr[i] % alignment == 0);
            CHECK(rt_dmem_pool_owns(phys_addr[i], len[i]));
            for (int j = 0; j < i; j++) CHECK(disjoint(phys_addr[i], len[i], phys_addr[j], len[j]));
        }
        /* Released out of order, so neighbours are joined from both sides. */
        int order[COUNT];
        for (int i = 0; i < COUNT; i++) order[i] = i;
        for (int i = COUNT - 1; i > 0; i--) {
            state = state * 6364136223846793005u + 1442695040888963407u;
            const int j = (int)((state >> 33) % (unsigned)(i + 1));
            const int swap = order[i];
            order[i] = order[j];
            order[j] = swap;
        }
        for (int i = 0; i < COUNT; i++) CHECK(rt_dmem_pool_release(phys_addr[order[i]], len[order[i]]) == RT_DMEM_OK);
        CHECK(rt_dmem_pool_free() == region - reserved_prefix);
    }

    if (failures) {
        fprintf(stderr, "dmem pool: %d check(s) failed\n", failures);
        return 1;
    }
    printf("dmem pool: ok\n");
    return 0;
}
