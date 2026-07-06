# Contributing to Voland

Voland is a Nintendo Switch emulator, web-first (WASM/WebGPU), with a C11
core and thin platform layers. The authoritative design document is
[`docs/DESIGN.md`](docs/DESIGN.md) — read §1 before anything else, and read
the section that governs whatever you're touching before you write code.
This file covers how to set up, build, test, and submit changes; it does not
duplicate the architecture itself.

If you're using an AI coding agent against this repo, also read
[`CLAUDE.md`](CLAUDE.md) — it states the same rules in the form an agent
needs (hard rules, review tripwires), and applies to human contributors too.

## Current phase

**Phase 0 — Skeleton.** The task list is `docs/DESIGN.md` §25. Don't start
work that belongs to a later phase just because it seems adjacent — e.g. the
softmmu (`core/common/vmm.c`, §5) and the guest thread scheduler (§7) are
Phase 1/2 deliverables, not something to bolt on early because a Phase 0
header happens to reference them.

## Before you write any code

1. Read `docs/DESIGN.md` §1 (Project Overview) and §1.6 (Legal Scope
   Boundaries) if you haven't already.
2. Read the specific section governing your change. Section numbers are
   cited throughout the codebase in comments (`// see §5`) — follow them.
3. **If your change adds or modifies a file under `core/**/*.h`**, propose
   the full header plus a short test plan first, and stop there for review
   before implementing against it. Headers are the interface contract for
   the rest of the project; get them reviewed before code depends on them.

## Development setup

Minimum tool versions are tracked in [`global.json`](global.json):

| Tool | Version |
|---|---|
| CMake | ≥3.24 |
| Ninja | ≥1.11 |
| Clang | ≥16 |
| Emscripten (emsdk) | ≥3.1.60 |
| Node.js | ≥22.6 (required for `--experimental-strip-types`, used by `npm run test`) |
| pnpm | ≥9 |

For the web build, install and activate the Emscripten SDK, then source its
environment script before configuring CMake:

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source ./emsdk_env.sh   # emsdk_env.bat / emsdk_env.ps1 on Windows
```

## Building and testing

Use the CMake presets in [`CMakePresets.json`](CMakePresets.json) rather
than hand-rolled `cmake` invocations — if a preset or npm script you need
doesn't exist yet, adding it is part of the task, not something to work
around with a one-off command.

```bash
# Native (noop backend, default) — everything except actual game execution
cmake --preset native-noop
cmake --build --preset native-noop
ctest --preset native-noop

# Web (Emscripten) — requires $EMSDK set, see above
cmake --preset web
cmake --build --preset web
```

`CPU_BACKEND` defaults to `noop` in both presets. The no-op backend
maintains full register state and honors the exit-reason contract (§9)
without executing any ARM instructions, which is why the entire frontend,
HLE scaffolding, and worker architecture can be built and tested well
before a real CPU backend exists.

```bash
cd platform/web
npm install
npm run typecheck   # tsc --noEmit
npm run test        # unit tests (node --test), no browser needed
npm run build        # production build
npm run e2e          # Playwright, headless Chromium: boots the built app,
                      # asserts crossOriginIsolated, asserts the full
                      # ~5.25GiB shared-memory allocation succeeds
```

`npm run e2e` needs Chromium once: `npx playwright install chromium`.

### What ships with a change

- **Every C subsystem ships tests in `tests/`.** Every HLE service ships an
  SVC-path test driven through the noop backend (set registers, invoke the
  SVC handler, assert register/vmm state). CPU instruction tests are
  data-driven vectors (encoding, pre-state, post-state), not ad-hoc asserts.
- **Web-platform logic that doesn't need a browser** gets a unit test under
  `platform/web/tests/unit/` (Node's built-in test runner, no bundler
  needed). Boot-path and cross-origin-isolation behavior that *does* need a
  browser goes in `platform/web/e2e/`.
- **Deviations update the doc.** If the implementation forces a departure
  from `DESIGN.md` — an Emscripten limitation, a WebGPU quirk, a browser API
  that doesn't behave the way an earlier revision assumed — fix the doc in
  the same PR and say so explicitly in the PR description. Verify surprising
  claims against a real browser/toolchain before trusting them; this
  document has had to correct itself before (see its changelog) precisely
  because assumptions about web platform APIs didn't hold up.
- **Nothing is done until it's actually been run.** "It should work" isn't
  a status. If your change touches `platform/web/` or the Emscripten build
  flags, run the e2e test against a real build before calling it finished.

## Workflow

- One task per branch.
- Conventional commit messages: `feat(vmm): ...`, `fix(hle): ...`,
  `test(cpu): ...`, `build: ...`, `docs: ...`, `chore: ...`.
- PR description states: what changed, which `DESIGN.md` sections govern
  it, and how it was verified (commands run, tests added, what you
  confirmed against a real build vs. what you couldn't verify and why).

## Hard rules

These fail review regardless of how the rest of the change looks. The full,
authoritative list is in [`CLAUDE.md`](CLAUDE.md#hard-rules--violating-any-of-these-fails-review);
summarized:

1. **No decryption code.** No prod.keys handling, no key derivation, no
   parsing of encrypted NSP/XCI. The loader consumes pre-decrypted NCA only
   (§1.6). If a task seems to require decryption, stop and say so instead
   of implementing it.
2. **Never copy code** from Ryubing, yuzu, dynarmic, or any other emulator.
   Read them for behavior; reimplement from the observed behavior.
3. The three load-bearing architectural decisions are fixed — don't
   "simplify" around them: one linear memory (no separate `SharedArrayBuffer`
   for anything the C core touches, §4); all guest memory access goes
   through the softmmu, HLE uses `vmm_*` only (§5); CPU backends implement
   `run(state, cycle_budget) → CPU_ExitReason`, never run-to-completion (§7/§8).
4. `core/` is C11: no C++, no exceptions, no `malloc` in hot paths (use
   `common/arena`), explicit `Error` returns, no magic numbers.
5. TypeScript strict, no `any`, no implicit returns. Never mutate function
   arguments — return new state.
6. No per-frame data over `postMessage` (§6). Frames, audio, GPU commands,
   and input travel through linear-memory regions; `postMessage` is for
   lifecycle events only.
7. Memory size is fixed at boot — `initial === maximum`, growth disabled
   (§4). Never add `ALLOW_MEMORY_GROWTH` or grow the memory.
8. Register index 31 is invalid at the CPU backend interface (§8). SP has
   dedicated accessors; debug-assert `index <= 30`.
9. No aggregation servers, no cloud cache, no telemetry, no code that
   connects to Nintendo's servers or ships Nintendo endpoints/DNS
   names/certificates (§1.6, §15, §20). Mod repos are mechanism only.
10. `core/cpu/backends/ballistic/**` and `recompiler/` are off-limits until
    unlocked by the maintainer.

## Review tripwires

Run these before requesting review. A hit is either a bug or needs a
written justification in the PR description:

```bash
grep -rn  "malloc\|new \|throw"          core/ --include=*.c
grep -rn  "guest_ram"                    core/hle/
grep -rln ": any\|as any"                platform/web/src/
grep -rin "aes\|decrypt\|prod\.keys"     core/
grep -rn  "postMessage"                  platform/web/workers/   # lifecycle only?
grep -rn  "addEventListener(.message"    platform/web/src/       # zero allowed on window/document (§16)
```

`core/common/arena.c`'s own `malloc()` call is the accepted exception — it's
the one place that's allowed to call it, so every other subsystem doesn't
have to.

## Where to start

**TypeScript/JavaScript:** the web platform layer develops entirely against
the no-op backend. Start at `platform/web/src/main.ts` and work outward —
the boot sequence, worker protocol, and layout handshake are all exercised
without needing a real CPU backend.

**C:** the highest-leverage subsystems are `core/common/vmm.c` (softmmu, §5)
and the guest thread scheduler (§7) — they gate almost everything else —
followed by HLE services from the §12 priority list. Use Ryubing's C#
implementations as a behavioral reference only (see hard rule 2).

**Compiler engineering:** the Ballistic WASM backend, once unlocked (see
hard rule 10). Join the Pound Discord and engage on IR design before it
solidifies; the integration contract is §10.

## Code review requirements

- No merge without tests for HLE services.
- No C++ in `core/`, no exceptions. The only third-party code permitted in
  `core/` is vendored single-file LZ4 (BSD) for NSO segment decompression.
- No `any` in TypeScript.
- All guest addresses are virtual unless a parameter is named `guest_pa`;
  HLE memory access goes through `vmm_*` — direct guest-RAM pointer
  arithmetic outside `vmm.c` is rejected.
- No per-frame data over `postMessage`.
- No code that decrypts Nintendo content; no server-side aggregation of
  user-derived data.

## Legal

Voland accepts only pre-decrypted game data and does not contain, and will
never contain, Nintendo decryption or key-derivation logic (§1.6). Do not
open PRs proposing otherwise, even as an opt-in feature — see the design
doc's Non-Goals (§1.5) for why "just make it optional" doesn't change the
legal exposure.

Voland is licensed under [GPL-2.0](LICENSE) and is not affiliated with
Nintendo Co., Ltd.
