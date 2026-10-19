/* SPDX-License-Identifier: GPL-2.0-or-later */
/* System call group: direct memory (libkernel). sceKernelAllocateDirectMemory,
   sceKernelMapDirectMemory and sceKernelReleaseDirectMemory over the pool in
   runtime/syslib/dmem_pool.c.

   What the calls do, from the public declarations in the OpenOrbis toolchain's libkernel.h (the
   same source the neighbouring entries cite):

     int sceKernelAllocateDirectMemory(off_t searchStart, off_t searchEnd, size_t len,
                                       size_t alignment, int memoryType, off_t *physAddrOut);
     int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, off_t physAddr,
                                  size_t alignment);
     int sceKernelReleaseDirectMemory(off_t start, size_t len);

   A title allocates a physical range, maps it to read and write it, and releases it when it is
   done; the physical address is the key the two later calls use, and it is also what a title hands
   to the GPU, which is why the pool hands out addresses inside the region rather than host
   pointers.

   The mapping is a fresh anonymous mapping per call, so the guest gets zeroed memory with exactly
   the protection it asked for. On a console the same physical pages can be mapped twice, and both
   mappings then show the same bytes; this runtime has no physical pages behind the pool to share,
   so two mappings of one range are independent. Nothing we have observed asks for that, and it is
   recorded as an open question in STATUS.md rather than approximated.

   memoryType selects the GPU's caching policy for the range (the toolchain's ORBIS_KERNEL_WB_* and
   ORBIS_KERNEL_WC_* values). Every one of them describes the same CPU-coherent memory, so the
   value is accepted and has no effect here; this runtime has no GPU path yet.

   Errors are the SCE form of a FreeBSD error number, as in runtime/syslib/kernel_thread.c. The
   documented failure cases of these calls are the ones used: a null result pointer, a size or
   alignment the region cannot satisfy, a range that is not one live allocation, and a mapping the
   host cannot place. */
#define _GNU_SOURCE
#include "runtime/dmem.h"
#include "runtime/process.h"
#include "runtime/syslib.h"

#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>

enum {
    BSD_EFAULT = 14,
    BSD_EINVAL = 22,
};
#define SCE_KERNEL_ERROR(e) ((int)(0x80020000u + (unsigned)(e)))

/* The protection bits the toolchain documents (ORBIS_KERNEL_PROT_*). The CPU bits are the ones
   this runtime can honour; the GPU bits (0x10 and above) describe access from the GPU to the same
   memory and have no meaning for the host mapping, so they are accepted and change nothing. */
enum {
    DMEM_PROT_CPU_READ = 0x01,
    DMEM_PROT_CPU_WRITE = 0x02,
    DMEM_PROT_CPU_EXEC = 0x04,
};

/* ORBIS_KERNEL_MAP_FIXED: map at the address the caller names, rather than wherever the system
   chooses. The other documented flag bits describe sharing and naming this runtime does not model
   yet; they are accepted and do not change the mapping, which is always private to the process. */
enum {
    DMEM_MAP_FIXED = 0x10,
};

/* How the region is aligned when the executable's image is rounded up: 2 MiB, the alignment the
   boot's own direct memory request uses. */
#define DMEM_REGION_ALIGNMENT UINT64_C(0x200000)

static int pool_ready;

/* The bottom of the region holds the executable: its image is what the loader mapped at the
   process's base, and a title's first direct memory request asks for the region from address 0,
   which must never hand back memory the executable is standing on. Reserving the image's size is a
   model of the console's physical layout, not a measurement of it: the one part of physical memory
   this runtime knows an owner for is that image. It also keeps address 0 out of circulation, which
   a guest that mistakes a physical address for a status would read as a failure. */
static void start_pool(void)
{
    if (pool_ready) return;
    uint64_t reserved = 0;
    if (rt_process.image_size) {
        const uint64_t image = rt_process.image_size;
        reserved = (image + DMEM_REGION_ALIGNMENT - 1) / DMEM_REGION_ALIGNMENT * DMEM_REGION_ALIGNMENT;
    }
    rt_dmem_pool_init(RT_DMEM_SIZE, reserved);
    pool_ready = 1;
}

static int sce_result(int result)
{
    return result == RT_DMEM_OK ? 0 : SCE_KERNEL_ERROR(result);
}

/* sceKernelAllocateDirectMemory: finds len bytes inside [searchStart, searchEnd) at the requested
   alignment and returns the range's physical address. eboot's first call searches the whole region
   (0 to sceKernelGetDirectMemorySize) for 116 MiB at 2 MiB alignment. */
static int dmem_allocate(int64_t search_start, int64_t search_end, uint64_t len, uint64_t alignment,
                         int memory_type, uint64_t *phys_addr_out)
{
    (void)memory_type; /* accepted: it selects a GPU cache policy, not a different kind of memory */
    if (!phys_addr_out) return SCE_KERNEL_ERROR(BSD_EFAULT);
    if (search_start < 0 || search_end < 0) return SCE_KERNEL_ERROR(BSD_EINVAL);
    start_pool();
    uint64_t phys = 0;
    const int result = rt_dmem_pool_allocate((uint64_t)search_start, (uint64_t)search_end, len, alignment, &phys);
    if (result != RT_DMEM_OK) return sce_result(result);
    *phys_addr_out = phys;
    return 0;
}
RT_SYSLIB("libkernel", sceKernelAllocateDirectMemory, dmem_allocate);

/* sceKernelMapDirectMemory: makes a live physical range readable and writable by the guest, at
   *addr when MAP_FIXED is asked for, and writes the address it used back to *addr. With MAP_FIXED
   the range must be free: an existing mapping is never replaced, because the runtime may not take
   away memory the guest is still using. */
static int dmem_map(void **addr, uint64_t len, int prot, int flags, uint64_t phys_addr, uint64_t alignment)
{
    if (!addr) return SCE_KERNEL_ERROR(BSD_EFAULT);
    if (!len || !alignment || (alignment & (alignment - 1))) return SCE_KERNEL_ERROR(BSD_EINVAL);
    if (phys_addr & (alignment - 1)) return SCE_KERNEL_ERROR(BSD_EINVAL);
    if ((flags & DMEM_MAP_FIXED) && !*addr) return SCE_KERNEL_ERROR(BSD_EINVAL);
    start_pool();
    /* Mapping memory the pool does not own would be a mapping of nothing; a console answers a
       range that is not allocated as an argument error. */
    if (!rt_dmem_pool_owns(phys_addr, len)) return SCE_KERNEL_ERROR(BSD_EINVAL);

    int host_prot = PROT_NONE;
    if (prot & DMEM_PROT_CPU_READ) host_prot |= PROT_READ;
    if (prot & DMEM_PROT_CPU_WRITE) host_prot |= PROT_WRITE;
    if (prot & DMEM_PROT_CPU_EXEC) host_prot |= PROT_EXEC;

    void *want = *addr;
    int host_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    /* Without MAP_FIXED the address is a preference, which is how mmap already treats it. */
    if (flags & DMEM_MAP_FIXED) host_flags |= MAP_FIXED_NOREPLACE;
    void *mapped = mmap(want, (size_t)len, host_prot, host_flags, -1, 0);
    if (mapped == MAP_FAILED) {
        /* EINVAL, ENOMEM and EEXIST mean the same thing on the host as in the guest's numbering. */
        return SCE_KERNEL_ERROR(errno);
    }
    *addr = mapped;
    return 0;
}
RT_SYSLIB("libkernel", sceKernelMapDirectMemory, dmem_map);

/* sceKernelReleaseDirectMemory: returns a physical range to the pool so a later allocation can use
   it. Only the range that was handed out goes back: an overlap, a partial release and a second
   release of the same range are all refused. A mapping the guest still holds keeps working after
   the release, because unmapping (sceKernelMunmap) is not implemented and the runtime has no
   record of which mappings exist; that is more forgiving than a console, and cannot corrupt the
   pool. */
static int dmem_release(int64_t start, uint64_t len)
{
    if (start < 0) return SCE_KERNEL_ERROR(BSD_EINVAL);
    start_pool();
    return sce_result(rt_dmem_pool_release((uint64_t)start, len));
}
RT_SYSLIB("libkernel", sceKernelReleaseDirectMemory, dmem_release);
