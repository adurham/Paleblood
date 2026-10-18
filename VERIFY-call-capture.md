# Independent verification: generic call capture

- **Target:** `adurham/Paleblood` branch `runtime/generic-input-capture`, commit
  `4c4c92c130eac929ae2393de2ce72ac877d26056` "runtime: record hooked and imported calls through a
  generic thunk". Parent `4372e3f24c` (the owner's local base).
- **Reviewer:** an independent Claude Code session. It did not write the change and made no
  changes to the branch under test. This report is the only file on this branch, which is based on
  the fork's `main` (= `4372e3f`).
- **Date:** 2026-10-18.

## Verdict summary

| Item | Verdict |
| --- | --- |
| CI battery, gcc (build, ctest syslib, ctest rest, harness, differential) | **PASS** |
| CI battery, clang (same steps) | **PASS** |
| Policy steps (no game data, validate, agnostic, readable, progress, hooks) | **PASS** |
| Q1 Default-off is identical to the parent | **PASS** |
| Q2 Env-var naming in code and docs | **PASS**: no mismatch (details below) |
| Q3 Tests exercise the capture-on path | **PASS**, with gaps |
| Q4 ABI safety of the thunk | **FAIL**: the ordinary call ABI is correct, but non-local exits and deep recursion crash the run |
| Q5 Clean room | **PASS** |
| Q6 Base is the owner's local commits | **CONFIRMED**: the commit cannot go upstream by itself |

**Overall: FAIL as written.** CI is green on both compilers and the off path is exactly the parent's
behaviour. When capture is turned on, though, the thunk crashes or aborts the process on
`longjmp`, a wrapped `setjmp`, C++ exceptions, and nesting deeper than 1024 (defects D1 and D2).
The docs describe these cases only as "not recorded". The change also goes against the repo's own
`docs/capture-architecture.md` decision (D3). Since the feature is opt-in, merging it puts default
users at no risk. It should not be presented as safe for capture runs until D1 to D3 are handled or
documented accurately.

## Environment

- Ubuntu 24.04.5 LTS, gcc 13.3.0, Ubuntu clang 18.1.3, cmake 3.28.3. Every Python step ran with
  `python3.12` (the CI runner's python3); the container's default `python3` is 3.13.
  `capstone==5.0.9` was requested, and `python3.12` reports capstone 5.0.7 installed.
- **Clone depth:** the session clone started shallow (`git rev-parse --is-shallow-repository` →
  `true`). I ran `git fetch --unshallow origin` before anything else, so every step, including
  `tools/test_hooks.sh`, ran on a full history. I did not see `test_hooks` misbehave, because I
  never ran it shallow.
- Target and parent were checked out as separate worktrees (`../pb-target` at `4c4c92c`,
  `../pb-base` at `4372e3f`), and the identical battery was run on each.

## CI battery

Script, per tree: `BB_CALL_CAPTURE_DIR`, `BB_CALL_CAPTURE_ONLY` and `BB_CAPTURE_ONLY` unset, then,
for `cc` in `gcc` and `clang`:

```
cmake -S . -B build-$cc -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=$cc -DCMAKE_CXX_COMPILER=$cxx
cmake --build build-$cc -j8
ctest --test-dir build-$cc --output-on-failure --no-tests=error -R '^syslib$'
ctest --test-dir build-$cc --output-on-failure --no-tests=error -E '^syslib$'
CC=$cc BB_LOADER=build-$cc/runtime/libpbloader.so python3.12 -m unittest discover -s test -p 'test_harness.py'
CC=$cc BB_LOADER=build-$cc/runtime/libpbloader.so python3.12 -m unittest discover -s test -p 'test_differ*.py'
```

then once:

```
python3.12 tools/check_no_game_data.py --tracked
python3.12 tools/validate_functions.py
python3.12 tools/check_agnostic.py
python3.12 tools/check_readable.py --base 4372e3f      # parent; the base run used --base 5376799
python3.12 tools/progress.py --check
tools/test_hooks.sh
```

(CI's last build step uses the pattern `test_differential.py`. I ran `test_differ*.py`, which
matches the same single file.)

| Step | Parent `4372e3f` | Target `4c4c92c` |
| --- | --- | --- |
| gcc configure / build | rc 0 / rc 0 | rc 0 / rc 0 (no new warnings) |
| gcc ctest `^syslib$` | 1/1 passed | 1/1 passed |
| gcc ctest `-E ^syslib$` | 12/12 passed | 12/12 passed |
| gcc harness | Ran 34, OK | Ran 35, OK (+`test_call_capture_records_are_not_cases`) |
| gcc differential | Ran 18, OK | Ran 18, OK |
| clang configure / build | rc 0 / rc 0 | rc 0 / rc 0 |
| clang ctest `^syslib$` | 1/1 passed | 1/1 passed |
| clang ctest `-E ^syslib$` | 12/12 passed | 12/12 passed |
| clang harness | Ran 34, OK | Ran 35, OK |
| clang differential | Ran 18, OK | Ran 18, OK |
| check_no_game_data --tracked | rc 0 | rc 0 |
| validate_functions | rc 0 | rc 0 |
| check_agnostic | rc 0 | rc 0 |
| check_readable | rc 0 | rc 0, `readable: ok (0 grandfathered file(s) left ...)` |
| progress --check | rc 0 | rc 0 |
| test_hooks.sh | rc 0, all `ok` | rc 0, 19 `ok` lines |

**Faithfulness note.** CTest's `tools` test (`unittest discover -p "test_*.py"`) skips
`test_tools.Boot` when `build/runtime/pbboot` does not exist. CI builds into `build/`, so CI runs
`Boot`; my `build-gcc` and `build-clang` directories made the ctest step above skip it. To close
that gap I symlinked `build -> build-$cc` in each tree and re-ran the full Python suite
(`env -u BB_CALL_CAPTURE_DIR python3.12 -m unittest discover -s test -p 'test_*.py' -v`):

| | Parent | Target |
| --- | --- | --- |
| gcc | Ran 185, OK | Ran 187, OK |
| clang | Ran 185, OK | Ran 187, OK |

Diffing the per-test outcomes between parent and target shows only two additions, both `ok`:
`test_tools.Boot.test_call_capture_names_import_calls` and
`test_harness.Harness.test_call_capture_records_are_not_cases`. No existing test changed outcome.

Not run: the `session-report` job (it needs a PR body). For reference,
`runtime_pr.classify_paths(git diff --name-only 4372e3f 4c4c92c)` returns
`{'classification': 'decompilation/mixed', 'runtime_changed': True}`. A PR carrying this commit
would therefore need the **full** session report, not the concise runtime one, because it touches
`NEXT.md`, `STATUS.md`, `test/test_harness.py` and `test/test_tools.py`.

## Q1. Default-off is identical to the parent: PASS

Evidence:

1. **Code path.** `rt_call_capture_wrap` returns `target` unchanged when
   `getenv("BB_CALL_CAPTURE_DIR")` is unset or empty (`runtime/call_capture.c:151-155, 218`).
   It returns before `pthread_once`, before any `mmap`, and before touching any file.
   `import_target` in `boot.c` returns `o->bound[i]` directly when capture is off, so the
   trampoline gets the same target as before. In `plugin.c`, `target == h->fn`, so `recorded`
   stays 0 and the extra stderr line is not printed.
2. **Tests.** Every pre-existing test has the same outcome on parent and target, for both
   compilers (table above).
3. **pbboot off run, compared directly.** I built the synthetic one-import ELF from
   `test_call_capture_names_import_calls` and ran it on both the parent's and the target's
   `pbboot` (gcc and clang builds) with `BB_CALL_CAPTURE*` stripped from the environment, under
   `strace -f -e trace=mkdir,mkdirat,openat`. All four runs: rc 0, the same stderr, and the same
   `--status` JSON (`diff` → `IDENTICAL`). Each showed **0 mkdir calls, 0 opens of
   `*.calls.jsonl`, and no new files**.
4. `test_plugin.c` (capture explicitly unset) checks that both hooks receive the replacement
   pointers themselves. `test_syslib.c` checks that `rt_call_capture_wrap(...) == target` with
   capture off.

## Q2. Env-var naming: PASS (no mismatch for this feature)

- **What the code reads** (`grep -rn getenv runtime/call_capture.c`):
  - `BB_CALL_CAPTURE_DIR` (lines 153 and 194)
  - `BB_CALL_CAPTURE_ONLY` (line 159)
  - `BB_CAPTURE_RUN` (line 134)
- **What the docs say:** `call_capture.h:10` says `BB_CALL_CAPTURE_ONLY`, and `STATUS.md:36` (the
  new entry) says `BB_CALL_CAPTURE_ONLY`. The tests set `BB_CALL_CAPTURE_ONLY`. These all agree.
- `BB_CAPTURE_ONLY` appears at `STATUS.md:86` (Known issues) and `game/routes/README.md:13`. It
  belongs to the **pre-existing** case writer `runtime/capture.c:59-63`, not to this feature.
  `git blame` puts `STATUS.md:86` in `b6e299c` (2026-10-06), not in this commit. Both names are
  correct for their own features.
- **Minor (not a defect):** the two features use separate `_ONLY` variables but share
  `BB_CAPTURE_RUN`. That is deliberate (one run id for both kinds of output), and the header says
  so. A reader may still assume `BB_CAPTURE_ONLY` limits call capture too. Suggestion: one
  sentence in STATUS.md or runtime/README.md saying the two `_ONLY` variables are separate.

## Q3. Tests exercise the on path: PASS, with gaps

| Test | Where it runs in CI | What it proves |
| --- | --- | --- |
| `test_syslib.c` `call_capture_tests` | ctest `syslib` (both compilers) | **Off:** wrap returns the target. **On:** wrap returns a different address, and calls through it return the target's result: `sceKernelGetDirectMemorySize` gives `0x180000000`. `errno` is preserved across the record write. The record has the expected name, run_id, call and record number, and `returns.rax`. Pointer argument values are recorded in `rsi`, and the target still writes through them. Eight integer args, two on the stack, give correct results. Doubles and floats go through ymm0/ymm1 and the double return through ymm0. Variadic `al` = 3 is preserved and recorded. A nested wrapped call returns correctly, with one record each. Sampling: 1201 calls give 51 records. `BB_CALL_CAPTURE_ONLY` filters names. |
| `test_plugin.c` | ctest `plugin` | `bbgame_init` with capture on and `ONLY=replacement_one` sends only that hook through a thunk (`installed[0].target != replacement_one`, `installed[1].target == replacement_two`). Calling the installed thunk runs the replacement, and a record line containing `"name": "replacement_one"` exists in `<dir>/replacement_one/plugin-test.calls.jsonl`. |
| `test_tools.Boot.test_call_capture_names_import_calls` | ctest `tools` (runs only when `build/runtime/pbboot` exists, which is true in CI) | End to end through `pbboot`. Off: no capture directory is created, and the outcome is `returned`. On: the outcome is still `returned`. The file is `libkernel.sceKernelGetDirectMemorySize/boot-test.calls.jsonl`, and **every line goes through `json.loads`**: one record, `returns.rax == 0x180000000`, and `from` equals the eboot call site `0x800000006`. |
| `test_harness.Harness.test_call_capture_records_are_not_cases` | harness step | `verify.run_cases`, `promote.cases` and `harness.py --cases` ignore `*.calls.jsonl` files, even inside a case directory. |

So the on path is exercised: thunk installed, calls recorded, JSON well-formed (parsed in the
pbboot test).

Gaps:

- The C tests match records with `strstr` and never parse them. Only the pbboot test parses JSON.
  My own probe parsed 22 records of mixed classes plus 200 multithreaded records: all well-formed,
  with exactly the documented keys.
- No tests for:
  - threads (my probe: 8 threads × 200k calls gave correct results, exactly 200 records, unique
    record numbers)
  - non-local exits (D1)
  - nesting depth (D2)
  - a host without AVX
  - a failed record write
  - x87 returns

## Q4. ABI safety of the recording thunk: FAIL (crashes on non-local exits)

### The thunk's register and stack handling (`call_capture.c:70-111`), as read

- **Entry.** The stub does `movabs r11, thunk; jmp [rip]`. r11 is scratch in System V, and the
  import trampoline also uses r11 for its own jump, so nothing an argument uses is lost.
- **Save and restore around `rt_call_capture_enter`.** The thunk does `push rbp; mov rbp,rsp;
  and -16; sub 320`. It saves rdi, rsi, rdx, rcx, r8, r9, rax (for `al`) and ymm0-7 (unaligned
  `vmovdqu`), and restores them all exactly. Both C helpers run on a 16-byte-aligned stack.
- **Return address and stack arguments.** The thunk does `leave; add $8,%rsp; call *%r11`, which
  overwrites the caller's return-address slot with the thunk's own. The target therefore sees
  `rsp ≡ 8 (mod 16)` and its stack arguments at their original offsets. The caller's return
  address is kept on the per-thread shadow stack and written back into the same slot before `ret`.
  Tail calls between wrapped functions are handled correctly too.
- **Return values.** rax, rdx, ymm0 and ymm1 are saved around `rt_call_capture_leave` and
  restored. That covers INTEGER, SSE, mixed rax/xmm0, the xmm0:xmm1 pair, `__m256`, and sret,
  where the hidden pointer arrives in rdi and comes back in rax.
- **Callee-saved registers.** rbx and r12-r15 are preserved by the C helpers. rbp is pushed and
  popped. r10 and r11 are clobbered, which System V allows (r10 is only a static-chain register,
  unused by C and C++).
- **TLS.** The guest's thread pointer is redirected to `gs` (`ARCH_SET_GS`, `boot.c:451`), so the
  host's `fs`-based `__thread shadow` stays valid while guest code runs.
- **errno** is saved and restored in both helpers.

### Probes

Sources are in the appendix. Each probe is linked against the target's `runtime/call_capture.c`
and built with gcc and with clang (`-O2`). Results were identical for both compilers.

| Probe | Result |
| --- | --- |
| `long double` return (x87 st0) | PASS (forwarded; see D4) |
| `struct {double,double}` return (xmm0:xmm1) | PASS |
| `struct {long,long}` return (rax:rdx) | PASS |
| 64-byte struct return (sret) | PASS |
| `__m256d` arguments and return (full ymm0/ymm1) | PASS |
| 10 doubles (2 on the stack) | PASS |
| 16-byte-aligned `long double` passed by value on the stack | PASS |
| A 32-byte-aligned local in the callee is aligned | PASS |
| rbx, rbp, r12-r15 preserved; rax forwarded (asm harness) | PASS |
| x87 control word and MXCSR preserved | PASS |
| 8 threads × 200k calls to one wrapped function | PASS: results correct, 200 records, all well-formed |
| Recursion 2000 deep through a wrapped name | **FAIL**: `call capture: rec: calls nested deeper than 1024`, abort |
| 1100 `longjmp`s out of a wrapped call to a `setjmp` in an unwrapped caller | **FAIL**: `call capture: jumper: calls nested deeper than 1024`, abort |
| `longjmp` from an inner wrapped call to a `setjmp` inside an outer wrapped call, then the outer returns | **FAIL**: **SIGSEGV** (rc 139); the outer's `ret` pops the inner's stale frame |
| `setjmp` itself wrapped (as a module-provided import would be), then `longjmp` to it | **FAIL**: `call capture: a call returned that was never entered`, abort |
| C++ exception thrown in a wrapped function, caught by its caller | **FAIL**: `terminate called after throwing an instance of 'std::runtime_error'`, abort (the thunk has no CFI and the return address points into the thunk) |

### Concrete risks

The list below is not exhaustive.

- **Non-local exits.** Any non-local exit through a wrapped frame breaks the per-thread shadow
  stack's last-in, first-out order. This is reachable in `pbboot`: `resolve()` binds imports to
  module exports (`boot.c:264-274`), and `import_target` wraps **every** bound import, including
  module-provided `setjmp`, `longjmp`, `__cxa_throw` and `_Unwind_*` when a libc or C++ runtime
  module is loaded with `--module`. Guest C++ exceptions also have to unwind through the thunk's
  return address, which no guest `.eh_frame` describes.
- **x87.** `rt_call_capture_leave` is a C call made while st0 may hold a `long double` result.
  System V requires the x87 stack to be empty at a call. It works today only because the helper
  never uses x87.
- **AVX-512.** Only ymm0-7 are saved. On an AVX-512 host, a host-compiled replacement that takes
  `__m512` arguments would lose their upper halves. This does not affect the guest (Jaguar has no
  AVX-512, QUIRKS.md:98).

## Q5. Clean room: PASS

- `check_no_game_data --tracked`, `check_agnostic` and `check_readable` all pass (above).
- `git diff --name-only 4372e3f 4c4c92c -- game/` is empty: nothing recorder-like was added under
  `game/`, and STYLE.md's "no recording code" rule holds.
- `runtime/call_capture.c` and `call_capture.h` name no module, library, symbol or address. The
  `"library.symbol"` labels in `boot.c` are built at run time from the ELF's import library and
  the user-supplied NID database. The only SDK names (`libkernel.sceKernelGetDirectMemorySize`,
  `scePthreadAttrGetaffinity`) appear in tests and docs, and are public API names already in
  `runtime/syslib`.

## Q6. Base: CONFIRMED on the owner's local commits

- Upstream: `git fetch https://github.com/Yharnam-Hunters/Paleblood.git main`. Its head is
  `af97b90` "feat: add opt-in candidate automation pipeline (#79)".
- `git merge-base upstream/main 4c4c92c` = `af97b90`.
- `git log upstream/main..4372e3f` lists 4 commits that are not upstream:
  - `a7b8443` docs: record the amd-workstation contribution setup
  - `cf7a2c4` runtime: implement scePthreadAttrGetaffinity
  - `5376799` runtime: implement sceKernelGetDirectMemorySize
  - `4372e3f` docs: reflow the STATUS.md entry for the direct memory size
- Upstream has 0 commits that this base lacks, and the fork's `main` equals `4372e3f`.

What this means for an upstream PR:

- A PR from `runtime/generic-input-capture` to `Yharnam-Hunters/Paleblood:main` would carry all
  5 commits. That breaks CONTRIBUTING.md's "one call or a tightly related group in each PR".
- The unit commit **does not stand alone.** Cherry-picking `4c4c92c` onto `upstream/main`
  conflicts in `NEXT.md` and `STATUS.md`. With those conflicts resolved, the gcc build fails in
  `test/test_syslib.c`: `unknown type name 'f_size'`, `'f_affinity'`, `'f_initattr'`,
  `'f_destroyattr'`, and `'direct_memory_size' undeclared`. Its tests depend on `cf7a2c4` and
  `5376799`.
- So the two syscall PRs (`cf7a2c4`, `5376799`) have to land upstream first, and the
  `a7b8443` docs commit has to be dropped or sent separately. Only then can this commit be rebased
  onto upstream main as its own PR, with the full session report (see the CI section).

## Defects, ranked

1. **D1 (HIGH, capture-on only): non-local exits through the thunk crash or abort the run.**
   Reproduced above:
   - nested `longjmp` → SIGSEGV
   - wrapped `setjmp` → abort
   - C++ exception → `std::terminate`

   `pbboot` wraps every bound import, including module-provided `setjmp`, `longjmp` and
   `__cxa_throw`. The header and STATUS.md describe this case only as "A call that does not return
   (longjmp, exit) is not recorded", which understates it: the process dies, or worse, returns to
   the wrong address. At a minimum, never wrap known non-returning or returns-twice symbols and
   say plainly that exceptions cannot cross a wrapped frame. A proper fix would add CFI for the
   thunk and resynchronise the shadow stack by `rsp`.
2. **D2 (MEDIUM, capture-on only): the fixed shadow depth of 1024 aborts the process.**
   Recursion deeper than 1024 through a wrapped name aborts. So do more than 1024 cumulative
   `longjmp`s out of wrapped calls on one thread, because their frames are never popped. This is
   not documented. A recursive hooked game function, or a long session, can hit it.
3. **D3 (MEDIUM, process/docs): goes against `docs/capture-architecture.md`.** That page says an
   assembly trampoline is "Not the first implementation: too broad and risky before a typed
   contract exists". It says "Adding any such production path requires a separately reviewed
   design and ABI tests". It lists "unwind behavior, ... Signal/exception paths ... recursion
   tests" as requirements, and D1 and D2 are exactly those gaps. The commit says so in STATUS.md
   ("Decisions needed") but leaves the page unchanged, so the repo's docs and code now disagree.
   This needs the maintainer's explicit sign-off before an upstream PR.
4. **D4 (LOW): x87 returns.** A `long double` result is forwarded unchanged in practice, but st0
   and st1 are not recorded (the header's list of return registers omits them). The leave helper
   is also called while st0 is live, which is an ABI violation that works only because the helper
   uses no x87. Relevant if PS4 code returns `long double`.
5. **D5 (LOW): possible use-after-free at thread exit.** The `free_shadow` key destructor frees
   the shadow stack, but the `__thread shadow` pointer is never cleared. A wrapped call made from
   a later TLS or key destructor on the same exiting thread would use freed memory. Theoretical.
6. **D6 (LOW): RWX stub pages.** Stubs are mapped `PROT_READ|PROT_WRITE|PROT_EXEC`. On a host
   that denies W+X mappings (SELinux `deny_execmem`) the `mmap` fails and capture warns and
   wraps nothing. That is safe, but worth a line in the docs.
7. **D7 (INFO): test gaps** as listed in Q3. In particular, no test covers non-local exits,
   depth, threads, or the no-AVX and write-failure fallbacks.
8. **D8 (INFO): naming.** `BB_CALL_CAPTURE_ONLY` and `BB_CAPTURE_ONLY` are separate filters but
   share `BB_CAPTURE_RUN`. Not a mismatch (see Q2), but worth one sentence in the docs.

No defect was found on the default-off path.

## Appendix: probes

Probe sources lived in the verifier's scratchpad. The essential ones are reproduced here.
Build and run each against the target tree:
`cc -O2 -mavx -Iruntime/include probe.c runtime/call_capture.c -lpthread`.

`lj.c`: longjmp from an inner wrapped call into an outer wrapped frame. Result: SIGSEGV.

```c
#define _GNU_SOURCE
#include "runtime/call_capture.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
static jmp_buf jb;
static void (*inner_w)(void);
__attribute__((noinline)) void inner(void) { longjmp(jb, 1); }
__attribute__((noinline)) long outer(void) { if (!setjmp(jb)) inner_w(); return 5; }
int main(void)
{
    char dir[] = "/tmp/pb-lj-XXXXXX"; mkdtemp(dir); setenv("BB_CALL_CAPTURE_DIR", dir, 1);
    inner_w = rt_call_capture_wrap("inner", (void *)inner);
    long (*o)(void) = rt_call_capture_wrap("outer", (void *)outer);
    long r = o();
    printf("outer returned %ld to main (expected 5)\n", r);
    return r != 5;
}
```

`sj.c`: setjmp itself wrapped. Result: `call capture: a call returned that was never entered`, abort.

```c
#define _GNU_SOURCE
#include "runtime/call_capture.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
static jmp_buf jb;
int main(void)
{
    char dir[] = "/tmp/pb-sj-XXXXXX"; mkdtemp(dir); setenv("BB_CALL_CAPTURE_DIR", dir, 1);
    int (*sj)(jmp_buf) = rt_call_capture_wrap("libc.setjmp", (void *)_setjmp);
    volatile int n = 0;
    int r = sj(jb);
    printf("setjmp returned %d\n", r); fflush(stdout);
    if (!n++) longjmp(jb, 7);
    puts("second return reached the caller");
    return 0;
}
```

`ex.cpp`: C++ exception through the thunk. Result: `std::terminate`. Build with
`g++ -O2 -Iruntime/include ex.cpp -x c runtime/call_capture.c -lpthread`.

```cpp
#include "runtime/call_capture.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
__attribute__((noinline)) void thrower(int) { throw std::runtime_error("x"); }
int main() {
    char dir[] = "/tmp/pb-ex-XXXXXX"; mkdtemp(dir); setenv("BB_CALL_CAPTURE_DIR", dir, 1);
    auto t = (void (*)(int))rt_call_capture_wrap("thrower", (void *)thrower);
    try { t(1); } catch (const std::exception &) { puts("caught"); return 0; }
    return 1;
}
```

The full ABI probe (`abi.c`) covers every return class and argument class in the Q4 table, a
hand-written asm harness that checks callee-saved registers, the x87 control word and MXCSR, the
2000-deep recursion, and the 1100 `longjmp`s. The multithreaded probe (`mt.c`) runs 8 threads ×
200k calls and parses the records afterwards. Both follow the same pattern as the probes above.
