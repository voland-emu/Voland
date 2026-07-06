# Builds the platform-agnostic C11 emulator core. See docs/DESIGN.md section 2.

set(SWITCH_CORE_SOURCES
  common/log.c
  common/arena.c
  common/layout.c
  cpu/cpu.c
  cpu/backends/noop/noop.c
  hle/hle.c
  emulator.c
)

set(SWITCH_CORE_PUBLIC_HEADERS
  common/log.h
  common/arena.h
  common/assert.h
  common/result.h
  common/layout.h
  common/vmm.h
  cpu/cpu.h
  cpu/backends/noop/noop.h
  hle/hle.h
  emulator.h
)

add_library(switch_core STATIC ${SWITCH_CORE_SOURCES})

target_include_directories(switch_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

#
# Backend selection. Every backend obeys CPU_Backend; a single define wires
# cpu_get_active_backend() to the compiled-in choice. Additional backends
# (interpreter, ballistic) will add their own subdirectory and define.
#
if(CPU_BACKEND STREQUAL "noop")
  target_compile_definitions(switch_core PUBLIC SWITCH_CPU_BACKEND_NOOP=1)
elseif(CPU_BACKEND STREQUAL "interpreter")
  message(FATAL_ERROR "interpreter backend is a Phase 2 deliverable, not yet built")
elseif(CPU_BACKEND STREQUAL "ballistic")
  message(FATAL_ERROR "ballistic backend is a Phase 5 deliverable, not yet built (§10)")
else()
  message(FATAL_ERROR "Unknown CPU_BACKEND '${CPU_BACKEND}'")
endif()

#
# Web build: expose the core as a WASM module directly from this target.
#
if(EMSCRIPTEN)
  add_executable(switch_core_wasm stubs/wasm_entry.c)
  target_link_libraries(switch_core_wasm PRIVATE switch_core)
  set_target_properties(switch_core_wasm PROPERTIES
    OUTPUT_NAME "switch_core"
    SUFFIX ".js"
  )
endif()
