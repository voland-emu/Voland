# Voland

A Nintendo Switch emulator targeting the web as a primary platform, with native apps on iOS, Android, macOS, tvOS, Windows, Linux, and Xbox.

Play Switch games in your browser. No installation. No setup beyond providing your own keys and games.

> **Status:** Early development — **Phase 1 (Load & Memory)** of the plan in [DESIGN.md §25](docs/DESIGN.md#25-development-phases). Phase 0 (skeleton) is complete: the linear-memory layout, no-op CPU backend, SVC/HLE dispatcher stub, and web boot path (single shared memory, COOP/COEP, Workers) all build and pass CI natively and under Emscripten. The softmmu (`core/common/vmm`), the decrypted-NCA parsers (`core/hle/loader`: NCA container, ExeFS, RomFS, npdm — pre-decrypted input only, per §1.6), the NSO loader (LZ4 via vendored `core/third_party/lz4`), the process bootstrap (`core/hle/kernel`: NSOs mapped through vmm at an ASLR'd base, Horizon region layout, main-thread stack and entry ABI — `emulator_load_program` runs the whole path) and TLS allocation with `tpidrro_el0` plumbing (`tls.{h,c}` per-process block allocator, `thread.{h,c}` thread objects) have landed; next are memory HLE (SetHeapSize/MapMemory/GetInfo over that layout) and the minimal IPC + sm: stub. Nothing executes guest code yet — the interpreter is a Phase 2 goal.

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
  cpu/          ← abstract backend interface + no-op, interpreter, dynarmic, ballistic
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

recompiler/     ← Ballistic (git submodule)
```

Full architecture documentation: [DESIGN.md](docs/DESIGN.md)

---

## Recompiler

The primary recompiler target is [Ballistic](https://github.com/pound-emu/ballistic), a C rewrite of dynarmic currently in early development. Ballistic targets the specific performance bottlenecks in dynarmic - JIT state cache line overflow, block eviction pathology, heavy IR memory footprint, and missing peephole optimisation.

The CPU backend vtable means Voland is not blocked on Ballistic. Current backend status:

| Backend | Status | Notes |
|---|---|---|
| noop | Complete | Default - everything except game execution is testable |
| interpreter | Planned | Slow but correct, unblocks homebrew |
| dynarmic | Planned | Interim desktop backend - archived upstream, requires patching for GCC 14 |
| ballistic | Planned | Primary target, wired in when instruction coverage is sufficient |

The WASM JIT pipeline - ARM bytecode → WASM bytecode → `WebAssembly.compile()` → cached module - is architecturally proven. Ballistic providing a WASM bytecode emission backend makes Voland the only Switch emulator that runs natively in a browser, on iOS without App Store restrictions, and on Xbox UWP. No other ARM recompiler targets this deployment surface.

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

# Dynarmic - desktop only, requires GCC 13 or patched GCC 14
cmake -B build -DCPU_BACKEND=dynarmic

# Ballistic - when available
cmake -B build -DCPU_BACKEND=ballistic
```

---

## Contributing

### Where to start

**If you know TypeScript / JavaScript:** The web platform layer is the right entry point. The CPU backend is a no-op stub - the entire frontend, storage layer, Worker architecture, and Service Worker can be built and tested without any emulation knowledge. Start with `platform/web/`.

**If you know C:** The HLE service layer needs contributors. Pick any unimplemented service from the priority list in [DESIGN.md](docs/DESIGN.md#8-hle-service-layer) and implement it in C. Study [Ryubing](https://github.com/Ryubing) as a reference for service behaviour - reimplement in C, do not copy code.

**If you know compiler engineering:** The highest-leverage contribution is the Ballistic WASM bytecode emission backend. Join the [Pound Discord](https://discord.gg/aMmTmKsVC7) and engage on IR design before it solidifies. The three requirements are documented in [DESIGN.md](docs/DESIGN.md#7-ballistic-integration-plan).

### Standards

- C11 for core - no C++ except the dynarmic wrapper
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
| [Emscripten Relooper](https://github.com/WebAssembly/binaryen/blob/main/src/cfg/Relooper.h) | CFG → structured control flow |

---

## Proof of concept

`poc/` contains three standalone demonstrations built to validate the architecture and make the case for Ballistic WASM support:

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

- [Ballistic](https://github.com/pound-emu/ballistic) - the planned primary recompiler
- [Ryubing](https://github.com/Ryubing) - HLE reference implementation
- [dynarmic](https://github.com/merryhime/dynarmic) - interim recompiler and benchmark baseline
- The Switch homebrew and emulation community for reverse engineering documentation
