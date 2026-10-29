# CLAUDE.md

Voland: Nintendo Switch emulator, web-first (WASM/WebGPU), C11 core, thin
platform layers. The authoritative design is `docs/DESIGN.md` (v3.30.0).
Read §1 before any task. Read the section governing your task before
writing code — section numbers are cited below.

## Current phase

**Phase 1 — Load & Memory.** Task list: DESIGN.md §25. Phase 0 is
complete. Do not start work belonging to a later phase, even if it seems
adjacent.
<!-- Maintainer updates this line at each phase gate. -->

## Hard rules — violating any of these fails review

1. **NEVER write code that decrypts Nintendo content**, handles prod.keys
   for decryption, implements key derivation, or parses encrypted NSP/XCI.
   The loader consumes pre-decrypted NCA only (§1.6). If a task appears to
   require decryption, STOP and say so instead of implementing it.
2. **Never copy code** from Ryubing, yuzu, dynarmic, Ballistic, or any other
   emulator or recompiler. Read them for behavior; reimplement from the
   observed behavior.
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
10. **JIT backends (§10).** The web JIT is Voland's own ARM64→WASM
    translator, `core/cpu/backends/wasm_jit/`, specified in
    `docs/WASM_JIT.md`. It is a Phase 5 deliverable: no `wasm_jit` code
    before Phase 5 opens (the shared A64 decoder and record format land
    with the Phase 2 interpreter), and its headers go through header review
    like any other. Ballistic informs it as ideas only (rule 2), never code.
    Ballistic itself is an optional desktop x86 backend consumed unmodified:
    never modify, fork, patch, or vendor anything under `recompiler/`, and
    `core/cpu/backends/ballistic/**` stays off-limits until the maintainer
    unlocks it. Until a JIT lands, the execution path on every platform is
    the interpreter (predecoded form from Phase 5).

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
- **Keep the README's status current.** Whenever a DESIGN.md §25 checkbox
  is ticked, a phase opens or closes, or a backend's status in the
  README table changes, update the `> **Status:**` block in `README.md`
  in the same PR. It is the first thing a newcomer reads; a stale status
  causes more confusion than no status at all.
- **README status is brief but explanatory.** Under 5 sentences,
  ideally about 3: which phase we are in, what has landed, what is next,
  and what does not work yet. Link to DESIGN.md §25 for the full list
  instead of repeating it.
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

A hit is either a bug or needs a written justification in the PR. Standing
exceptions, listed in CONTRIBUTING.md: `core/common/arena.c`'s own
`malloc()`, and hits under `core/cpu/` that name or implement the four A64
AES instructions (AESE, AESD, AESMC, AESIMC), which are guest CPU semantics,
not decryption (rule 1, DESIGN.md §1.6). Any other `aes` or `decrypt` hit
still needs its justification.

## Layout pointers

- `core/common/layout.{h,c}` — single source of truth for linear-memory
  regions (§4). All offsets come from `layout_get()`; never hardcode one.
- `core/common/vmm.{h,c}` — softmmu (§5). The only guest-memory gateway.
- `core/hle/kernel/scheduler.{h,c}` — guest green-thread scheduler (§7).
- `core/cpu/dispatch.{h,c}` — block cache + dispatcher shared by the
  predecoded interpreter and `wasm_jit` (§11). Phase 5.
- `docs/WASM_JIT.md` — `wasm_jit` specification (§10–11): the IR, region
  ABI, translation rules, invalidation, PTC. Changes to the region ABI or
  emitted code update it in the same PR.
- `docs/GPU_COMMAND_STREAM.md` — versioned CPU→GPU record format (§13).
  Any change to `command_stream.h` bumps the version and updates this doc.
- Reference implementations table: DESIGN.md §26. Behavior only; rule 2.
