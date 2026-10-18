# CI-parity verification: `runtime/module-stub-loading`

Report-only run. No code was changed.

| | |
|---|---|
| Branch under test | `runtime/module-stub-loading` @ `1675719904ca299e7b34ed4d23735b2ba79d39d2` ("runtime: report stub modules loaded at run time") |
| Parent (baseline) | `4372e3f24c512ff264bd3667e7d92b7c2599f4d8` ("docs: reflow the STATUS.md entry for the direct memory size") |
| Date | 2026-10-18 |
| Host | Ubuntu 24.04.5 LTS (same runner image family as `ubuntu-24.04`) |
| Toolchain | gcc 13.3.0, clang 18.1.3, cmake/ctest 3.28.3, Python 3.13.16 |
| Workflow replicated | `.github/workflows/ci.yml` as it is on the branch |

## Verdict

**The branch passes the CI battery as-is.** All 13 `build` matrix steps (gcc **and** clang) and all 6 `policy`
steps pass on the branch in a CI-equivalent checkout. Nothing regresses against the parent: in every
environment tested, the branch and its parent produce the same results.

One step, `tools/test_hooks.sh`, failed in the *session's own* checkout, on the branch **and** on the
parent alike. The cause is the session's depth-50 shallow clone and has nothing to do with the code: the
same script passes for both commits in a depth-1 clone (what `actions/checkout@v4` produces) and in a
full-history clone. See [the analysis below](#the-one-failure-test_hookssh-in-the-sessions-checkout).

## Scope: the workflow file vs. the request

The request named 8 steps. Read in full, `ci.yml` contains more, so I ran everything it contains:

* `build` is a **matrix of gcc/g++ and clang/clang++** (`fail-fast: false`). Both legs were run.
* `policy` has three more steps after the three that were requested: `tools/check_readable.py`,
  `tools/progress.py --check` and `tools/test_hooks.sh`. All were run.
* `session-report` runs only on `pull_request`. There is no PR, so it is **not applicable** and was not run.

`check_readable.py` takes a `--base`. On a `push` event, CI uses `github.event.before`. For a one-commit
push of this branch that is the parent, so I used `--base 4372e3f2` for the branch run and `--base 5376799`
(the parent's own parent) for the parent run.

Each tree was a separate `git worktree` checkout. `build/` was wiped before each compiler leg.

## Commands (run in each tree)

```sh
python3 -m pip install --disable-pip-version-check capstone==5.0.9
# for (cc,cxx) in (gcc,g++) (clang,clang++):
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=$cc -DCMAKE_CXX_COMPILER=$cxx
cmake --build build -j
ctest --test-dir build --output-on-failure --no-tests=error -R '^syslib$'
ctest --test-dir build --output-on-failure --no-tests=error -E '^syslib$'
CC=$cc BB_LOADER=build/runtime/libpbloader.so python3 -m unittest discover -s test -p 'test_harness.py'
CC=$cc BB_LOADER=build/runtime/libpbloader.so python3 -m unittest discover -s test -p 'test_differential.py'
# policy job
python3 tools/check_no_game_data.py --tracked
python3 tools/validate_functions.py
python3 tools/check_agnostic.py
python3 tools/check_readable.py --base <before-sha>
python3 tools/progress.py --check
tools/test_hooks.sh
```

## Results

| Job / step | Branch `1675719` | Parent `4372e3f` |
|---|---|---|
| pip install capstone==5.0.9 | PASS (`Successfully installed capstone-5.0.9`) | PASS (already satisfied) |
| **gcc** configure | PASS | PASS |
| gcc build | PASS, 0 warnings | PASS, 0 warnings |
| gcc ctest `^syslib$` | PASS, 1/1 | PASS, 1/1 |
| gcc ctest remaining | PASS, 12/12 | PASS, 12/12 |
| gcc `test_harness.py` | PASS, `Ran 34 tests … OK` | PASS, `Ran 34 tests … OK` |
| gcc `test_differential.py` | PASS, `Ran 18 tests … OK` | PASS, `Ran 18 tests … OK` |
| **clang** configure | PASS | PASS |
| clang build | PASS, 0 warnings | PASS, 0 warnings |
| clang ctest `^syslib$` | PASS, 1/1 | PASS, 1/1 |
| clang ctest remaining | PASS, 12/12 | PASS, 12/12 |
| clang `test_harness.py` | PASS, `Ran 34 tests … OK` | PASS, `Ran 34 tests … OK` |
| clang `test_differential.py` | PASS, `Ran 18 tests … OK` | PASS, `Ran 18 tests … OK` |
| policy: `check_no_game_data.py --tracked` | PASS | PASS |
| policy: `validate_functions.py` | PASS | PASS |
| policy: `check_agnostic.py` | PASS | PASS |
| policy: `check_readable.py --base …` | PASS (`readable: ok (0 grandfathered file(s) left …)`) | PASS |
| policy: `progress.py --check` | PASS | PASS |
| policy: `test_hooks.sh`, session's depth-50 checkout | **FAIL** (env) | **FAIL** (env), identical |
| policy: `test_hooks.sh`, depth-1 clone (CI-equivalent) | PASS, exit 0 | PASS, exit 0 |
| policy: `test_hooks.sh`, full-history clone | PASS, exit 0 | PASS, exit 0 |
| session-report | N/A (pull_request only) | N/A |

CTest suites, identical for both compilers and both commits:

```
 1/12 hooks  2/12 plugin  3/12 host_hooks  4/12 capture  5/12 capture_poc_fail_closed
 6/12 capture_poc_replay  7/12 tools  8/12 verify_self_test  9/12 validate_functions
10/12 check_agnostic  11/12 float_flags  12/12 progress_readme        -> 100% passed
 syslib (#7 in -R run)                                                  -> 100% passed
```

The branch's new test code ran as part of this battery: 41 lines in `test/test_syslib.c` (the `syslib`
CTest) and 69 lines in `test/test_tools.py` (the `tools` CTest). Both pass under gcc and clang.

## The one failure: `test_hooks.sh` in the session's checkout

Raw output, branch (the parent's is the same apart from git's detached-HEAD notice):

```
ok   shared pre-commit hook is a worktree dispatcher
ok   shared hook dispatches to the active linked worktree
ok   shared commit-msg hook preserves its name for legacy checks
ok   fake ELF under a harmless name (fail)
ok   fake eboot.elf (fail)
ok   local-only file staged (fail)
ok   AGENTS.md staged (fail)
ok   stale README progress block (fail)
ok   hand-edited progress number (fail)
ok   consistent change with regenerated README (pass)
ok   transcription under game/ (fail)
ok   allowlist grown (fail)
ok   wrong identity refused
ok   bb.refuseMessage line in commit message refused
ok   Co-Authored-By trailer accepted
ok   contributor identity passes without --maintainer
ok   a failing gate refuses a piped push
FAIL a passing gate blocked the push
ok   install_hooks.sh without --maintainer leaves enforcement off
EXIT 1
```

The script hides the push's stderr (`2>&1 | tail -1 >/dev/null`). I reran it from a scratch copy with the
output visible:

```
error: Could not read a5490a81fa337d20654ee58d515c1ae647e93fde
fatal: Failed to traverse parents of commit e21034d1f9f3397070291963996be37446470252
error: remote unpack failed: eof before pack header was fully read
 ! [remote rejected] HEAD -> main (unpacker error)
```

`e21034d1…` is the shallow boundary of the session's clone (`.git/shallow`, 59 commits deep), and
`a5490a81…` is its missing parent. The test makes a local clone of that shallow repository, then pushes
from it to a new bare repository, and pack-objects fails at the graft. Setting `push.negotiate=false`
(this VM sets it to `true` globally) did not help.

To confirm the failure comes from the environment, I ran `tools/test_hooks.sh` for both commits in fresh
clones fetched from `origin`:

| Checkout | Branch `1675719` | Parent `4372e3f` |
|---|---|---|
| `git fetch --depth=1` (matches `actions/checkout@v4`) | exit 0, no FAIL | exit 0, no FAIL |
| full history (101–102 commits) | exit 0, no FAIL | exit 0, no FAIL |

**Classification:** pre-existing, environment-only (session checkout depth). It is not caused by the
branch and would not occur on a GitHub-hosted runner. Optional, not done here: the test could be hardened
against arbitrary-depth shallow sources, for example by cloning with `--no-local` or pushing with
`--depth`/`git fetch --unshallow` handling.

## Branch diff under test (`4372e3f2..16757199`)

```
 NEXT.md                           |  9 +++--
 STATUS.md                         | 16 ++++++++
 docs/runtime-syscalls.md          | 30 ++++++++++++++
 game/modules.csv                  | 16 ++++----
 runtime/boot.c                    | 47 ++++++++++++++++++++--
 runtime/include/runtime/process.h | 13 ++++++
 runtime/syslib/kernel_sysmodule.c | 85 +++++++++++++++++++++++++++++++++++++++
 test/test_syslib.c                | 41 +++++++++++++++++++
 test/test_tools.py                | 69 +++++++++++++++++++++++++++++++
 tools/boot.py                     | 38 +++++++++++++----
 10 files changed, 343 insertions(+), 21 deletions(-)
```
