/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RUNTIME_CALL_CAPTURE_H
#define RUNTIME_CALL_CAPTURE_H

/* Generic call capture: a thunk between a call and its target that records, by name, the
   registers the call was entered with and the registers it returned with. It needs nothing from
   the function, so it serves every hook (runtime/plugin.c) and every import the runtime
   implements (runtime/boot.c) alike. An opt-in development facility (docs/capture-architecture.md).

   Two switches. At build time, BB_ENABLE_CALL_CAPTURE (CMake option, on by default) decides
   whether the thunk exists at all: built with it off, the thunk, its stubs and its frames are not
   compiled, rt_call_capture_enabled() is 0 and rt_call_capture_wrap() hands back the target (a
   warning says so once when BB_CALL_CAPTURE_DIR is set). At run time, capture is off unless
   BB_CALL_CAPTURE_DIR is set; then BB_CALL_CAPTURE_ONLY (comma-separated names) limits it to those
   names. Off, every call is exactly what it was without call capture.

   A recorded call is one line of $BB_CALL_CAPTURE_DIR/<name>/<run>.calls.jsonl (<run>:
   BB_CAPTURE_RUN, or start time and process id); per name and process the first 50 calls, then
   every 1000th, at most 200. Every line starts with "record_format" (1, the line's layout) and
   "capture_commit": the commit the capture code was built from, with "-dirty" when runtime/ had
   uncommitted changes, or "unknown" outside a git checkout. Records from a commit later found to
   have a thunk bug are invalidated by that field.

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
   per name, and its record (if sampled) has "returns": null.

   Known gaps:
   - Guest C++ exceptions cannot cross a wrapped frame. A guest unwinder looks for unwind
     information only in the guest modules' tables; the thunk's (and any host replacement's) is in
     the host's. Finding none for the first host frame it reaches (the thunk, or a host function
     the thunk called), its search ends there and the throw calls the guest's std::terminate: a
     loud stop, never a silent skip, since an unwinder has no rule for stepping past a frame it
     cannot describe (test_call_capture_unwind.cpp shows the host unwinder doing exactly that).
     No wrapped path is crossed by guest unwinding today: a hook's replacement is host code that
     the same exception could not cross without the thunk, and pbboot does not wrap imports that
     guest modules provide (runtime/boot.c).
   - A call that returns on a different thread from the one it entered on (a fiber or coroutine
     moved between threads) stops the process with a message naming the call; its record is
     dropped. Each frame carries the entering thread's identity and the return checks it before
     touching any bookkeeping.
   - A target that returns twice (setjmp) must not be wrapped: its second return stops the process
     with a message.
   - Stubs are W^X: written once, then read-only and executable, their data slots in a separate
     writable, non-executable mapping. A host that refuses executable anonymous memory altogether
     (SELinux deny_execmem, PaX MPROTECT) gets no thunks: nothing is wrapped and a warning says so.

   Per thread, up to 2^20 wrapped calls can be in progress (or left and not yet noted); past that,
   and when memory runs out, calls run unrecorded with a warning. Needs an x86-64 host with AVX
   (vector arguments are saved whole); without it, nothing is wrapped and a warning says so. */

#ifndef BB_ENABLE_CALL_CAPTURE
#define BB_ENABLE_CALL_CAPTURE 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Nonzero when BB_CALL_CAPTURE_DIR is set (always 0 in a build without call capture). */
int rt_call_capture_enabled(void);

/* The address to call instead of `target` so that calls to it are recorded under `name`: a new
   thunk, or `target` itself when capture is off, the name is not wanted or the thunk cannot be
   made. `name` is copied. */
void *rt_call_capture_wrap(const char *name, void *target);

#ifdef __cplusplus
}
#endif

#endif
