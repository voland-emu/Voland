cmake_minimum_required(VERSION 3.16)
project(dynarmic_bench CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Disable vcpkg if it was set in the environment - all deps are vendored
# inside dynarmic's own externals/ directory, vcpkg only causes conflicts
set(CMAKE_TOOLCHAIN_FILE "" CACHE STRING "" FORCE)

#  Dynarmic (vendored submodule)
# Point this at wherever you cloned dynarmic.
# Default assumes: Voland/externals/dynarmic
set(DYNARMIC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../externals/dynarmic"
    CACHE PATH "Path to dynarmic source root")

if(NOT EXISTS "${DYNARMIC_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR
    "dynarmic not found at ${DYNARMIC_ROOT}\n"
    "Run: git submodule add https://github.com/merryhime/dynarmic externals/dynarmic\n"
    "     git submodule update --init --recursive"
  )
endif()

# Suppress dynarmic's -Werror so it builds on GCC 14 / current toolchains.
# dynarmic is archived and will never fix the GCC 14 stringop-overread false positive.
add_compile_options(-Wno-error=stringop-overread -Wno-stringop-overread)

add_subdirectory(${DYNARMIC_ROOT} dynarmic_build EXCLUDE_FROM_ALL)

#  Benchmark executable
add_executable(bench bench.cpp)

target_link_libraries(bench PRIVATE dynarmic)

target_include_directories(bench PRIVATE
  ${DYNARMIC_ROOT}/include
)

# -O2 so the benchmark loop itself does not dominate measurements.
# -g so the profiler can resolve symbols.
target_compile_options(bench PRIVATE -O2 -g)
