# Workstation setup (`amd-workstation`)

One page for the Linux contribution environment: what is installed, where the dump lives, and
the exact commands that are known to work for build, test, boot and verify. Everything below was
run end to end on this host. No game data is recorded here; the dump stays outside the
repository ([CONTRIBUTING.md](../CONTRIBUTING.md), "Your own copy").

## Host

- `ssh amd-workstation` (key-based from the Mac).
- Ubuntu 26.04.1 LTS ("resolute"), x86-64, 32 cores, 59 GB RAM, 312 GB free after setup.
- **No passwordless sudo and no root SSH.** System packages were therefore *not* installed with
  `apt`. Every dependency below is either already present or installed into the user's home /
  a user prefix. Anything that needs a system package has to go in a prefix here.

## Repository

- `~/repos/paleblood`
  - `origin`   = `https://github.com/adurham/Paleblood.git`
  - `upstream` = `https://github.com/Yharnam-Hunters/Paleblood.git`
- Checked out at `main` (`af97b90` when this page was written).
- `third_party/bbport` submodule is **not** fetched: CI does not fetch it and the repository
  builds and tests without it.

## Toolchain (versions verified on this host)

| Tool | Version | Where | How it was installed |
|---|---|---|---|
| gcc / g++ | 15.2.0 | `/usr/bin` | preinstalled (`build-essential`) |
| cmake | 4.2.3 | `/usr/bin` | preinstalled |
| make | 4.4.1 | `/usr/bin` | preinstalled |
| python3 | 3.14.4 | `/usr/bin` | preinstalled |
| git | 2.53.0 | `/usr/bin` | preinstalled |
| gh | 2.46.0 | `/usr/bin` | preinstalled; logged in as `adurham` |
| rsync | 3.4.1 | `/usr/bin` | preinstalled |
| unzip / curl | — | `/usr/bin` | preinstalled |
| rustc / cargo | 1.98.1 | `~/.cargo/bin` | rustup, preinstalled |
| orbis-pkg-util | 0.1.0 | `~/.cargo/bin` | `cargo install orbis-pkg-util --version 0.1.0 --locked` |
| Capstone | 5.0.9 (Python wheel; `cs_version()` = `(5, 0, 1280)`) | `~/.local` | `python3 -m pip install --user capstone==5.0.9` (matches the CI pin; the wheel's bundled `libcapstone.so` is what the tracer loads, so `libcapstone-dev` was not needed) |
| ninja | 1.13.2 | `~/.local/bin` | `python3 -m pip install --user ninja` (only for `third_party/bbport`; not needed for `ctest`) |
| OpenJDK | 21.0.12.1 (Temurin) | `~/bb/tools/jdk21` | Adoptium tarball, extracted into the user prefix (Ghidra needs a JDK 21+) |
| clang / clang++ | 21.1.8 | `~/tools/clang` + wrappers in `~/.local/bin` | Ubuntu debs extracted into a user prefix, no sudo (see below) |
| Ghidra | 12.0.3 (`sha256` verified) | `~/bb/tools/ghidra_12.0.3_PUBLIC` | release zip, `MAXMEM=14G` |
| GhidraOrbis | 1.0144 (`sha256` verified) | `…/Ghidra/Extensions/GhidraOrbis` | release zip into `Ghidra/Extensions/` |

`clang` is not in `tools/deps.txt`, but `test/test_differential.py` asserts `clang` is on `PATH`,
so it has to exist. Without sudo it was installed from Ubuntu's debs into a user prefix:

```sh
mkdir -p /tmp/clangdl && cd /tmp/clangdl
apt-get download clang-21 libclang-cpp21 libllvm21 libclang1-21 libclang-common-21-dev
for d in *.deb; do dpkg-deb -x "$d" "$HOME/tools/clang"; done
# ~/.local/bin/clang and ~/.local/bin/clang++ are one-line wrappers that set
# LD_LIBRARY_PATH=$HOME/tools/clang/usr/lib/x86_64-linux-gnu and exec clang-21 / clang++-21.
```

Ghidra's NID database (`nid_db.xml`) lives at
`~/bb/tools/ghidra_12.0.3_PUBLIC/Ghidra/Extensions/GhidraOrbis/data/nid_db.xml`.

## Environment

```sh
export PATH="$HOME/.local/bin:$HOME/.cargo/bin:$PATH"
export JAVA_HOME="$HOME/bb/tools/jdk21"
export BB_DATA_ROOT="$HOME/bb"
export BB_ELF="$BB_DATA_ROOT/elf/eboot.elf"
export BB_NID_DB="$HOME/bb/tools/ghidra_12.0.3_PUBLIC/Ghidra/Extensions/GhidraOrbis/data/nid_db.xml"
```

## The dump

The owner's own pkg pair (base + the 1.09 update), rsynced from the Mac to
`~/Games/Bloodborne/_src/`:

- `CUSA03173.pkg` — 31,351,111,680 bytes
- `CUSA03173 Update v1.09.pkg` — 181,338,112 bytes

(Both byte-exact. On the Mac these live under `/Users/adam.durham/Games/Bloodborne/_src/`. The
merged *bbport-mac* game folder `/Users/adam.durham/Games/Bloodborne/CUSA03173` is **not** what
`prepare_dump.sh` consumes — that script takes the two pkgs.)

Prepared exactly per its usage (`docs/ONBOARDING.md` §2), output **outside** the repository:

```sh
export PATH="$HOME/.cargo/bin:$HOME/.local/bin:$PATH"
cd ~/repos/paleblood
tools/prepare_dump.sh \
    ~/Games/Bloodborne/_src/CUSA03173.pkg \
    "$HOME/Games/Bloodborne/_src/CUSA03173 Update v1.09.pkg" \
    ~/bb
```

Every line said `ok` — `eboot.elf`, the seven `sce_module` ELFs and `eboot.elf (loaded image)`
all match `target.sha256` / `target.image.sha256`.

- **Checked ELFs: `~/bb/elf/eboot.elf`** (plus `libc.elf`, `libSceFios2.elf`, `libSceFace.elf`,
  `libSceHand.elf`, `libSceHeadTracker.elf`, `libSceS3DConversion.elf`, `libSceSmart.elf`).
- Output dir `~/bb` holds `base/`, `update/`, `merged/`, `elf/`, `tools/selfutil` (+ `tools/jdk21`,
  `tools/ghidra_12.0.3_PUBLIC`).
- Re-check any time: `python3 tools/check_target.py ~/bb/elf`.

## Build and test

```sh
cd ~/repos/paleblood
cmake -S . -B build && cmake --build build -j

export PATH="$HOME/.local/bin:$HOME/.cargo/bin:$PATH"
export GIT_CONFIG_GLOBAL="$HOME/.gitconfig-nosign"
ctest --test-dir build --output-on-failure
```

Result on this host: **13/13 tests pass** (100%).

Two local environment tweaks, both outside the repository (no source file is modified):

1. `PATH` must include `~/.local/bin` so the `clang`/`clang++` wrappers are found
   (`test/test_differential.py`'s compiler-identity test).
2. `GIT_CONFIG_GLOBAL=~/.gitconfig-nosign`. This machine's `~/.gitconfig` forces
   `commit.gpgsign=true` with `gpg.format=ssh`, and a non-interactive run has no agent socket, so
   the throw-away `git commit` inside `test/test_tools.py` aborts with `exit status 128`
   ("Couldn't get agent socket"). `~/.gitconfig-nosign` is a minimal config (`user.name` /
   `user.email` only) used for the test run only; a normal interactive `git commit` with
   `SSH_AUTH_SOCK` set still signs as usual.

## Boot harness

```sh
cd ~/repos/paleblood
export BB_ELF="$HOME/bb/elf/eboot.elf"
export BB_NID_DB="$HOME/bb/tools/ghidra_12.0.3_PUBLIC/Ghidra/Extensions/GhidraOrbis/data/nid_db.xml"
export BB_PBBOOT="$PWD/build/runtime/pbboot"
python3 tools/boot.py --elf "$BB_ELF" --nids "$BB_NID_DB"
```

Boot stop line (2026-10-16):

```
pbboot: libc.elf: unimplemented: libc.elf calls libkernel scePthreadAttrGetaffinity (NID 8+s5BzZjxSg), returning to libc.elf+0x81ad5
pbboot: milestone loaded; executable imports 686: 219 from bundled modules, 28 implemented by the runtime, 4 unresolved diagnostics, 435 unbound; imports called 7
```

This matches the documented stop in `STATUS.md` (`scePthreadAttrGetaffinity`, called by
`libc.elf`).

`boot.py` also refreshes `symbols/boot.json` and `symbols/imports.csv`. On this host a fresh run
shifts the imported-symbol counts away from the committed ones (2 → 4 unresolved diagnostics,
437 → 435 unbound; `libc.elf` imports_remaining 57 → 55, `libSceFios2.elf` 71 → 70;
`getpagesize` and `scePthreadYield` become `runtime-unresolved`, `libSceFios2` `scePthreadEqual`
becomes `runtime`). Re-run `boot.py` only when you intend to commit that refresh.

## Verify harness

- `python3 tools/verify.py --self-test` → `verify: self-test ok` (exit 0).
- CI-canonical harness tests (need Linux `ptrace` + Capstone 5):

  ```sh
  cd ~/repos/paleblood
  export BB_LOADER=build/runtime/libpbloader.so CC=gcc
  python3 -m unittest discover -s test -p 'test_harness.py' -v      # 34 tests OK, incl. instruction-trace
  python3 -m unittest discover -s test -p 'test_differential.py' -v # 18 tests OK
  ```

  (`docs/VERIFY.md`'s dotted form `python3 -m unittest test.test_harness …` fails on this
  checkout — `test` is not an importable package; use `discover` as CI does.)
- End-to-end pipeline, original vs replacement on the real executable:

  ```sh
  BB_ELF="$HOME/bb/elf/eboot.elf" tools/smoke.sh    # smoke: PASS (edge cases 10/10; mutant fails 7/10)
  ```
- `ptrace`: `/proc/sys/kernel/yama/ptrace_scope` = `1`. This is fine — the harness attaches to
  its own forked child, which `ptrace_scope=1` permits.

## Gotchas

- **No sudo**: system-wide packages cannot be installed here; use a user prefix (`~/.local`,
  `~/tools`, `~/bb`).
- **macOS `rsync`** is Apple's *openrsync* and rejects `--info=progress2`. Use
  `/opt/homebrew/bin/rsync` (3.5.1) for Mac → workstation transfers:
  `/opt/homebrew/bin/rsync -a --info=progress2 --no-i-r SRC amd-workstation:'DEST/'`.
- **`gh`** is configured as the https git credential helper (`gh auth setup-git`); pushes go over
  https with the gh token. There is no private SSH key in `~/.ssh` on this host.
- Nothing from `~/bb` or `~/Games` may enter the repository or a commit: `.gitignore` covers
  `*.elf` / `*.pkg` and the pre-commit hook (`tools/install_hooks.sh`) refuses game data.
