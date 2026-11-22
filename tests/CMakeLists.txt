# SPDX-License-Identifier: GPL-2.0-only
# ---- unit tests: pure logic, no hardware needed ----
foreach(t test_common test_wire test_lifecycle test_topology test_coherence test_kernel_view)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/${t}.cc)
    add_executable(${t} unit/${t}.cc)
    target_link_libraries(${t} PRIVATE dnuma_coord dnuma_core)
    add_test(NAME ${t} COMMAND ${t})
  endif()
endforeach()

# ---- integration programs: distributed applications driven by the scripts
#      in tests/integration and tests/failure (they need both machines) ----
if(TARGET dnuma)
  foreach(t t_memory t_exec t_coherence t_worker t_migrate_abort t_peers)
    dnuma_add_executable(${t} integration/progs/${t}.cc)
    set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  endforeach()
endif()

# ---- ordinary native programs (no dnuma library) used by the process-export tests ----
enable_language(C)
foreach(t n_spin n_readmostly)
  add_executable(${t} integration/progs/${t}.c)
  set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests COMPILE_OPTIONS "-O2")
endforeach()

# Run inside a disposable kernel with CONFIG_DNUMA_NATIVE_PAGER.
add_executable(n_native_pager integration/progs/n_native_pager.cc)
target_link_libraries(n_native_pager PRIVATE dnuma_coord Threads::Threads)

add_executable(n_thread_pair integration/progs/n_thread_pair.c)
target_link_libraries(n_thread_pair PRIVATE Threads::Threads)
add_test(NAME native_thread_pair_local COMMAND n_thread_pair)
set_tests_properties(native_thread_pair_local PROPERTIES SKIP_RETURN_CODE 77)
