# Capture architecture: return-aware observation

The runtime has two capture paths. The typed, schema-1 path (the Recorder and the synthetic proof
below) produces verification cases. Generic call capture (`runtime/call_capture.c`) is an
assembly trampoline that records the registers of every wrapped call by name; it is an opt-in
development facility, adopted on the fork on 2026-10-18 under the conditions listed below, and
its records are not verification cases.

## Current hook boundary

The version-1 host interface in `runtime/include/runtime/host.h` exposes an image pointer,
image size, and `install_hook(offset, size, target)`. `runtime/plugin.c::bbgame_init` passes each
replacement function pointer directly as `target` (with generic call capture on, a recording thunk
that calls it, below). The replacement entry then owns the call; the host interface exposes no
callback after it returns and no original-call trampoline.

`rt_hook_entry` contains an address, code size, name, and `void(void)` function pointer. The
registry does not carry the actual function signature, argument register classes, stack argument
count, return class, pointer direction or extent, or the memory reachable through nested pointers.
The Recorder therefore cannot infer a safe schema-1 case from a registry row. A raw address or
register dump would not describe replayable memory and must not be labelled a verification case.

Schema-1 verification cases also need import/stub effects, image globals, and the initial bytes of
every described buffer. Their replay observes return registers, preserved registers, writes, and
scripted calls. Those inputs are function-specific facts, not recoverable from the hook address.

## Observation approaches

| Approach | ABI and return behavior | Stack/register behavior | Pointer memory and imports | Concurrency and failure | Decision |
| --- | --- | --- | --- | --- | --- |
| Typed wrapper dispatch | A wrapper declared with the exact function-pointer type lets the compiler perform the platform ABI call and return the exact result. Unsupported types can be rejected before calling. | Ordinary typed calls handle register/stack assignment; this is only established for each declared signature and build ABI. No universal wrapper is implied. | Explicit descriptors can snapshot bounded input/in-out memory. Nested pointers, globals, and imports still need descriptors or dedicated observers. | Use per-invocation state; reject missing/invalid descriptors before invoking the target. A failed write must not fabricate a completed case. | Smallest candidate for a later production design, but production dispatch needs a way to select and install typed wrappers. The current host interface does not provide one. |
| Assembly trampoline | Forwards unknown calls; must handle each return class exactly (integer, vector/float, x87, and any ABI-specific aggregate class). | Must preserve argument registers, stack arguments, alignment, red zone, callee-saved state, return address, unwind behavior, and nested/reentrant calls. Signal/exception paths and per-thread nesting need explicit treatment. | Still cannot infer pointee extents or import side effects. Descriptors remain necessary. | Needs per-thread nesting state, bounded storage, recursion tests, and defined behavior on allocation/I/O failure. A bug can corrupt arbitrary calls. | Built as generic call capture, an opt-in development facility for recording registers (below), after two independent verifications. It records no memory and yields no cases, so it does not replace the typed path for verification. |
| Isolated typed test harness | Calls only explicitly declared synthetic signatures; compiler handles that signature. No production hook or runtime ABI change. | Tests a declared call and replay behavior, but makes no claim about arbitrary signatures or a production trampoline. | Synthetic descriptors state exact pointer roles and sizes. Existing verifier scripts imports and compares memory effects. | Deterministic local fixtures, fail-closed descriptor checks, and isolated test directories. | Chosen for this milestone: smallest safe proof that validates the metadata and case/replay contract without game data. |

## Generic call capture (what was built)

`runtime/call_capture.c` and `runtime/include/runtime/call_capture.h` (the header is the reference).
With `BB_CALL_CAPTURE_DIR` set, `bbgame_init` installs each hook through a recording thunk named
after the hook, and `pbboot` sends the trampoline of each import the runtime provides through one
named `library.symbol`. Imports bound to a bundled module's export are never wrapped.

- **Stub.** Each wrapped target gets a 16-byte stub, `mov r11, [rip+slot]; jmp [rip+slot+8]`, that
  loads the target's thunk record into r11 (scratch, never an argument) and jumps to the one shared
  thunk. Stubs are W^X: a region's code half is written once, all stubs identical, then made
  read-only and executable; the slots are in its separate writable, non-executable half.
- **Thunk.** It saves rdi..r9, rax (`al`) and ymm0..ymm7, and lets `rt_call_capture_enter` open a
  frame on the calling thread: the caller's return address, rbx and r12, and the argument
  registers when the call is sampled. It removes the return address from the stack and calls the
  target with rbx pointing at the frame and r12 holding its serial, so stack arguments are where
  the caller put them. On return, it saves rax, rdx, ymm0, ymm1 and the x87 state (`fnsave`), and
  `rt_call_capture_leave` writes the record, closes the frame and gives back the caller's return
  address, rbx and r12.
- **Frames.** Per thread, an open-addressed table keyed by the return-address stack slot. A frame
  left without returning (longjmp, a C++ exception, the thread's exit) is closed when the next
  wrapped call on the thread enters at the same slot, or at the thread's exit. Up to 2^20 frames
  per thread; past that, or out of memory, calls run unrecorded with a warning.
- **Unwind information.** The thunk's CFI recovers the caller's return address, rbx and r12 from
  the frame, so host unwinders (C++ exceptions, `backtrace`) pass through it.
- **Threads.** A serial's top 24 bits are the entering thread's capture identity, never reused in
  the process. A return on another thread is detected from r12 before any bookkeeping is touched:
  the record is dropped and the process stops, naming the call. Frame memory is never returned to
  `malloc` (an exiting thread's chunks are kept for later threads), so the name can still be read
  after the entering thread exited.
- **Records.** One JSON line per sampled call (first 50 per name, then every 1000th, at most 200),
  in `$BB_CALL_CAPTURE_DIR/<name>/<run>.calls.jsonl`. Each line begins with `"record_format": 1`
  and `"capture_commit"`, the commit the capture code was built from (`git rev-parse HEAD` at build
  time, `-dirty` when `runtime/` differed from it, `unknown` outside a checkout;
  `runtime/call_capture_commit.cmake`). A thunk bug found later invalidates every record whose
  commit is a descendant of the commit that introduced it and not of the fix.
- **Two switches.** At build time, the CMake option `BB_ENABLE_CALL_CAPTURE` (on by default): off,
  the thunk, stubs, frames and record writer are not compiled at all, and `BB_CALL_CAPTURE_DIR` only
  produces a warning. At run time, nothing is wrapped unless `BB_CALL_CAPTURE_DIR` is set;
  `BB_CALL_CAPTURE_ONLY` limits it to the names listed. With the directory unset, every hook and
  import gets the target itself, as before generic capture existed.

### Verification

Two independent Claude Code sessions, neither of which wrote the code, verified it:

- `VERIFY-call-capture.md` (branch `claude/verify-call-capture-r7k2qx`), of `4c4c92c`: CI green on
  gcc and clang, default-off identical to its parent; **FAIL** as written on non-local exits and
  depth (D1 high: longjmp, wrapped setjmp and C++ exceptions crashed or aborted the run; D2 medium:
  a fixed 1024-frame depth), D3 (medium) the contradiction with this page, D4 to D8 low or info.
- `VERIFY-call-capture-hardening.md` (branch `claude/verify-capture-hardening-xb0hv6`), of
  `bad3066`, the hardening of D1, D2, D4, D5 and D7: **PASS**. Every earlier failing probe passes on
  both compilers, valgrind and ASan are clean on the thread-exit case, and default-off is identical
  to the parent down to the syscalls. It found N1 (low, test only: a bare `.text` in a test's
  top-level asm crashed `test_syslib` under gcc `-O0` ASan) and N2 (low: a call returning on
  another thread corrupted that thread's frames), and left D3 to the maintainer.

### Conditions of adoption

The fork adopted generic call capture as an opt-in development facility (2026-10-18) on these
conditions, all met by the sign-off change (branch `runtime/generic-input-capture-signoff`):

1. **N2 fails loud.** The entering thread's identity is in every frame's serial; a call that
   returns on another thread stops the process naming the call, and its record is dropped
   (`test_syslib`: entering thread still running, and exited).
2. **N1 fixed and sanitized in CI.** Every top-level asm block uses `.pushsection`/`.popsection`;
   CI's `call capture` job runs the capture-on battery (`test_syslib`, `test_plugin`) at `-O0`
   under ASan and UBSan with gcc and clang.
3. **Records are stamped** with `record_format` and `capture_commit`.
4. **This page** describes what was built (this section) instead of ruling it out.
5. **Known gaps are documented** here and in the header (below).
6. **Compile-time gate** `BB_ENABLE_CALL_CAPTURE`; CI builds with it off and checks that no thunk
   symbol is present.

Whether generic call capture goes upstream is a separate decision (D3), still pending.

### Known limitations

- **Guest C++ exceptions cannot cross a wrapped frame.** A guest unwinder searches only the guest
  modules' unwind tables. At the first host frame (the thunk, or the host replacement it calls)
  it finds no unwind information, its search ends, and `__cxa_throw` calls the guest's
  `std::terminate`: a loud stop. It cannot be a silent skip, because an unwinder has no rule for
  stepping past a frame it cannot describe (`test_call_capture_unwind.cpp` shows the host unwinder
  terminating at a frame without unwind information). The thunk adds no new case: a hook's
  replacement is host code that the exception could not cross without the thunk either, and
  module exports are not wrapped. Not confirmed against the target's own unwinder, which would
  need the game's modules.
- **Calls returning on another thread** stop the process (condition 1). The guest imports no
  fiber library (no `libSceFiber` import in `symbols/imports.csv`; STATUS.md).
- **Returns twice.** A wrapped target that returns twice (setjmp) stops the process with a message;
  nothing wrapped today does.
- **rbx and r12.** The frame is found through them; a target that breaks the System V rule stops
  the process or, with a stale value, could crash. A target that rewrites its own return-address
  slot, or switches stacks and reuses a slot of a still-running call, breaks the "same slot means
  the earlier call was left" rule.
- **Signals.** A signal handler on the same thread that makes a wrapped call while the thunk is in
  its enter or leave helper can corrupt that thread's frames (no signal masking).
- **Executable memory.** Stubs are W^X, but still need executable anonymous memory. A host that
  refuses it altogether (SELinux `deny_execmem`, PaX `MPROTECT`) gets no thunks: nothing is
  wrapped and a warning says so. `pbboot` maps guest code executable anyway, so such a host cannot
  run it either way.
- **Not recorded:** stack arguments (no signature gives their count), memory behind pointers, the
  calls a function makes, zmm upper halves (a host replacement taking `__m512` on an AVX-512 host
  loses them; the guest CPU has none), and the x87/SSE control state. Needs an AVX host.
- **Late noting.** Left calls are noted at the next call entering the same slot or at the thread's
  exit; calls still open when the process ends are never noted.
- **Memory.** Frame chunks are never freed, only reused by later threads: peak use is kept for the
  life of the process.
- **Cost.** About twice the per-call overhead of the first version (hash lookup per call); only
  when capture is on.

## Synthetic proof scope

`test/test_capture_poc.cpp` declares one signature explicitly:

```cpp
using Fixture = int32_t (*)(int32_t *out, const int32_t *input, int32_t factor);
```

Its descriptor binds `out` to `rdi` as a four-byte in/out buffer, `input` to `rsi` as a four-byte
input buffer, and `factor` to `rdx` as a signed scalar; the return is `i32`. The adapter snapshots
only those declared bytes, calls through the typed function pointer, and serializes a schema-1
case. The synthetic guest and native replacement are then replayed by `tools/harness.py` and
compared by `tools/verify.py`.

This validates typed-call metadata, deterministic serialization, and compatibility with the
existing replay verifier. It does not instrument `bbgame_init`, install production wrappers, infer
arbitrary signatures, capture real game calls, or establish correctness of a machine-code
trampoline. The fixture covers three integer/pointer arguments in `rdi`, `rsi`, and `rdx`, an `i32`
return, and two distinct four-byte buffers. It does not test stack arguments, floating-point or
aggregate argument/return classes, imports, globals, nested pointers, concurrent calls, or output
file failures. A production path that makes cases from such calls requires a separately reviewed
design and ABI tests (generic call capture, above, had its own and makes no cases).

## Fail-closed conditions

The proof rejects an unsupported function signature, absent descriptor, wrong signature tag,
incorrect argument/register mapping, missing pointer range, zero/oversized buffer extent, or invalid
function/run identifier before calling the function. These checks are deliberately in test-only
code. Production hooks get the replacement itself unless generic call capture is switched on
(above), which records registers only and makes no case.
