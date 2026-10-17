/* SPDX-License-Identifier: GPL-2.0-or-later */
/* System call group: kernel, threads and memory. Threads and thread attributes (libkernel).

   Facts used: a thread or attribute variable holds a pointer to an object the library allocates
   (FreeBSD libthr, which the PS4's thread library is); the SCE names return 0 or 0x80020000 plus a
   FreeBSD error number. An attribute records only what has been set or copied into it; reading a
   value the runtime does not know (one the system would supply) is not implemented, so the boot
   stops there by name instead of guessing. */
#include "runtime/process.h"
#include "runtime/syslib.h"

#include <sched.h>
#include <stdint.h>
#include <stdlib.h>

enum { BSD_ENOMEM = 12, BSD_EINVAL = 22 };
#define SCE_KERNEL_ERROR(e) ((int)(0x80020000u + (unsigned)(e)))

/* The set of CPUs a PS4 application may run on. The APU has eight x86-64 cores; two are reserved
   for the kernel and system threads, and six are left to applications (PS4 Developer wiki, "APU";
   Naughty Dog on the console's CPU). The application-visible CPU set is therefore bits 0..5, and a
   shipped PS4 title spells that out: C4JThread.cpp sets "all cores" to 63 on the __ORBIS__ path
   (scePthreadAttrSetaffinity(&attr, 63)). A running thread and a freshly initialised attribute
   both default to that whole set. */
#define RT_CPU_MASK_USER_ALL UINT64_C(0x3f)

typedef struct {
    void *stack;
    uint64_t stack_size;
    uint64_t affinity;    /* the cores this thread may run on */
} guest_thread;

typedef struct {
    int has_stack;
    void *stack;
    uint64_t stack_size;
    uint64_t affinity;    /* the cores the thread it describes may run on */
} guest_attr;

static guest_thread main_thread;
static _Thread_local guest_thread *current;

void rt_thread_register_main(void *stack, uint64_t stack_size)
{
    main_thread.stack = stack;
    main_thread.stack_size = stack_size;
    main_thread.affinity = RT_CPU_MASK_USER_ALL;
    current = &main_thread;
}

static guest_thread *thread_self(void)
{
    if (!current) rt_fatal("scePthreadSelf", "no guest thread registered on this host thread");
    return current;
}

/* Two thread variables name the same thread when they hold the same object pointer. Non-zero when
   equal, zero otherwise, with no error return (FreeBSD pthread_equal(3)). */
static int thread_equal(guest_thread *a, guest_thread *b)
{
    return a == b;
}

/* sched_yield: the calling thread gives up the processor and the call returns 0 (POSIX and FreeBSD
   sched_yield(2), which define no error for it). A guest thread runs on a host thread, so the
   host's yield is the same request. A host failure has no FreeBSD error number we could report, so
   it stops by name instead. */
static int kernel_sched_yield(void)
{
    if (sched_yield() != 0) rt_fatal("sched_yield", "the host did not yield the processor");
    return 0;
}

/* scePthreadYield asks for the same yield, but its PS4-visible signature is not established:
   FreeBSD's pthread_yield(3) returns nothing, Linux's returns an int, and no PS4-specific public
   source or guest call site we can cite says which one the guest sees. Returning would leave the
   value a caller might read undefined, so the call stops by name. It is registered with
   RT_SYSLIB_UNRESOLVED: a boot that reaches it names the call, and coverage leaves the import
   unchecked, since a diagnostic is not an implementation. */
static void thread_yield(void)
{
    rt_fatal("scePthreadYield", "the PS4 return behaviour is not established by public evidence");
}

static int attr_init(guest_attr **attr)
{
    if (!attr) return SCE_KERNEL_ERROR(BSD_EINVAL);
    guest_attr *a = calloc(1, sizeof *a);
    if (!a) return SCE_KERNEL_ERROR(BSD_ENOMEM);
    a->affinity = RT_CPU_MASK_USER_ALL;
    *attr = a;
    return 0;
}

static int attr_destroy(guest_attr **attr)
{
    if (!attr || !*attr) return SCE_KERNEL_ERROR(BSD_EINVAL);
    free(*attr);
    *attr = NULL;
    return 0;
}

/* Copies a running thread's attributes into an initialised attribute object. */
static int attr_get(guest_thread *thread, guest_attr **attr)
{
    if (!thread || !attr || !*attr) return SCE_KERNEL_ERROR(BSD_EINVAL);
    (*attr)->has_stack = 1;
    (*attr)->stack = thread->stack;
    (*attr)->stack_size = thread->stack_size;
    (*attr)->affinity = thread->affinity;
    return 0;
}

/* scePthreadAttrGetaffinity(attr, mask): writes the CPUs the attribute admits to the uint64_t the
   caller points at. The PS4-visible declaration - an attribute pointer, a mask pointer, an int32_t
   result - is the public OpenOrbis libkernel.h one. The attribute the game reads comes from
   scePthreadAttrGet(scePthreadSelf()) as libc.elf starts, or from scePthreadAttrInit, so the whole
   application CPU set above comes back. 0 on success; a null pointer is EINVAL, as the neighbouring
   attribute entries report it, and no public SCE source gives another value. */
static int attr_getaffinity(guest_attr **attr, uint64_t *mask)
{
    if (!attr || !*attr || !mask) return SCE_KERNEL_ERROR(BSD_EINVAL);
    *mask = (*attr)->affinity;
    return 0;
}

RT_SYSLIB("libkernel", scePthreadSelf, thread_self);
RT_SYSLIB("libkernel", scePthreadEqual, thread_equal);
RT_SYSLIB("libkernel", sched_yield, kernel_sched_yield);
RT_SYSLIB_UNRESOLVED("libkernel", scePthreadYield, thread_yield);
RT_SYSLIB("libkernel", scePthreadAttrInit, attr_init);
RT_SYSLIB("libkernel", scePthreadAttrDestroy, attr_destroy);
RT_SYSLIB("libkernel", scePthreadAttrGet, attr_get);
RT_SYSLIB("libkernel", scePthreadAttrGetaffinity, attr_getaffinity);
