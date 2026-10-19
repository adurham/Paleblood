/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RUNTIME_DMEM_H
#define RUNTIME_DMEM_H

/* Direct memory: the console's physical region that a title allocates from, maps into its address
   space and releases again (sceKernelAllocateDirectMemory, sceKernelMapDirectMemory,
   sceKernelReleaseDirectMemory, all in libkernel).

   A physical address here is an offset into that one region, not a host address: the pool owns
   ranges of it and hands them out, and only sceKernelMapDirectMemory turns a range into memory the
   guest can read or write. Keeping the two apart is what lets the runtime answer "is this
   allocation still live" without touching host memory, and it is why the pool has no operating
   system calls in it and can be tested on any host.

   The bookkeeping is single-threaded: the runtime has one guest thread so far (scePthreadCreate is
   not implemented), so a title cannot reach the pool from two threads at once. */

#include <stdint.h>

/* The region the pool covers, and the value sceKernelGetDirectMemorySize reports
   (ORBIS_KERNEL_MAIN_DMEM_SIZE): the console's 8 GiB of unified memory less the 2 GiB the kernel
   keeps. The two have to be one number, so both read this. */
#define RT_DMEM_SIZE UINT64_C(0x180000000)

/* Results are 0 or a FreeBSD error number, the numbering the console's kernel errors use; the
   system call implementations turn them into SCE codes (runtime/syslib/dmem.c). */
enum {
    RT_DMEM_OK = 0,
    RT_DMEM_ENOENT = 2,  /* no such allocation */
    RT_DMEM_ENOMEM = 12, /* no free range of that size */
    RT_DMEM_EFAULT = 14, /* a result the caller cannot receive */
    RT_DMEM_EINVAL = 22, /* an argument the region cannot satisfy */
};

/* Starts the pool over [0, size). [0, reserved) is part of the region but not allocatable: it is
   where the executable itself lives. Any earlier contents are dropped. */
void rt_dmem_pool_init(uint64_t size, uint64_t reserved);

/* Finds len bytes inside [search_start, search_end) at the requested alignment, marks them used and
   writes their physical address to *phys. The alignment must be a power of two; the size and the
   alignment need not be multiples of anything, because the pool rounds up internally and the guest
   only ever sees a start address at the alignment it asked for. */
int rt_dmem_pool_allocate(uint64_t search_start, uint64_t search_end, uint64_t len, uint64_t alignment,
                          uint64_t *phys);

/* Returns an allocation to the pool. start and len have to name exactly one live allocation: a
   range that was never handed out, one already released, or part of a live allocation is refused,
   which is also what makes an overlap or a double release impossible. */
int rt_dmem_pool_release(uint64_t start, uint64_t len);

/* Non-zero when [phys, phys + len) lies inside one live allocation. */
int rt_dmem_pool_owns(uint64_t phys, uint64_t len);

/* The region's size, and how much of it is free (the reserved prefix is not). */
uint64_t rt_dmem_pool_size(void);
uint64_t rt_dmem_pool_free(void);

#endif
