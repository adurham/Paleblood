// SPDX-License-Identifier: GPL-2.0-or-later
// C++ exceptions through call capture's thunk (runtime/call_capture.h), for test_syslib.c: the
// thunk's unwind information has to lead the unwinder back to the wrapped call's caller, with the
// caller's callee-saved registers, through one thunk, two, and a tail call between two; and an
// unwinder that finds no unwind information for a frame stops the program rather than skip it.
#include "runtime/call_capture.h"

#include <stdexcept>

namespace {

using Thrower = int (*)(int);

Thrower thrower_call, outer_call, tail_call;

__attribute__((noinline)) int throw_unless_negative(int n)
{
    if (n >= 0)
        throw std::runtime_error("thrown through the thunk");
    return -n;
}

__attribute__((noinline)) int call_thrower(int n) { return thrower_call(n) + 1; }


// Each catch must see this frame's own locals, which live in callee-saved registers across the
// calls when optimized.
int catches(Thrower call, int times)
{
    int caught = 0;
    for (int i = 0; i < times; i++) {
        try {
            call(i);
        } catch (const std::runtime_error &) {
            caught++;
        }
    }
    return caught;
}

}  // namespace

// The tail call: a wrapped target that jumps straight into another thunk.
extern "C" {
__attribute__((visibility("hidden"))) void *tail_thrower_target;
int tail_into_thrower(int);
}
__asm__(".pushsection .text\n.globl tail_into_thrower\n.hidden tail_into_thrower\ntail_into_thrower:\n"
        "  jmp *tail_thrower_target(%rip)\n.popsection\n");

// A frame with no unwind information - what a wrapped frame is to a guest unwinder, which reads
// only the guest modules' tables: it calls its first argument with its second.
extern "C" int call_without_unwind_info(Thrower call, int n);
__asm__(".pushsection .text\n.globl call_without_unwind_info\n.hidden call_without_unwind_info\n"
        "call_without_unwind_info:\n"
        "  push %rbp\n  mov %rsp, %rbp\n  mov %rdi, %rax\n  mov %esi, %edi\n  call *%rax\n  pop %rbp\n  ret\n"
        ".popsection\n");

// An exception thrown below that frame must stop in std::terminate: the unwinder cannot step past
// a frame it cannot describe, so the catch here is never reached (returning is the failure).
extern "C" void throw_through_frame_without_unwind_info(void)
{
    try {
        call_without_unwind_info(throw_unless_negative, 1);
    } catch (...) {
    }
}

extern "C" int call_capture_exceptions(void)
{
    thrower_call = (Thrower)rt_call_capture_wrap("thrower", (void *)throw_unless_negative);
    outer_call = (Thrower)rt_call_capture_wrap("rethrow_outer", (void *)call_thrower);
    tail_thrower_target = (void *)thrower_call;
    tail_call = (Thrower)rt_call_capture_wrap("tail_to_thrower", (void *)tail_into_thrower);
    int failures = 0;
    failures += catches(thrower_call, 3) != 3;
    failures += catches(outer_call, 3) != 3;
    failures += catches(tail_call, 3) != 3;
    // and calls that return still return through all three
    failures += thrower_call(-4) != 4 || outer_call(-4) != 5 || tail_call(-4) != 4;
    return failures;
}
