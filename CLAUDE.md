# CLAUDE.md

Voland: Nintendo Switch emulator, web-first (WASM/WebGPU), C11 core, thin
platform layers. The authoritative design is `docs/DESIGN.md` (v3.0.0).
Read §1 before any task. Read the section governing your task before
writing code — section numbers are cited below.

## Current phase

**Phase 0 — Skeleton.** Task list: DESIGN.md §25. Do not start work
belonging to a later phase, even if it seems adjacent.
<!-- Maintainer updates this line at each phase gate. -->

## Hard rules — violating any of these fails review

1. **NEVER write code that decrypts Nintendo content**, handles prod.keys
   for decryption, implements key derivation, or parses encrypted NSP/XCI.
   The loader consumes pre-decrypted NCA only (§1.6). If a task appears to
   require decryption, STOP and say so instead of implementing it.
2. **Never copy code** from Ryubing, yuzu, dynarmic, or any other emulator.
   Read them for behavior; reimplement from the observed behavior.
3. The three load-bearing decisions are fixed. Do not "simplify" around them:
   - One linear memory; guest RAM is a region inside it (§4). Never allocate
     a separate SharedArrayBuffer for anything the C core touches.
   - All guest addresses are virtual; every guest memory access goes through
     the softmmu (§5). HLE uses `vmm_*` only. Pointer arithmetic into guest
     RAM outside `core/common/vmm.c` is a rejected PR.
   - CPU backends implement `run(state, cycle_budget) → CPU_ExitReason` (§7,
     §8). Run-to-completion does not exist. Blocking SVCs mark the thread
     waiting and return; nothing in HLE ever blocks or busy-waits.
4. **core/ is C11.** No C++, no exceptions, no `malloc` in hot paths (use
   `common/arena`), explicit `Error` returns (§3). No magic numbers.
5. **TypeScript strict, no `any`, no implicit returns.** Never mutate
   function arguments; return new state (§3).
6. **No per-frame data over postMessage** (§6). Frames, audio, GPU commands,
   and input travel through linear-memory regions. postMessage is for
   lifecycle events only (init, pause, resize, connect/disconnect).
7. **Memory size is fixed at boot** — `initial === maximum`, growth
   disabled (§4). Never add `ALLOW_MEMORY_GROWTH` or grow the memory.
8. **Register index 31 is invalid** at the CPU backend interface (§8).
   SP has dedicated accessors. Debug-assert `index <= 30`.
9. **No aggregation servers, no cloud cache, no telemetry** (§1.6, §15).
   **Never write code that connects to Nintendo servers or ships Nintendo
   endpoints, DNS names, or certificates** — not for online play, not for
   auth, not "for completeness" (§20). The sfdnsres override table ships
   empty. **Mod repos: mechanism only** — no shipped/curated repos, no CORS
   proxy, and no host-side plugin execution (JS/wasm/UI). Mods are
   guest-side data only (§19).
10. **Off-limits until unlocked by the maintainer:**
    `core/cpu/backends/ballistic/**`. Ballistic is a separate upstream
    project; its WASM backend is paused and its IR API is unstable per its
    maintainer. Do not write code against Ballistic's IR, vendor its
    headers, implement a homegrown WASM emitter, or modify anything under
    `recompiler/`. The web execution path is the interpreter (predecoded
    form from Phase 5). Desktop ballistic-x86 integration unlocks
    separately, also by the maintainer.

## Workflow

- **Interface headers gate implementation.** For any new or changed file in
  `core/**/*.h`: propose the full header plus a test plan, then STOP for
  maintainer review. Do not implement against an unreviewed header.
- **Tests land with code.** Every C subsystem ships tests in `tests/`.
  Every HLE service ships an SVC-path test driven through the noop backend
  (set registers, invoke the SVC handler, assert register/vmm state).
  CPU instruction tests are data-driven vectors (encoding, pre-state,
  post-state), not ad-hoc asserts.
- **Deviations update the doc.** If implementation forces a departure from
  DESIGN.md (an Emscripten limitation, a WebGPU quirk, anything), update
  `docs/DESIGN.md` in the same PR and state the deviation explicitly in the
  PR description. Silent workarounds fail review.
- **Nothing is done until CI proves it.** "It should work" is not a status.
  The browser smoke test must pass for any change touching `platform/web/`
  or Emscripten flags.
- One task per branch. Conventional commits (`feat(vmm): ...`,
  `test(hle): ...`). PR description: what, which DESIGN.md sections govern
  it, how it was verified.

## Build and test

```
# Native (noop backend, default)
cmake --preset native-noop && cmake --build --preset native-noop
ctest --preset native-noop

# Web (Emscripten; flags per DESIGN.md §24 — do not alter without a
# same-PR §24 update)
cmake --preset web && cmake --build --preset web

# Web platform
npm ci && npm run typecheck && npm test
npm run e2e     # headless Chrome: boots, asserts crossOriginIsolated,
                # asserts the full memory allocation succeeds
```

If a preset or script above doesn't exist yet, creating it IS the Phase 0
task — do not substitute manual commands and move on.

## Review tripwires (run before requesting review)

```
grep -rn  "malloc\|new \|throw"          core/ --include=*.c
grep -rn  "guest_ram"                    core/hle/
grep -rln ": any\|as any"                platform/web/src/
grep -rin "aes\|decrypt\|prod\.keys"     core/
grep -rn  "postMessage"                  platform/web/workers/   # lifecycle only?
grep -rn  "addEventListener(.message"    platform/web/src/       # zero allowed on window/document (§16)
```

A hit is either a bug or needs a written justification in the PR.

## Layout pointers

- `core/common/layout.{h,c}` — single source of truth for linear-memory
  regions (§4). All offsets come from `layout_get()`; never hardcode one.
- `core/common/vmm.{h,c}` — softmmu (§5). The only guest-memory gateway.
- `core/hle/kernel/scheduler.{h,c}` — guest green-thread scheduler (§7).
- `core/cpu/dispatch.{h,c}` — JIT block dispatcher (§11). Phase 5.
- `docs/GPU_COMMAND_STREAM.md` — versioned CPU→GPU record format (§13).
  Any change to `command_stream.h` bumps the version and updates this doc.
- Reference implementations table: DESIGN.md §26. Behavior only; rule 2.
