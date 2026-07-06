cmake_minimum_required(VERSION 3.24)
project(Voland
  VERSION 0.0.1
  DESCRIPTION "Nintendo Switch emulator, WASM-first C core"
  LANGUAGES C)

#
# Options (see docs/DESIGN.md section 24)
#
set(CPU_BACKEND "noop" CACHE STRING "CPU backend: noop|interpreter|ballistic")
set_property(CACHE CPU_BACKEND PROPERTY STRINGS noop interpreter ballistic)

set(GPU_BACKEND "auto" CACHE STRING "GPU backend: webgpu|vulkan|metal|d3d12|auto")
set_property(CACHE GPU_BACKEND PROPERTY STRINGS webgpu vulkan metal d3d12 auto)

set(AUDIO_BACKEND "auto" CACHE STRING "Audio backend: wasapi|coreaudio|pipewire|none|auto (web: none, §14)")
set_property(CACHE AUDIO_BACKEND PROPERTY STRINGS wasapi coreaudio pipewire none auto)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Debug|Release|RelWithDebInfo" FORCE)
endif()

#
# Language settings. Core is C11 strict, no C++ anywhere in core (§3).
#
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#
# Strict warnings on the core. No exceptions-based error handling.
#
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
                      -Wmissing-prototypes -Wno-unused-parameter)
endif()

message(STATUS "Voland configuration")
message(STATUS "  CPU_BACKEND:   ${CPU_BACKEND}")
message(STATUS "  GPU_BACKEND:   ${GPU_BACKEND}")
message(STATUS "  AUDIO_BACKEND: ${AUDIO_BACKEND}")
message(STATUS "  Build type:    ${CMAKE_BUILD_TYPE}")

#
# WASM / Emscripten build flags (see docs/DESIGN.md section 24).
#
# The core module owns no memory of its own: the boot sequence (§16)
# creates ONE shared, memory64 WebAssembly.Memory and imports it in, sized
# to fit guest RAM + every layout.h region + Emscripten's own data/stack/
# heap. INITIAL_MEMORY here and TOTAL_MEMORY_PAGES in platform/web/src/
# main.ts must both equal 5_637_144_576 bytes (~5.25GiB) - growth is
# disabled, so a mismatch is an instantiation-time failure, not silent
# truncation.
#
if(EMSCRIPTEN)
  # MEMORY64/pthreads change codegen (wasm64 target, shared-memory atomics),
  # not just link behavior - they are REQUIRED on both the compile and link
  # lines, or object files come out wasm32 and linking fails outright
  # ("wasm32 object file can't be linked in wasm64 mode").
  set(VOLAND_WASM_CODEGEN_FLAGS
    -msimd128                 # REQUIRED: NEON -> WASM SIMD; ~4x slowdown without
    -sMEMORY64=1               # REQUIRED for Switch 1, not just Switch 2 (§4)
    -sSHARED_MEMORY=1
    -sUSE_PTHREADS=1
  )
  add_compile_options(${VOLAND_WASM_CODEGEN_FLAGS})
  add_link_options(
    ${VOLAND_WASM_CODEGEN_FLAGS}
    -sWASM=1
    -sWASM_BIGINT=1
    -sIMPORTED_MEMORY=1        # host creates the single WebAssembly.Memory (§16)
    -sINITIAL_MEMORY=5637144576   # ~5.25GB; must equal the host's initial
    -sMAXIMUM_MEMORY=5637144576   # == INITIAL: growth disabled, views never detach
    -sALLOW_MEMORY_GROWTH=0
    -sPTHREAD_POOL_SIZE=8
    -sMODULARIZE=1             # emits a factory function, not a self-executing script
    -sEXPORT_ES6=1             # ...as an ES module, so cpu.worker.ts can `import()` it
    -sEXPORT_NAME=createVolandCoreModule
    -sENVIRONMENT=worker       # only ever instantiated inside cpu.worker.ts
    "-sEXPORTED_FUNCTIONS=['_emulator_create_ffi','_emulator_destroy_ffi',
                            '_emulator_run_ffi','_emulator_step_ffi',
                            '_layout_get_ffi','_cpu_backend_id_ffi',
                            '_cpu_get_reg_ffi','_cpu_set_reg_ffi','_cpu_get_pc_ffi']"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
  )
endif()

add_subdirectory(core)

#
# Optional: tests (Phase 0 ships with a smoke test only)
#
option(SWITCH_EMU_BUILD_TESTS "Build test binaries" ON)
if(SWITCH_EMU_BUILD_TESTS AND NOT EMSCRIPTEN)
  enable_testing()
  add_subdirectory(tests)
endif()
