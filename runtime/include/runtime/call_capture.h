/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RUNTIME_CALL_CAPTURE_H
#define RUNTIME_CALL_CAPTURE_H

/* Generic call capture: a thunk between a call and its target that records, by name, the
   registers the call was entered with and the registers it returned with. It needs nothing from
   the function, so it serves every hook (runtime/plugin.c) and every import the runtime
   implements (runtime/boot.c) alike.

   Off unless BB_CALL_CAPTURE_DIR is set; then BB_CALL_CAPTURE_ONLY (comma-separated names) limits
   it to those names. A recorded call is one line of
   $BB_CALL_CAPTURE_DIR/<name>/<run>.calls.jsonl (<run>: BB_CAPTURE_RUN, or start time and process
   id); per name and process the first 50 calls, then every 1000th, at most 200.

   BB_CALL_CAPTURE_ONLY filters only these records; BB_CAPTURE_ONLY is the case writer's own
   filter (runtime/capture.c). The two share only BB_CAPTURE_RUN, the run id.

   A record is not a verification case (docs/VERIFY.md): it holds the argument registers rdi..r9,
   al and ymm0..ymm7, and the return registers rax, rdx, ymm0, ymm1 and, when they hold a value
   (a long double result), st0 and st1, without the signature that says which of them are
   meaningful. Not recorded: stack arguments, the memory the function reads, the calls it makes,
   zmm upper halves and the x87 or SSE control state.

   Calls that leave without returning - longjmp, a C++ exception thrown by host code (the thunk
   has unwind information), the thread's exit - are safe: the call is noted when the next wrapped
   call on that thread is entered at the same stack slot, or when the thread exits, with a warning
   per name, and its record (if sampled) has "returns": null. A guest unwinder cannot read the
   host's unwind information, so a guest exception must not cross a thunk (pbboot does not wrap
   imports that guest modules provide, runtime/boot.c). A target that returns twice (setjmp) must
   not be wrapped: its second return stops the process with a message.

   Per thread, up to 2^20 wrapped calls can be in progress (or left and not yet noted); past that,
   and when memory runs out, calls run unrecorded with a warning. Needs an x86-64 host with AVX
   (vector arguments are saved whole) that allows writable and executable memory (the thunks'
   stubs); without either, nothing is wrapped and a warning says so. */

#ifdef __cplusplus
extern "C" {
#endif

/* Nonzero when BB_CALL_CAPTURE_DIR is set. */
int rt_call_capture_enabled(void);

/* The address to call instead of `target` so that calls to it are recorded under `name`: a new
   thunk, or `target` itself when capture is off, the name is not wanted or the thunk cannot be
   made. `name` is copied. */
void *rt_call_capture_wrap(const char *name, void *target);

#ifdef __cplusplus
}
#endif

#endif
