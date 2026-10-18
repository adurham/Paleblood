# Independent verification: call capture hardening

- **Target:** `adurham/Paleblood` branch `runtime/generic-input-capture-hardening`, commit
  `bad3066185907c415ced12705598521ccbdf7f56` "runtime: make call capture safe across non-local
  exits". Parent `4c4c92c130` (the unit verified in `VERIFY-call-capture.md` on
  `claude/verify-call-capture-r7k2qx`).
- **Reviewer:** an independent Claude Code session. It did not write the change and made no
  changes to the branch under test. This report is the only file this branch adds.
- **Date:** 2026-10-18.

## Verdict summary

| Item | Verdict |
| --- | --- |
| CI battery, gcc (configure, build, ctest syslib, ctest rest, harness, differential, full Python) | **PASS** |
| CI battery, clang (same steps) | **PASS** |
| Policy steps (no game data, validate, agnostic, readable, progress, hooks) | **PASS** |
| Probe 1: nested longjmp into an outer wrapped call | **PASS** (parent: SIGSEGV) |
| Probe 2: 1100 longjmps out of wrapped calls | **PASS** (parent: abort at 1024) |
| Probe 3: recursion 2000 deep (and 200000 deep) | **PASS** (parent: abort) |
| Probe 4: module exports are not wrapped | **PASS**, by code reading and by a pbboot `--module` probe |
| Probe 5: C++ exception through a wrapped frame | **PASS** (parent: `std::terminate`) |
| Probe 6: 8 threads x 200k calls | **PASS** |
| Probe 7: x87 `long double` return (D4) | **PASS** |
| Probe 8: TLS-destructor ordering (D5) | **PASS**: 0 invalid accesses under valgrind or ASan (parent: heap-use-after-free) |
| Probe 9: record write fails mid-run | **PASS** |
| Probe 10: default-off is identical to the parent | **PASS** |
| New tests fail on the parent and pass on the target | **PASS**, but the D5 test also passes on the parent (N3) |
| Clean room (agnostic, nothing under `game/`) | **PASS** |
| STATUS.md claims match the code | **PASS**, with two inaccuracies (N3, N4) |

**Overall: PASS.** Every probe that failed before (D1, D2, D4, D5) now passes on both compilers.
CI and the policy steps are green. The off path is identical to the parent's, including syscalls.
I found no defect on the CI path. I found two new LOW defects, outside CI and not covered by the
docs:

- **N1:** a test-only section bug. It crashes `test_syslib` under gcc `-O0 -fsanitize=address`.
- **N2:** a wrapped call that returns on a different thread from the one it entered on (a coroutine
  or fiber moved between threads). It corrupts the per-thread bookkeeping on the returning thread.

D3 (maintainer sign-off on the trampoline approach) is still open, as STATUS.md says.

## Environment

- Ubuntu 24.04.5 LTS, gcc 13.3.0, Ubuntu clang 18.1.3, cmake 3.28.3.
- Python: every step ran with `python3.12`, the CI runner's `python3`. The container's default
  `python3` is 3.13 and has no capstone. `pip install capstone==5.0.9` reports version 5.0.7, the
  same as in the previous report.
- Clone: the session clone was shallow. I ran `git fetch --unshallow origin` before anything else,
  so `tools/test_hooks.sh` ran on full history.
- Worktrees:
  - `../pb-t` at `bad3066` (target)
  - `../pb-p` at `4c4c92c` (parent)
  - `../pb-x`: the parent with the target's `test/` and `CMakeLists.txt` checked out over it, to
    show that the new tests fail on the parent
- **One environment trap, not a defect.** A plain `cmake` configure picks up `python3` 3.13
  through `find_package(Python3)`. ctest's `tools` test then fails 20 harness tests with
  "libcapstone is unavailable". That happens on target and parent alike. I passed
  `-DPython3_EXECUTABLE=/usr/bin/python3.12` to match CI, and every run below used it.

## CI battery

Script, per tree: `BB_CALL_CAPTURE_DIR`, `BB_CALL_CAPTURE_ONLY`, `BB_CAPTURE_ONLY` and
`BB_CAPTURE_RUN` unset, then for `cc` in `gcc` and `clang`:

```
cmake -S . -B build-$cc -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=$cc -DCMAKE_CXX_COMPILER=$cxx -DPython3_EXECUTABLE=/usr/bin/python3.12
cmake --build build-$cc -j8
ctest --test-dir build-$cc --output-on-failure --no-tests=error -R '^syslib$'
ctest --test-dir build-$cc --output-on-failure --no-tests=error -E '^syslib$'
CC=$cc BB_LOADER=build-$cc/runtime/libpbloader.so python3.12 -m unittest discover -s test -p 'test_harness.py'
CC=$cc BB_LOADER=build-$cc/runtime/libpbloader.so python3.12 -m unittest discover -s test -p 'test_differ*.py'
ln -s build-$cc build    # so test_tools.Boot runs, as it does in CI
CC=$cc BB_LOADER=build-$cc/runtime/libpbloader.so python3.12 -m unittest discover -s test -p 'test_*.py' -v
```

| Step | Parent `4c4c92c` | Target `bad3066` |
| --- | --- | --- |
| gcc configure / build | rc 0 / rc 0, 0 warnings | rc 0 / rc 0, 0 warnings |
| gcc ctest `^syslib$` | 1/1 passed | 1/1 passed |
| gcc ctest `-E ^syslib$` | 12/12 passed | 12/12 passed |
| gcc harness | Ran 35, OK | Ran 35, OK |
| gcc differential | Ran 18, OK | Ran 18, OK |
| gcc full Python (with `build/`) | Ran 187, OK | Ran 188, OK |
| clang configure / build | rc 0 / rc 0, 0 warnings | rc 0 / rc 0, 0 warnings |
| clang ctest `^syslib$` | 1/1 passed | 1/1 passed |
| clang ctest `-E ^syslib$` | 12/12 passed | 12/12 passed |
| clang harness | Ran 35, OK | Ran 35, OK |
| clang differential | Ran 18, OK | Ran 18, OK |
| clang full Python (with `build/`) | Ran 187, OK | Ran 188, OK |

Diffing the per-test outcomes of the full Python suite, parent against target, for each compiler
gives exactly one addition: `test_tools.Boot.test_call_capture_leaves_module_exports_unwrapped ...
ok`. No test changed outcome, and no test was skipped. The one hit for "skipped" is the test
named `test_submodule_link_is_skipped`.

Policy steps, run once on the target tree (full history):

| Step | Result |
| --- | --- |
| `python3.12 tools/check_no_game_data.py --tracked` | rc 0 **PASS** |
| `python3.12 tools/validate_functions.py` | rc 0 **PASS** |
| `python3.12 tools/check_agnostic.py` | rc 0 **PASS** |
| `python3.12 tools/check_readable.py --base 4c4c92c130` | rc 0, `readable: ok (0 grandfathered file(s) left in tools/readable_allowlist.txt)` **PASS** |
| `python3.12 tools/progress.py --check` | rc 0 **PASS** |
| `tools/test_hooks.sh` (full clone, `is-shallow-repository` = false) | rc 0, 19 `ok` lines **PASS** |

Not run: the `session-report` job, which needs a PR body.

## New tests fail on the parent, pass on the target

`../pb-x` is the parent's runtime with the target's tests. Results:

| Test (in `test_syslib`, each in a forked child) | Parent gcc | Parent clang | Target (both) |
| --- | --- | --- | --- |
| `capture_tail_calls` | pass | pass | pass |
| `capture_longjmp_to_unwrapped_caller` (1100 longjmps) | **fail** | **fail** | pass |
| `capture_longjmp_into_outer_call` | **fail** (child crashed) | **fail** (`outer(3) == 5` x3) | pass |
| `capture_thread_exit_inside_a_call` | **fail** | **fail** | pass |
| `capture_call_from_late_destructor` (D5) | pass (!) | pass (!) | pass |
| `capture_returns_twice_is_fatal` | pass (the parent also aborts) | **fail** | pass |
| `capture_recursion_past_old_limit` (5000 deep) | **fail** | **fail** | pass |
| `capture_threads` | pass | pass | pass |
| `capture_x87_results` | **fail** (no st0/st1) | **fail** | pass |
| `capture_unwritable_record` | pass | pass | pass |
| `capture_exceptions` (`test_call_capture_unwind.cpp`) | **fail** (`terminate called ... thrown through the thunk`) | **fail** | pass |
| `test_tools.Boot.test_call_capture_leaves_module_exports_unwrapped` | **FAIL** (`True is not false`: a capture dir was created) | **FAIL** | ok |
| `test_harness ... records_are_not_cases` (now with a `"returns": null` line) | ok | ok | ok |

On the parent, `test_syslib` exits 1 on both compilers. Each D1, D2 and D4 regression test fails
on the parent and passes on the target, so those tests discriminate. The D5 test does not (see
N3). The threads, unwritable-record and tail-call tests are behaviour tests that already passed
on the parent, as expected.

## Probes

Each probe is linked against `runtime/call_capture.c` of the tree under test. I built every probe
with gcc and with clang (`-O2 -g`; C++ with g++ and clang++) and ran it against both the target
and the parent. The sources are in the appendix.

| # | Probe | Target gcc | Target clang | Parent (both) |
| --- | --- | --- | --- | --- |
| 1 | `p1_lj`: inner wrapped call `longjmp`s to a `setjmp` inside the outer wrapped call, then the outer returns; repeated 100 times | rc 0: sum 500 (expected 500); outer 50 records, all with results; inner 50 records, all `"returns": null`; one warning `inner: a call was left without returning ...` | identical | rc 139 **SIGSEGV** |
| 2 | `p2_j1100`: 1100 `longjmp`s out of a wrapped call to an unwrapped `setjmp` | rc 0: landed 1100/1100, 51 records, all `"returns": null`, one warning | identical | rc 134 `call capture: jumper: calls nested deeper than 1024` |
| 3 | `p3_rec`: recursion through a wrapped name, 2000 deep | rc 0: `rec(2000) = 2000`, 52 records (first 50, calls 1000 and 2000), all with results | identical | rc 134 `rec: calls nested deeper than 1024` |
| 3b | the same at 200000 deep (`ulimit -s unlimited`) | rc 0: 200 records, 0.10 s | rc 0, 0.14 s | n/a |
| 4a | `p4_sj`: host `_setjmp` wrapped, then `longjmp` to it | rc 134: `call capture: a wrapped call returned after its frame was closed: the target returns twice (as setjmp does) and cannot be wrapped, ...`. This is the documented stop, never a wrong return | identical | rc 134 `a call returned that was never entered` |
| 4b | `p4_module.py`: pbboot with a guest **module** exporting a setjmp-like `sj` and a longjmp-like `lj` (libfoo), called by the eboot; capture on | `returned`, imports bundled/called 2/2, no capture directory | identical | **`fault`** (the second return went through the thunk and landed on the `hlt`), capture directory created |
| 5 | `p5_ex`: `throw` from a wrapped host function, caught by its caller, 1000 times; then a normal wrapped call | rc 0: caught 1000/1000, `ok(7)=21`, one warning per name | identical | rc 134 `terminate called after throwing an instance of 'std::runtime_error'` |
| 6 | `p6_mt`: 8 threads x 200000 calls to a wrapped 8-argument function; records parsed with `json.loads` | rc 0: 0 wrong results; 200 records, one key set `{args, call, from, name, record, returns, run_id}`, record numbers exactly 0..199, 200 unique call numbers, 0 `rax` mismatches against `rdi+rsi+21`; 0.18 s | identical; 0.24 s | same correctness; 0.09-0.10 s |
| 7 | `p7_x87`: `long double` return, then `long double _Complex` return, then 100 more calls; x87 tag word checked after each | rc 0: `twice(1.5)=3`, tags `0xffff` (empty), pair `(1,2)`, tags `0xffff`, sum 9900; records hold `"st0": "00000000000000c00040"` (3.0L), and for the pair `"st0": "0000000000000080ff3f"` (1.0L) and `"st1": "00000000000000800040"` (2.0L) | identical | values forwarded correctly, but no `st0`/`st1` in the records |
| 8 | `p8_tls`: 50 threads; each makes a wrapped call, then two key destructors (one created after capture's key, one set from inside the first) make wrapped calls | rc 0, `late1=10 late2=42`. **valgrind: 0 invalid reads or writes** (gcc and clang builds). **gcc ASan (`-O1`): clean**. clang ASan runtime is not installed in this image | identical | gcc ASan: **heap-use-after-free** in `rt_call_capture_enter` (`call_capture.c:270`). valgrind: 12416 errors (gcc) / 11262 (clang), including invalid reads and writes |
| 9 | `p9_ro`: 10 recorded calls, then `chmod 0400` the record file and `0500` its directory, then 50 more calls with `errno` preset; run as uid 65534 via `setpriv` (as root, the permission bits would be ignored) | rc 0: sum 1830 (expected 1830), `errno` preserved on every call, file still holds 10 records, exactly one stderr line `call capture: cannot write a record: <path>` (as `write_record`/`warn_once` document) | identical | identical |
| 10 | default off, see below | identical to parent | identical to parent | — |

The only valgrind complaints on the target are "use of uninitialised value" in `hex()` while it
formats the **returned** `ymm0`/`ymm1`. The thunk records whatever is in those registers, defined
or not, and the parent does exactly the same. This is the documented "without the signature that
says which are meaningful", not a memory error.

### Item 4: how imports and exports are chosen

`runtime/boot.c` `resolve()` binds each import first to a module export (`provider = MODULE`),
then to an `RT_SYSLIB` entry (`RUNTIME`, or `RUNTIME_UNRESOLVED` for fail-loud stubs).
`import_target()` (line 321) returns `o->bound[i]` unwrapped when capture is off **or**
`o->provider[i] == MODULE`. Only `boot.c:331` and `plugin.c:54` call `rt_call_capture_wrap`, so
nothing else is wrapped. The claim is true. Probe 4b shows it end to end through pbboot with
`--module`, and it discriminates: the parent faults on the same ELF pair. I also checked that no
runtime `RT_SYSLIB` entry is setjmp-like, unwinding or non-returning-by-jump. The 36 entries are
net, pthread-mutex and attribute, and kernel queries. The `RT_SYSLIB_UNRESOLVED` stubs call
`rt_fatal`, which calls `abort()`. They are wrapped, which is harmless; see N4.

### Item 10: default-off

- **Code.** `rt_call_capture_wrap` returns `target` before AVX detection, `pthread_once`, `mmap`
  or any file access when `BB_CALL_CAPTURE_DIR` is unset or empty. `import_target` returns
  `bound[i]` first. `plugin.c` is unchanged by this commit.
- **Tests.** Per-test outcomes are identical (see the CI section). The only difference is the
  added test.
- **pbboot strace.** I ran the synthetic one-import eboot (`sceKernelGetDirectMemorySize`, with
  `--nids`) and the module ELF pair from probe 4b. Both ran on the target's and the parent's
  `pbboot`, gcc and clang builds, under
  `env -u BB_CALL_CAPTURE_DIR -u BB_CALL_CAPTURE_ONLY strace -f -qq -e trace=mkdir,mkdirat,openat,open,creat`.
  All 8 runs: 0 mkdir calls, 0 opens of `*.calls.jsonl`, 5 opens each. After normalizing pids
  and addresses, the strace logs are **IDENTICAL** between parent and target, and so are the
  stdout, stderr, rc and `--status` JSON.

## Clean room

- `git diff --name-only 4c4c92c bad3066 -- game/` is empty, and `grep -rniE
  'call_capture|calls\.jsonl|recorder' game/` finds nothing.
- `check_agnostic`, `check_no_game_data` and `check_readable` pass.
- `runtime/call_capture.c` reads only `BB_CALL_CAPTURE_DIR`, `BB_CALL_CAPTURE_ONLY` and
  `BB_CAPTURE_RUN`. It names no module, library, symbol or address.
- `docs/capture-architecture.md` is unchanged, so D3 is left to the maintainer, as STATUS.md
  says.

## STATUS.md claims against the code

| Claim | Verdict |
| --- | --- |
| D1 fixed: longjmp, host C++ exceptions, thread exit and tail calls return to the right place | **TRUE** (probes 1, 2, 5; tests for tail calls and thread exit; a `pthread_exit` forced unwind through the thunk works) |
| D2 fixed: no fixed 1024 limit; 2^20 per thread, then unrecorded with a warning | **TRUE** (`MAX_FRAMES = 1 << 20`, `open_frame`; probe 3b at 200000 deep) |
| D4 fixed: x87 state saved around the record write; st0/st1 recorded when they hold a value | **TRUE** (`fnsave`/`frstor` around `rt_call_capture_leave`; probe 7) |
| D5 fixed: the destructor clears its pointer | **TRUE** (`frames = NULL` in `thread_exit`; probe 8 clean under valgrind and ASan) |
| D7 fixed and tested: exits, depth, threads and write failures | **TRUE for those four**. D7 also listed the no-AVX fallback, and STATUS now adds the RWX-mapping failure; neither fallback has a test. The D5 test does not detect the D5 bug (N3) |
| D6 documented (RWX stubs) | **TRUE** (header, STATUS.md, comment at the `mmap`) |
| D8 documented (the two `_ONLY` filters) | **TRUE** (header, STATUS.md, runtime/README.md) |
| D3 open | **TRUE** (the page is unchanged; "needs sign-off") |
| "pbboot sends the trampoline of every import the runtime implements" through a thunk; module exports are not wrapped | **Mostly true**: `RUNTIME_UNRESOLVED` stubs are wrapped too (N4). Module exports: TRUE |
| "a call left without returning is noted ... when the next wrapped call enters at its stack slot or the thread exits" | **TRUE**. Caveat (also in STATUS): frames still open on the main thread at process exit are never noted |
| Remaining risks list (guest unwinder, returns-twice, rbx/r12 contract, signals, AVX-512, late noting) | **TRUE**, but incomplete: it leaves out N2 (a call that returns on another thread) |

## Defects, ranked

No defect was found on the default-off path or in the CI configuration.

1. **N2 (LOW, capture on only, undocumented): a wrapped call that returns on a different thread
   corrupts the returning thread's frames.**
   - **How it happens.** `rt_call_capture_leave` uses the *current* thread's `frames`. If a
     wrapped call is entered on thread A and returns on thread B (a `ucontext` or fiber
     coroutine resumed elsewhere), `close_frame(B, f)` does not find `f` in B's table. It still
     decrements `B->open` (a `size_t`, so it wraps to `SIZE_MAX`), and it pushes `f` onto B's
     free list although `f` lives in one of A's chunks and A's table still points to it.
   - **Observed** (`p13_xthread`, both compilers). The call returns the right value (42). B then
     prints the misleading `too many wrapped calls in progress on one thread; further calls are
     not recorded`, and B records nothing more.
   - **Latent.** If a later call on A enters at `f`'s slot, `close_left` frees `f` onto A's free
     list as well. One frame then sits on two free lists, which can corrupt both threads. The
     parent aborted outright in this case (`a call returned that was never entered`).
   - **Reach.** None of today's runtime imports switch context. A hook running on a fiber that
     migrates between threads would reach it; PS4 fibers may run on any thread.
   - **Fix.** Store the owning `thread_frames *` in each frame and refuse (stop with a message,
     or leave unrecorded) when it differs from `frames`. At least add it to STATUS.md's risk list.
2. **N1 (LOW, test only): `test/test_syslib.c` switches to `.text` in a top-level `asm` and never
   switches back.**
   - **Where.** `__asm__(".text\n.globl tail_call ... jmp *tail_target(%rip)\n")`.
   - **Effect.** gcc does not know the section changed. Under gcc `-O0 -fsanitize=address` it
     places the next variable, `static char *capture_dir`, in `.text`, and `test_syslib` dies
     with **SEGV on write** at `capture_dir = mkdtemp(dir)` (`test_syslib.c:441`).
   - **Configurations.** Checked with `objdump -t`: gcc `-O0`, gcc `-O2`, gcc `-O2` ASan and clang
     (all modes) are fine. The parent's `test_syslib` passes under the same ASan Debug build.
   - **Fix, verified locally (not pushed).** Wrapping that asm in `.pushsection .text` …
     `.popsection` makes the full `test_syslib`, every forked case included, pass under gcc
     Debug with ASan and UBSan: rc 0, no sanitizer reports. That run is also the strongest
     evidence I have that the new frame-table code has no memory errors.
   - **Same pattern, not observed misplacing anything.** `runtime/call_capture.c`'s thunk asm and
     the asm in `test_call_capture_unwind.cpp` also begin with a bare `.text`. Neither misplaced
     a variable under gcc `-O0`/`-O1` ASan or `-fno-toplevel-reorder`. `.pushsection`/`.popsection`
     would make that safe by construction.
3. **N3 (INFO, test gaps).**
   - `capture_call_from_late_destructor` (D5) passes on the parent too. The use-after-free there
     is silent without a sanitizer, so the test does not discriminate. Only valgrind or ASan
     (probe 8) show the fix.
   - The no-AVX fallback and the RWX `mmap` failure fallback remain untested. STATUS.md's "D7 ...
     fixed and tested" is true only for exits, depth, threads and write failures.
4. **N4 (INFO, docs): `RUNTIME_UNRESOLVED` stubs are wrapped.** STATUS.md, `boot.c` and
   runtime/README.md say "every import the runtime implements". `import_target` excludes only
   `MODULE`, so the fail-loud `RT_SYSLIB_UNRESOLVED` stubs (`getpagesize`, `scePthreadYield`,
   `sceNetCtlGetInfo`, ...) are wrapped as well. That is harmless: they `abort()` through
   `rt_fatal`. A record line appears only for a sampled call, and the process aborts inside the
   target, so in practice nothing is written.
5. **N5 (INFO, capture on only): hot-path cost about doubled.** 8 x 200k calls took 0.18 s (gcc)
   / 0.24 s (clang), against 0.09 / 0.10 s for the parent's shadow stack. The extra cost is the
   hash-table lookup per call. Worst-case memory is bounded by `MAX_FRAMES`: about 100 MB of
   frames plus a 16 MB table per thread whose left calls never reuse a slot.
6. **D3 (MEDIUM, process): still open by design.** The assembly trampoline still contradicts
   `docs/capture-architecture.md` until the maintainer signs it off. STATUS.md states this
   accurately.

Also checked and fine:

- pbboot's own `SIGALRM`/fault `siglongjmp` can leave frames open. With capture on, 20 runs of an
  eboot that loops on a wrapped runtime import with `--timeout 1` all reported `timeout`
  correctly: no hang, 200 records. After that the run ends, so the open frames are never used.
- `backtrace()` and `_Unwind_Backtrace` from inside a doubly wrapped call reach `main` (depth 9).
  On the parent they stop after 2 frames (`p12_bt`).

## Appendix: probe sources

All probes include this header (`common.h`):

```c
#define _GNU_SOURCE
#include "runtime/call_capture.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
static char capdir[64];
static void cap_on(const char *tag) { snprintf(capdir, sizeof capdir, "/tmp/pbv-%s-XXXXXX", tag); if(!mkdtemp(capdir)) abort(); setenv("BB_CALL_CAPTURE_DIR", capdir, 1); setenv("BB_CAPTURE_RUN","probe",1); }
static int count_lines(const char *name, const char *needle) { char p[256]; snprintf(p, sizeof p, "%s/%s/probe.calls.jsonl", capdir, name); FILE *f = fopen(p, "r"); if (!f) return -1; char l[8192]; int n = 0; while (fgets(l, sizeof l, f)) if (!needle || strstr(l, needle)) n++; fclose(f); return n; }
```

Build: `$cc -O2 -c -I<tree>/runtime/include <tree>/runtime/call_capture.c -o cc.o`, then
`$cc -O2 -g -I<tree>/runtime/include -I. pN.c cc.o -lpthread`.

`p1_lj.c`:

```c
#include "common.h"
static jmp_buf jb; static void (*inner_w)(void);
__attribute__((noinline)) void inner(void) { longjmp(jb, 1); }
__attribute__((noinline)) long outer(void) { if (!setjmp(jb)) inner_w(); return 5; }
int main(void) { cap_on("lj");
  inner_w = rt_call_capture_wrap("inner", (void *)inner);
  long (*o)(void) = rt_call_capture_wrap("outer", (void *)outer);
  long r = 0; for (int i = 0; i < 100; i++) r += o();
  printf("outer x100 sum %ld (expected 500); outer records %d ok-returns %d; inner records %d (null %d)\n", r, count_lines("outer",0), count_lines("outer","\"returns\": {"), count_lines("inner",0), count_lines("inner","\"returns\": null"));
  return r != 500; }
```

`p2_j1100.c`:

```c
#include "common.h"
static jmp_buf jb; static void (*j_w)(void);
__attribute__((noinline)) void jumper(void) { longjmp(jb, 1); }
int main(void) { cap_on("j1100"); j_w = rt_call_capture_wrap("jumper", (void *)jumper);
  volatile int landed = 0;
  for (volatile int i = 0; i < 1100; i++) { if (setjmp(jb)) { landed++; continue; } j_w(); }
  printf("landed %d/1100; records %d (null %d)\n", landed, count_lines("jumper",0), count_lines("jumper","\"returns\": null"));
  return landed != 1100; }
```

`p3_rec.c` (argument: the depth, default 2000):

```c
#include "common.h"
static long (*rw)(long);
__attribute__((noinline)) long rec(long n) { return n ? rw(n - 1) + 1 : 0; }
int main(int argc, char **argv) { long depth = argc > 1 ? atol(argv[1]) : 2000; cap_on("rec"); rw = rt_call_capture_wrap("rec", (void *)rec);
  long r = rw(depth); printf("rec(%ld) = %ld; records %d returns %d\n", depth, r, count_lines("rec",0), count_lines("rec","\"returns\": {")); return r != depth; }
```

`p4_sj.c`:

```c
#include "common.h"
static jmp_buf jb;
int main(void) { cap_on("sj");
  int (*sj)(jmp_buf) = rt_call_capture_wrap("libc.setjmp", (void *)_setjmp);
  volatile int n = 0; int r = sj(jb); printf("setjmp returned %d\n", r); fflush(stdout);
  if (!n++) longjmp(jb, 7); puts("second return reached the caller"); return 0; }
```

`p4_module.py` (run: `python3.12 p4_module.py <tree> <tree>/build-$cc/runtime/pbboot <outdir>`).
The guest module's `sj` saves its return address and `rsp`. Its `lj` restores them and returns 1,
so `sj` returns a second time. The eboot runs `call [sj]; test eax,eax; jnz ok; call [lj]; hlt;
ok: ret`, so a wrong return lands on `hlt` and faults.

```python
import json, os, struct, subprocess, sys
tree, pbboot, d = sys.argv[1:4]
sys.path[:0] = [os.path.join(tree, 'tools'), os.path.join(tree, 'test')]
import harness, ps4elf
sj, lj = harness.nid('sj') + '#B#A', harness.nid('lj') + '#B#A'
def rel(at, insn_len, target): return struct.pack('<i', target - (at + insn_len))
c = bytearray(0x1000); i = 0
def emit(b):
    global i; c[i:i + len(b)] = b; i += len(b)
emit(b'\xff\x15' + rel(i, 6, 0x1000)); emit(b'\x85\xc0'); emit(b'\x75\x07')
emit(b'\xff\x15' + rel(i, 6, 0x1008)); emit(b'\xf4'); emit(b'\xc3')
exe = ps4elf.build(segments=[(0, bytes(c), 0x1000, 5), (0x1000, bytes(16), 0x1000, 6)],
                   symbols=[(sj, ps4elf.STT_FUNC, None), (lj, ps4elf.STT_FUNC, None)], relocations=[],
                   jump_slots=[(0x1000, ps4elf.R_X86_64_JUMP_SLOT, 1, 0), (0x1008, ps4elf.R_X86_64_JUMP_SLOT, 2, 0)],
                   libraries=[(ps4elf.DT_SCE_IMPORT_LIB, 'libfoo', 1)])
m = bytearray(0x1000); m[0] = 0xc3; X, Y = 0x800, 0x808
a = 0x40; seq = b'\x48\x8b\x04\x24'; seq += b'\x48\x89\x05' + rel(a + len(seq), 7, X)
seq += b'\x48\x89\x25' + rel(a + len(seq), 7, Y); seq += b'\x31\xc0\xc3'; m[a:a + len(seq)] = seq
a = 0x80; seq = b'\x48\x8b\x25' + rel(a, 7, Y); seq += b'\x48\x8b\x05' + rel(a + len(seq), 7, X)
seq += b'\x48\x89\x04\x24\xb8\x01\x00\x00\x00\xc3'; m[a:a + len(seq)] = seq
mod = ps4elf.build(segments=[(0, bytes(m), 0x1000, 7)], symbols=[(sj, ps4elf.STT_FUNC, 0x40), (lj, ps4elf.STT_FUNC, 0x80)],
                   relocations=[], libraries=[(ps4elf.DT_SCE_EXPORT_LIB, 'libfoo', 1)])
# ... write e.elf / m.elf, run pbboot e.elf --module m.elf --status s.json --timeout 5 with capture off and on,
# print rc, (outcome, imports_bundled, imports_called) and whether the capture directory exists.
```

`p5_ex.cpp`:

```cpp
#include "runtime/call_capture.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <unistd.h>
__attribute__((noinline)) void thrower(int) { throw std::runtime_error("x"); }
__attribute__((noinline)) int ok(int x) { return x * 3; }
int main() { char dir[] = "/tmp/pbv-ex-XXXXXX"; mkdtemp(dir); setenv("BB_CALL_CAPTURE_DIR", dir, 1);
  auto t = (void (*)(int))rt_call_capture_wrap("thrower", (void *)thrower);
  auto o = (int (*)(int))rt_call_capture_wrap("ok", (void *)ok);
  int caught = 0; for (int i = 0; i < 1000; i++) { try { t(i); } catch (const std::exception &) { caught++; } }
  printf("caught %d/1000; ok(7)=%d\n", caught, o(7)); return !(caught == 1000 && o(7) == 21); }
```

`p6_mt.c`. Its records were checked with a `json.loads` script: key sets, record numbers 0..199,
unique call numbers, and `rax == rdi + rsi + 21`.

```c
#include "common.h"
#include <pthread.h>
static long (*w)(long,long,long,long,long,long,long,long);
__attribute__((noinline)) long f8(long a,long b,long c,long d,long e,long f,long g,long h){return a+b+c+d+e+f+g+h;}
static void *run(void *arg){ long base=(long)arg<<32; for(long i=0;i<200000;i++) if(w(base,i,1,2,3,4,5,6)!=base+i+21) return (void*)1; return NULL; }
int main(void){ cap_on("mt"); w = rt_call_capture_wrap("f8",(void*)f8); pthread_t t[8]; for(long i=0;i<8;i++) pthread_create(&t[i],0,run,(void*)i);
  int bad=0; for(int i=0;i<8;i++){void*r; pthread_join(t[i],&r); bad+= r!=NULL;} printf("threads with wrong results: %d\n", bad); printf("DIR %s\n", capdir); return bad; }
```

`p7_x87.c`:

```c
#include "common.h"
__attribute__((noinline)) long double twice(long double x){ return x*2; }
__attribute__((noinline)) long double _Complex pair(long double a, long double b){ return a + b*1.0iL; }
static unsigned tags(void){ unsigned char e[28]; __asm__ volatile("fnstenv %0\n fldenv %0":"=m"(e)); return e[8]|e[9]<<8; }
int main(void){ cap_on("x87"); long double (*t)(long double)=rt_call_capture_wrap("twice",(void*)twice);
  long double _Complex (*p)(long double,long double)=rt_call_capture_wrap("pair",(void*)pair);
  long double r = t(1.5L); unsigned tg1=tags(); long double _Complex c = p(1.0L, 2.0L); unsigned tg2=tags();
  long double s=0; for(int i=0;i<100;i++) s+=t(i);
  printf("twice(1.5)=%Lg tags=%#x pair=(%Lg,%Lg) tags=%#x sum=%Lg (exp 9900)\n", r, tg1, __real__ c, __imag__ c, tg2, s);
  return !(r==3.0L && __real__ c==1.0L && __imag__ c==2.0L && s==9900 && tg1==0xffff && tg2==0xffff); }
```

`p8_tls.c` (run plain, under `valgrind --error-exitcode=9`, and built with gcc
`-O1 -fsanitize=address`):

```c
#include "common.h"
#include <pthread.h>
static long (*w)(long);
__attribute__((noinline)) long id(long x){ return x+1; }
static pthread_key_t k1, k2; static long late1, late2;
static void d2(void *v){ (void)v; late2 = w(41); }
static void d1(void *v){ (void)v; late1 = w(9); pthread_setspecific(k2,(void*)1); }
static void *thr(void *v){ (void)v; w(1); pthread_setspecific(k1,(void*)1); return NULL; }
int main(void){ cap_on("tls"); w = rt_call_capture_wrap("id",(void*)id);
  pthread_key_create(&k1,d1); pthread_key_create(&k2,d2);
  for(int i=0;i<50;i++){ pthread_t t; pthread_create(&t,0,thr,0); pthread_join(t,0); }
  printf("late1=%ld late2=%ld (exp 10, 42)\n", late1, late2); return !(late1==10&&late2==42); }
```

`p9_ro.c` (run with `setpriv --reuid=65534 --regid=65534 --clear-groups`):

```c
#include "common.h"
#include <sys/stat.h>
__attribute__((noinline)) long id(long x){ return x+1; }
int main(void){ cap_on("ro"); long (*w)(long)=rt_call_capture_wrap("id",(void*)id);
  long s=0; for(int i=0;i<10;i++) s+=w(i);
  char d[128]; snprintf(d,sizeof d,"%s/id",capdir); char f[160]; snprintf(f,sizeof f,"%s/probe.calls.jsonl",d);
  chmod(f, 0400); chmod(d, 0500);
  int e0 = 1234; for(int i=10;i<60;i++){ errno=e0; s+=w(i); if(errno!=e0){printf("errno clobbered\n");return 2;} }
  printf("sum=%ld (exp %d) records=%d\n", s, 60*61/2, count_lines("id",0)); return s != 60*61/2; }
```

`p13_xthread.c` (N2):

```c
#include "common.h"
#include <pthread.h>
#include <ucontext.h>
static ucontext_t co, back; static char stack[1<<16];
static long (*yield_w)(long); static long (*plain_w)(long);
__attribute__((noinline)) long yielder(long x){ swapcontext(&co, &back); return x + 1; }
__attribute__((noinline)) long plain(long x){ return x * 2; }
static long result;
static void body(void){ result = yield_w(41); }
static void *resume(void *a){ (void)a; plain_w(1); getcontext(&back); static int done; if(!done){ done=1; swapcontext(&back, &co);} for(int i=0;i<1000;i++) plain_w(i); return NULL; }
int main(void){ cap_on("xt"); yield_w = rt_call_capture_wrap("yielder",(void*)yielder); plain_w = rt_call_capture_wrap("plain",(void*)plain);
  getcontext(&co); co.uc_stack.ss_sp = stack; co.uc_stack.ss_size = sizeof stack; co.uc_link = &back; makecontext(&co, body, 0);
  swapcontext(&back, &co);
  pthread_t t; pthread_create(&t, 0, resume, 0); pthread_join(t, 0);
  printf("result=%ld (exp 42)\n", result); for(int i=0;i<1000;i++) plain_w(i); puts("main continued"); return result != 42; }
```

Output on the target, both compilers: rc 0, `call capture: too many wrapped calls in progress on
one thread; further calls are not recorded`, `result=42`, `main continued`. On the parent:
rc 134, `a call returned that was never entered`.
