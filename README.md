# Voland

A Nintendo Switch emulator targeting the web as a primary platform, with native apps on iOS, Android, macOS, tvOS, Windows, Linux, and Xbox.

Play Switch games in your browser. No installation. No setup beyond providing your own keys and games.

> **Status:** Early development: **Phase 1 (Load & Memory)** of [DESIGN.md §25](docs/DESIGN.md#25-development-phases). Phase 0 and most of Phase 1 have landed and pass CI natively and under Emscripten: the softmmu, decrypted-NCA and NSO loading with process bootstrap, TLS, and the memory SVCs. Next is the minimal IPC + `sm:` stub. Nothing executes guest code yet: the interpreter arrives in Phase 2, and the web JIT (now Voland's own ARM64→WASM translator, `wasm_jit`, since Ballistic's WASM backend fell through) is designed in [WASM_JIT.md](docs/WASM_JIT.md) and lands in Phase 5.

---

## Why web-first

Every existing Switch emulator requires installation, driver configuration, and technical setup that eliminates most potential users before they ever reach a game. Voland's primary target is a browser tab - click a link, provide your decrypted game files, play.

The web version is not a port of a native emulator. It is designed from the ground up for the browser, using a WASM-compiled C core, WebGPU rendering, SharedArrayBuffer for guest RAM, and a dedicated Worker per emulator subsystem.

Native apps on iOS, Android, macOS, tvOS, Windows, Linux, and Xbox share the same C core with thin platform-specific UI and renderer layers on top.

---

## Requirements

To play games you must provide:

- **prod.keys** - cryptographic keys dumped from your own Nintendo Switch using [Lockpick_RCM](https://github.com/s1204IT/Lockpick_RCM)
- **Game files** - NSP or XCI files dumped from cartridges or digital purchases you own

Voland does not and will never distribute keys, firmware, or game files. These must come from hardware you own. See the [dumping guide](docs/DUMP.md) for instructions.

---

## Architecture

A single C11 core compiles to every target platform via CMake. The CPU backend is an abstract vtable - the recompiler slots in without touching anything else.

```
core/
  cpu/          ← abstract backend interface + no-op, interpreter, wasm_jit (web), optional ballistic (desktop)
  hle/          ← Switch OS service implementations
  gpu/          ← Maxwell GPU emulation
  audio/        ← DSP emulation and audio pipeline
  common/       ← arena allocator, ring buffer, trace buffer, logging

platform/
  web/          ← Solid.js frontend, Workers, Service Worker, WebGPU
  ios/          ← SwiftUI + Metal
  android/      ← Jetpack Compose + Vulkan
  macos/        ← SwiftUI + Metal
  tvos/         ← SwiftUI + Metal + focus engine
  windows/      ← Qt + Vulkan/D3D12
  linux/        ← Qt + Vulkan
  xbox/         ← WinUI 2 + D3D12 (Developer Mode)

recompiler/     ← Ballistic (git submodule, optional desktop backend)
```

Full architecture documentation: [DESIGN.md](docs/DESIGN.md)

---

## Recompiler

The primary recompiler target is `wasm_jit`, Voland's own ARM64→WASM translator, planned for Phase 5 ([DESIGN.md §10](docs/DESIGN.md#10-jit-backends); specification in [WASM_JIT.md](docs/WASM_JIT.md)). It translates hot guest code into WebAssembly modules that the browser compiles to native code, and shares its decoder and instruction format with the interpreter. [Ballistic](https://github.com/pound-emu/ballistic), a C ARM64 recompiler with an x86 backend, is an optional desktop backend consumed unmodified. Its WASM backend never materialized, which is why Voland builds its own.

The CPU backend vtable means Voland is not blocked on any recompiler. Current backend status:

| Backend | Status | Notes |
|---|---|---|
| noop | Complete | Default - everything except game execution is testable |
| interpreter | Planned (Phase 2; predecoded form in Phase 5) | Slow but correct, unblocks homebrew; the execution path on every platform until a JIT lands |
| wasm_jit | Designed, Phase 5 | Primary target on web - ARM64 → WASM translation of hot code regions |
| ballistic | Optional | Desktop x86 only, wired in when upstream instruction coverage is sufficient |

The browser side of the pipeline - WASM bytes → `WebAssembly.compile()` → instantiate against the core's shared memory and function table - is proven ([`poc/02_wasm_pipeline`](poc/), and the probe recorded in WASM_JIT.md); `wasm_jit` is the translator that feeds it.

---

## Platform targets

| Platform | UI | Renderer | Distribution |
|---|---|---|---|
| Web | Solid.js | WebGPU | URL / PWA |
| iOS | SwiftUI | Metal | Sideload |
| iPadOS | SwiftUI | Metal | Sideload |
| tvOS | SwiftUI + focus engine | Metal | Sideload |
| visionOS | SwiftUI + RealityKit | Metal | Sideload |
| Android | Jetpack Compose | Vulkan | APK sideload |
| Android TV | Jetpack Compose | Vulkan | APK sideload |
| macOS | SwiftUI | Metal | Direct download |
| Windows | Qt | Vulkan / D3D12 | Direct download |
| Linux | Qt | Vulkan | Direct download |
| Xbox | WinUI 2 | D3D12 | Developer Mode |

---

## Building

### Dependencies

- CMake 3.16+
- C11 compiler (GCC 12+, Clang 15+, MSVC 2022+)
- For web: Emscripten SDK ≥6.0.9 (see [`global.json`](global.json) for all minimum tool versions)
- For dynarmic backend: see `externals/dynarmic/`

### Web (requires Emscripten)

```bash
git clone https://github.com/Voland-emu/Voland
cd Voland
git submodule update --init --recursive

emcmake cmake -B build/web \
  -DCMAKE_BUILD_TYPE=Release \
  -DCPU_BACKEND=noop \
  -DGPU_BACKEND=webgpu \
  -DPLATFORM=web

cmake --build build/web -j$(nproc)
```

### Desktop

```bash
cmake -B build/desktop \
  -DCMAKE_BUILD_TYPE=Release \
  -DCPU_BACKEND=noop \
  -DPLATFORM=desktop

cmake --build build/desktop -j$(nproc)
```

### Backend selection

```bash
# No-op (default) - for frontend and HLE development
cmake -B build -DCPU_BACKEND=noop

# Interpreter - slow but executes ARM code
cmake -B build -DCPU_BACKEND=interpreter

# wasm_jit - web build only, Phase 5
emcmake cmake -B build-web -DCPU_BACKEND=wasm_jit

# Ballistic - optional desktop backend, once unlocked
cmake -B build -DCPU_BACKEND=ballistic
```

---

## Contributing

### Where to start

**If you know TypeScript / JavaScript:** The web platform layer is the right entry point. The CPU backend is a no-op stub - the entire frontend, storage layer, Worker architecture, and Service Worker can be built and tested without any emulation knowledge. Start with `platform/web/`.

**If you know C:** The HLE service layer needs contributors. Pick any unimplemented service from the priority list in [DESIGN.md §12](docs/DESIGN.md#12-hle-service-layer) and implement it in C. Study [Ryubing](https://github.com/Ryubing) as a reference for service behaviour - reimplement in C, do not copy code.

**If you know compiler engineering:** The highest-leverage contribution is `wasm_jit`, Voland's ARM64 → WASM translator. It opens in Phase 5; its specification is [WASM_JIT.md](docs/WASM_JIT.md) and its requirements are in [DESIGN.md §10](docs/DESIGN.md#10-jit-backends).

### Standards

- C11 for core - no C++
- TypeScript strict mode - no `any`, no implicit returns
- No third-party dependencies not listed in DESIGN.md
- HLE services must be validated against known-good game output before merge
- No keys, firmware, or game files in the repository or commit history - ever

### Reference implementations

These are studied as reference, not forked:

| Project | What to study |
|---|---|
| [Ryubing](https://github.com/Ryubing) | HLE service implementations |
| yuzu (archived) | GPU emulation approach |
| dynarmic | ARM instruction semantics and test cases |
| [Ballistic](https://github.com/pound-emu/ballistic) | Tier-1 ARM64 translation techniques - ideas only, never code |
| [Emscripten Relooper](https://github.com/WebAssembly/binaryen/blob/main/src/cfg/Relooper.h) | CFG → structured control flow |

---

## Proof of concept

`poc/` contains three standalone demonstrations built to validate the architecture and make the case for Ballistic WASM support. They are historical: Ballistic's WASM backend never materialized, and Voland now builds its own (`wasm_jit`, [DESIGN.md §10](docs/DESIGN.md#10-jit-backends)).

**`01_dynarmic_bottlenecks/`** - a synthetic ARM64 benchmark fed directly to dynarmic with no ROM, no HLE, no game. Measures the three performance pathologies documented in Ballistic's own README: JIT state cache line overflow, prologue/epilogue overhead per Run() call, and block eviction cost under cache invalidation.

Note: dynarmic is archived and no longer builds cleanly on GCC 14 without patching. This is expected behaviour from an unmaintained codebase and is itself evidence for why an actively maintained alternative matters.

**`02_wasm_pipeline/`** - a single self-contained HTML file. Open in Chrome. Hand-crafted WASM bytecode for an ARM ADD instruction is passed to `WebAssembly.compile()`, instantiated, and executed. Proves the browser-side JIT pipeline works today with no build system and no dependencies.

**`03_backend_interface/`** - the CPU backend vtable in C, a no-op implementation, a dynarmic wrapper, and a Ballistic stub. Shows exactly where Ballistic plugs in and what it needs to implement.

---

## Legal

Voland is open source software released under the [GPL-2.0 license](LICENSE).

Voland does not include, distribute, or facilitate obtaining Nintendo's copyrighted material. Game files must be dumped and decrypted by the user, with separate tools, from hardware they own; Voland accepts only the resulting decrypted NCA files and never consumes keys or firmware (see [DESIGN.md §1.6](docs/DESIGN.md#16-legal-scope-boundaries)). Forks or distributions that bundle Nintendo's IP are not affiliated with this project and are solely responsible for their own legal compliance.

Voland is not affiliated with Nintendo Co., Ltd.

---

## About the name

Voland is named for Völundr (Old Norse, also Wayland the Smith in Anglo-Saxon tradition) — the legendary craftsman of Germanic and Norse mythology, master of the forge.

The name has a second well-known association: Voland is also Mikhail Bulgakov's name for the devil in *The Master and Margarita*, drawn from Goethe's *Faust*. The Norse origin is the substantive reason for the name; the literary echo is welcome.

## Acknowledgements

- [Ballistic](https://github.com/pound-emu/ballistic) - optional desktop backend and a source of ideas for `wasm_jit`
- [Ryubing](https://github.com/Ryubing) - HLE reference implementation
- [dynarmic](https://github.com/merryhime/dynarmic) - ARM semantics reference and benchmark baseline
- The Switch homebrew and emulation community for reverse engineering documentation
