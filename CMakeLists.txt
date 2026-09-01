# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.18)
project(soft_program_app LANGUAGES C VERSION 0.1.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # POSIX shm/sem and clock_nanosleep

option(SOFTPLC_WITH_EIP     "Build the EtherNet/IP Adapter (target) protocol adapter" ON)
option(SOFTPLC_WITH_OPENER  "Link the real OpENer stack into the EtherNet/IP adapter" OFF)
option(SOFTPLC_WITH_EIP_SCANNER "Build the EtherNet/IP Scanner (originator) adapter" ON)
option(SOFTPLC_WITH_EIPSCANNER  "Link the real EIPScanner stack into the Scanner adapter" OFF)
option(SOFTPLC_BUILD_TESTS  "Build the test suite" ON)
option(SOFTPLC_WERROR       "Treat warnings as errors" OFF)

add_compile_options(
  -Wall -Wextra -Wpedantic
  -Wshadow -Wcast-qual
  -Wvla -Wpointer-arith
  # Prototype warnings are C-only; applying them to the scanner's C++ backend
  # would just emit "not valid for C++" on every compile.
  $<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>
  $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
)
if(SOFTPLC_WERROR)
  add_compile_options(-Werror)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

# --------------------------------------------------------------------------
# softplc_core - the IEC 61131-3 runtime and the adapter abstraction.
# Nothing protocol-specific may be linked in here.
# --------------------------------------------------------------------------
add_library(softplc_core STATIC
  src/core/plc_types.c
  src/core/plc_log.c
  src/core/process_image.c
  src/core/std_fb.c
  src/core/protocol_adapter.c
  src/core/adapter_registry.c
  src/core/plc_runtime.c
  src/ipc/spsc_ring.c
  src/ipc/shm.c
)
target_include_directories(softplc_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(softplc_core PUBLIC Threads::Threads rt)
target_compile_definitions(softplc_core PUBLIC
  SOFTPLC_WITH_EIP=$<BOOL:${SOFTPLC_WITH_EIP}>
  SOFTPLC_WITH_EIP_SCANNER=$<BOOL:${SOFTPLC_WITH_EIP_SCANNER}>
)

# --------------------------------------------------------------------------
# softplc_adapters - the pluggable protocol layer.
#
# Split from softplc_core so the core has no link-time dependency on any
# protocol.  The EtherNet/IP adapter contributes only its PLC-side proxy here;
# the stack itself lives in a separate executable, and that split is the
# crash-containment boundary.
# --------------------------------------------------------------------------
add_library(softplc_adapters STATIC
  src/adapters/builtins.c
  src/adapters/loopback/loopback_adapter.c
)
target_link_libraries(softplc_adapters PUBLIC softplc_core)

if(SOFTPLC_WITH_EIP)
  target_sources(softplc_adapters PRIVATE
    src/adapters/eip/eip_adapter_proxy.c
    src/adapters/eip/eip_names.c
  )
  target_include_directories(softplc_adapters PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/eip
  )
endif()

if(SOFTPLC_WITH_EIP_SCANNER)
  target_sources(softplc_adapters PRIVATE
    src/adapters/eip_scanner/eip_scanner_proxy.c
    src/adapters/eip_scanner/eip_scanner_names.c
    src/adapters/eip_scanner/eip_scanner_config.c
  )
  target_include_directories(softplc_adapters PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/eip_scanner
  )
endif()

# The factories are referenced only from builtins.c, and builtins.c only by
# name from an executable, so plain static linking would let the linker drop
# the objects that define them.
set(SOFTPLC_ADAPTERS_LINK
  -Wl,--whole-archive softplc_adapters -Wl,--no-whole-archive softplc_core)

# --------------------------------------------------------------------------
# PLC core executable
# --------------------------------------------------------------------------
add_executable(softplc
  src/app/softplc_main.c
  examples/demo_program.c
)
target_include_directories(softplc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples)
target_link_libraries(softplc PRIVATE ${SOFTPLC_ADAPTERS_LINK})

# --------------------------------------------------------------------------
# EtherNet/IP adapter process
# --------------------------------------------------------------------------
if(SOFTPLC_WITH_EIP)
  if(SOFTPLC_WITH_OPENER)
    include(cmake/OpENer.cmake)
    set(EIP_BACKEND_SRC src/adapters/eip/eip_backend_opener.c)
  else()
    set(EIP_BACKEND_SRC src/adapters/eip/eip_backend_loopback.c)
  endif()

  add_executable(softplc-eip-adapter
    src/adapters/eip/eip_adapter_main.c
    src/adapters/eip/eip_names.c
    ${EIP_BACKEND_SRC}
  )
  target_include_directories(softplc-eip-adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/eip
  )
  target_link_libraries(softplc-eip-adapter PRIVATE softplc_core)

  if(SOFTPLC_WITH_OPENER)
    target_link_libraries(softplc-eip-adapter PRIVATE opener)
    target_compile_definitions(softplc-eip-adapter PRIVATE SOFTPLC_WITH_OPENER=1)
    # The backend includes OpENer's headers, which predate some of the
    # warnings this project enables.  Relax only the ones upstream trips, and
    # only for the file that includes them.
    set_source_files_properties(src/adapters/eip/eip_backend_opener.c
      PROPERTIES COMPILE_OPTIONS "-Wno-strict-prototypes")
  endif()
endif()

# --------------------------------------------------------------------------
# EtherNet/IP Scanner process
#
# Its service loop is C, not C++, even though the stack is C++20: that file
# owns the _Atomic rings, and keeping it in C means the two languages never
# have to agree on an atomic's layout. Only the backend TU is C++.
# --------------------------------------------------------------------------
if(SOFTPLC_WITH_EIP_SCANNER)
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  if(SOFTPLC_WITH_EIPSCANNER)
    include(cmake/EIPScanner.cmake)
    set(SCANNER_BACKEND_SRC src/adapters/eip_scanner/eip_scanner_backend_eipscanner.cpp)
  else()
    set(SCANNER_BACKEND_SRC src/adapters/eip_scanner/eip_scanner_backend_mirror.cpp)
  endif()

  add_executable(softplc-eip-scanner
    src/adapters/eip_scanner/eip_scanner_main.c
    src/adapters/eip_scanner/eip_scanner_names.c
    src/adapters/eip_scanner/eip_scanner_config.c
    ${SCANNER_BACKEND_SRC}
  )
  target_include_directories(softplc-eip-scanner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/eip_scanner
  )
  target_link_libraries(softplc-eip-scanner PRIVATE softplc_core)

  if(SOFTPLC_WITH_EIPSCANNER)
    target_link_libraries(softplc-eip-scanner PRIVATE EIPScannerS)
    target_compile_definitions(softplc-eip-scanner PRIVATE SOFTPLC_WITH_EIPSCANNER=1)
    # The backend includes upstream headers; relax only what they trip, and
    # only for that file.
    set_source_files_properties(${SCANNER_BACKEND_SRC}
      PROPERTIES COMPILE_OPTIONS "-Wno-shadow;-Wno-cast-qual")
  endif()
endif()

# --------------------------------------------------------------------------
# Phase 0 measurement harness. Not part of the shipped image; built with the
# tests so it cannot rot.
# --------------------------------------------------------------------------
if(SOFTPLC_BUILD_TESTS AND SOFTPLC_WITH_EIP)
  add_executable(bench_exchange tools/bench_exchange.c)
  target_link_libraries(bench_exchange PRIVATE ${SOFTPLC_ADAPTERS_LINK})
  add_dependencies(bench_exchange softplc-eip-adapter)
endif()

# --------------------------------------------------------------------------
if(SOFTPLC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "soft-program-app ${PROJECT_VERSION}")
message(STATUS "  EtherNet/IP adapter : ${SOFTPLC_WITH_EIP}")
message(STATUS "  OpENer stack linked : ${SOFTPLC_WITH_OPENER}")
message(STATUS "  EtherNet/IP scanner : ${SOFTPLC_WITH_EIP_SCANNER}")
message(STATUS "  EIPScanner linked   : ${SOFTPLC_WITH_EIPSCANNER}")
message(STATUS "  tests               : ${SOFTPLC_BUILD_TESTS}")
