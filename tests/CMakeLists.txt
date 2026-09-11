# SPDX-License-Identifier: Apache-2.0

function(softplc_add_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE ${SOFTPLC_ADAPTERS_LINK})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

softplc_add_test(test_spsc_ring)
softplc_add_test(test_shm_ownership)
softplc_add_test(test_process_image)
softplc_add_test(test_std_fb)
softplc_add_test(test_adapter_contract)
softplc_add_test(test_plc_config)
softplc_add_test(test_scan_overrun)

# The ladder logic itself. Needs the examples dir for the POU it exercises.
add_executable(test_demo_program test_demo_program.c
               ${CMAKE_SOURCE_DIR}/examples/demo_program.c)
target_link_libraries(test_demo_program PRIVATE ${SOFTPLC_ADAPTERS_LINK})
target_include_directories(test_demo_program PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/examples)
add_test(NAME test_demo_program COMMAND test_demo_program)
set_tests_properties(test_demo_program PROPERTIES TIMEOUT 60)

if(SOFTPLC_WITH_EIP_SCANNER)
  softplc_add_test(test_scanner_config)
endif()

if(SOFTPLC_WITH_EIP_SCANNER)
  # Forks and execs the real scanner binary, so it needs it built first.
  add_executable(test_eip_scanner_ipc test_eip_scanner_ipc.c)
  target_link_libraries(test_eip_scanner_ipc PRIVATE ${SOFTPLC_ADAPTERS_LINK})
  target_include_directories(test_eip_scanner_ipc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(test_eip_scanner_ipc PRIVATE
    EIP_SCANNER_PATH="$<TARGET_FILE:softplc-eip-scanner>"
    SOFTPLC_SCANNER_MIRROR_BACKEND=$<NOT:$<BOOL:${SOFTPLC_WITH_EIPSCANNER}>>)
  add_dependencies(test_eip_scanner_ipc softplc-eip-scanner)
  add_test(NAME test_eip_scanner_ipc COMMAND test_eip_scanner_ipc)
  set_tests_properties(test_eip_scanner_ipc PROPERTIES TIMEOUT 120)
endif()

if(SOFTPLC_WITH_EIP)
  # These fork and exec the real adapter binary, so they need it built first
  # and need to know where it landed.
  foreach(t test_eip_ipc test_eip_failsafe_clear test_eip_data_invalid test_abi_skew)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE ${SOFTPLC_ADAPTERS_LINK})
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${t} PRIVATE
      EIP_ADAPTER_PATH="$<TARGET_FILE:softplc-eip-adapter>"
      # Only the mirror backend makes the returned payload predictable; with
      # the real stack the inputs come from the consumed assembly.
      SOFTPLC_EIP_MIRROR_BACKEND=$<NOT:$<BOOL:${SOFTPLC_WITH_OPENER}>>)
    add_dependencies(${t} softplc-eip-adapter)
    add_test(NAME ${t} COMMAND ${t})
    # Each test namespaces its own /dev/shm objects by pid, so they are safe
    # to run concurrently.
    set_tests_properties(${t} PROPERTIES TIMEOUT 120)
  endforeach()
endif()

# `softplc --status` is the health probe D1 added; it forks the real binary,
# so it needs it built and needs to know where it landed.
add_executable(test_status_json test_status_json.c)
target_link_libraries(test_status_json PRIVATE ${SOFTPLC_ADAPTERS_LINK})
target_include_directories(test_status_json PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_status_json PRIVATE
  SOFTPLC_PATH="$<TARGET_FILE:softplc>")
add_dependencies(test_status_json softplc)
add_test(NAME test_status_json COMMAND test_status_json)
set_tests_properties(test_status_json PROPERTIES TIMEOUT 60)
