/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RUNTIME_CALL_CAPTURE_H
#define RUNTIME_CALL_CAPTURE_H

/* Generic call capture: a thunk between a call and its target that records, by name, the
   registers the call was entered with and the registers it returned with. It needs nothing from
   the function, so it serves every hook (runtime/plugin.c) and every bound import
   (runtime/boot.c) alike.

   Off unless BB_CALL_CAPTURE_DIR is set; then BB_CALL_CAPTURE_ONLY (comma-separated names) limits
   it to those names. A recorded call is one line of
   $BB_CALL_CAPTURE_DIR/<name>/<run>.calls.jsonl (<run>: BB_CAPTURE_RUN, or start time and process
   id); per name and process the first 50 calls, then every 1000th, at most 200.

   A record is not a verification case (docs/VERIFY.md): it holds the argument registers rdi..r9,
   al and ymm0..ymm7, and the return registers rax, rdx, ymm0 and ymm1, without the signature
   that says which of them are meaningful. Stack arguments, the memory the function reads and the
   calls it makes are not recorded. A call that does not return (longjmp, exit) is not recorded.
   Needs an x86-64 host with AVX (vector arguments are saved whole); without it nothing is
   wrapped and a warning says so. */

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
