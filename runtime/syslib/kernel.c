/* SPDX-License-Identifier: GPL-2.0-or-later */
/* System call group: kernel, threads and memory (libkernel, libScePosix, libSceSysmodule,
   libSceLibcInternal). */
#include "runtime/process.h"
#include "runtime/syslib.h"

/* The process parameter block: the executable's PT_SCE_PROCPARAM segment as mapped. */
static void *kernel_get_proc_param(void)
{
    if (!rt_process.proc_param) rt_fatal("sceKernelGetProcParam", "the executable has no process parameter segment");
    return rt_process.proc_param;
}
RT_SYSLIB("libkernel", sceKernelGetProcParam, kernel_get_proc_param);

/* The C library registers its heap functions (a table of function pointers, its own or the
   application's replacements) so that system code allocates from the application's heap. Called
   once while the C library starts; the result is not used by the caller. */
static void kernel_rtld_set_application_heap_api(void **heap_api)
{
    if (!heap_api) rt_fatal("_sceKernelRtldSetApplicationHeapAPI", "no table");
    rt_process.heap_api = heap_api;
}
RT_SYSLIB("libkernel", _sceKernelRtldSetApplicationHeapAPI, kernel_rtld_set_application_heap_api);

/* getpagesize, imported by eboot from libScePosix and by libc.elf from libkernel
   (symbols/imports.csv, two rows naming one call): the loader binds an import by library and NID
   (runtime/boot.c), so each library needs its own entry, both pointing here.

   The PS4's system page size is not established by any evidence we can cite. The target's ELF
   headers bound it at best, and only under assumptions about the PS4 loader; the guest itself just
   rejects a non-power-of-two and caches whatever it gets, so it never states the number. The full
   record - what is observed, what is inferred and on which premise, and what remains open - is in
   docs/runtime-syscalls.md, section "getpagesize: observed, inferred, unknown".

   So the call fails loudly instead of returning an invented number or the host's page size. It is
   registered with RT_SYSLIB_UNRESOLVED: the boot gets a named diagnostic if it reaches this, and
   coverage leaves the import unchecked, since a diagnostic is not an implementation. */
static void kernel_getpagesize(void)
{
    rt_fatal("getpagesize", "the PS4 system page size is not established by public evidence");
}
RT_SYSLIB_UNRESOLVED("libScePosix", getpagesize, kernel_getpagesize);
RT_SYSLIB_UNRESOLVED("libkernel", getpagesize, kernel_getpagesize);

/* sceKernelGetDirectMemorySize: the size of the console's direct memory region, the pool a title
   allocates from with sceKernelAllocateDirectMemory (they are neighbours in libkernel). Public
   documentation gives the shape and the region: OpenOrbis declares size_t
   sceKernelGetDirectMemorySize() and describes it as getting "the amount of direct memory available
   for allocation", its libkernel page records the direct memory attribute
   ORBIS_KERNEL_MAIN_DMEM_SIZE 0x180000000u - 6 GiB, the console's 8 GiB of unified memory less the
   2 the kernel keeps - and OpenOrbis's own samples pass this call's result as the search end of
   sceKernelAllocateDirectMemory, the same use eboot makes of it.

   Our own call-site inspection of the executable says the same: all four of its calls
   (0x33ea9f, 0x2081a80, 0x2081ea4, 0x26d3ebf) hand the result straight to
   sceKernelAllocateDirectMemory(0, size, ...) for at most 116 MiB; none compares it with a
   constant, stores it, or branches on it, so what the call has to supply is a bound that covers
   the region the guest allocates from. What a console answers a particular title - the whole
   region or the title's own share of it - is not public (docs/runtime-syscalls.md, section
   "Direct memory size"). */
static uint64_t kernel_get_direct_memory_size(void)
{
    return UINT64_C(0x180000000); /* ORBIS_KERNEL_MAIN_DMEM_SIZE: 6 GiB of the console's 8 GiB */
}
RT_SYSLIB("libkernel", sceKernelGetDirectMemorySize, kernel_get_direct_memory_size);
