# smoke_phase0 is the cross-subsystem integration path (emulator/cpu/hle/
# vmm wired together); layout_test and vmm_test are common/layout's (§4)
# and common/vmm's (§5) own unit tests. Further per-subsystem tests land
# alongside the subsystems they cover (CLAUDE.md: "every C subsystem
# ships tests in tests/").

add_executable(smoke_phase0 smoke_phase0.c)
target_link_libraries(smoke_phase0 PRIVATE switch_core)
add_test(NAME smoke_phase0 COMMAND smoke_phase0)

add_executable(layout_test layout_test.c)
target_link_libraries(layout_test PRIVATE switch_core)
add_test(NAME layout_test COMMAND layout_test)

add_executable(vmm_test vmm_test.c)
target_link_libraries(vmm_test PRIVATE switch_core)
add_test(NAME vmm_test COMMAND vmm_test)

#
# core/hle/loader (§1.6, §12): one unit test per parser over synthesized
# images from loader_fixtures.c, plus loader_smoke for the composed
# bootstrap path (NCA -> ExeFS -> npdm, NCA -> ExeFS -> NSO -> segment,
# NCA -> RomFS -> file).
#
add_library(loader_fixtures STATIC loader_fixtures.c)
target_include_directories(loader_fixtures PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# NSO fixtures compress segments with the vendored LZ4 (core/third_party).
target_link_libraries(loader_fixtures PRIVATE switch_core)

foreach(loader_test byte_source_test exefs_test romfs_test npdm_test nca_parse_test nso_test loader_smoke)
  add_executable(${loader_test} ${loader_test}.c)
  target_link_libraries(${loader_test} PRIVATE switch_core loader_fixtures)
  add_test(NAME ${loader_test} COMMAND ${loader_test})
endforeach()

#
# core/hle/kernel (§12 process bootstrap): the page allocator and address
# space are pure units; process_test drives the bootstrap over a
# synthesized ExeFS and asserts through vmm.
#
add_executable(page_allocator_test page_allocator_test.c)
target_link_libraries(page_allocator_test PRIVATE switch_core)
add_test(NAME page_allocator_test COMMAND page_allocator_test)

add_executable(address_space_test address_space_test.c)
target_link_libraries(address_space_test PRIVATE switch_core)
add_test(NAME address_space_test COMMAND address_space_test)

add_executable(process_test process_test.c)
target_link_libraries(process_test PRIVATE switch_core loader_fixtures)
add_test(NAME process_test COMMAND process_test)

#
# core/hle/kernel TLS + threads (§12 "TLS is the transport"): tls_test
# drives the block allocator over a real vmm; thread_test arms threads
# through the noop backend and asserts the register file and
# tpidrro_el0 through the backend interface.
#
add_executable(tls_test tls_test.c)
target_link_libraries(tls_test PRIVATE switch_core)
add_test(NAME tls_test COMMAND tls_test)

add_executable(thread_test thread_test.c)
target_link_libraries(thread_test PRIVATE switch_core)
add_test(NAME thread_test COMMAND thread_test)

#
# core/hle/kernel/svc_memory (§25 checkbox 5): SVC-path tests over a real
# process_bootstrap()'d Process, driven through hle_on_svc the way
# thread_test drives thread SVCs.
#
add_executable(svc_memory_test svc_memory_test.c)
target_link_libraries(svc_memory_test PRIVATE switch_core loader_fixtures)
add_test(NAME svc_memory_test COMMAND svc_memory_test)
