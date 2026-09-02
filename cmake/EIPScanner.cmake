# SPDX-License-Identifier: Apache-2.0
#
# Builds the vendored EIPScanner (nimbuscontrols) for the EtherNet/IP Scanner
# (originator) adapter process.
#
# Note the contrast with cmake/OpENer.cmake, which compiles upstream's sources
# directly. That was forced: OpENer ships an application layer whose symbols
# collide with ours. EIPScanner is a plain library with no application
# call-backs to collide with, so add_subdirectory() is used here and upstream
# keeps ownership of its own build. Fewer things for us to restate, and a
# submodule bump stays a pointer change.

set(EIPSCANNER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/EIPScanner)

if(NOT EXISTS ${EIPSCANNER_ROOT}/src/ConnectionManager.h)
  message(FATAL_ERROR
    "EIPScanner sources are missing. Run:\n"
    "  git submodule update --init --recursive\n"
    "or configure with -DSOFTPLC_WITH_EIPSCANNER=OFF to build the scanner "
    "adapter with its mirror backend.")
endif()

# Upstream's options, forced off: we want the library and nothing else.
set(TEST_ENABLED    OFF CACHE BOOL "" FORCE)
set(EXAMPLE_ENABLED OFF CACHE BOOL "" FORCE)
# Vendor sources are Rockwell/Yaskawa device-specific helpers we do not use.
set(ENABLE_VENDOR_SRC OFF CACHE BOOL "" FORCE)

add_subdirectory(${EIPSCANNER_ROOT} ${CMAKE_BINARY_DIR}/eipscanner EXCLUDE_FROM_ALL)

# Upstream only does a bare include_directories(), so the include path is not
# carried on the target. Attach it here rather than leaking a global include.
target_include_directories(EIPScannerS INTERFACE ${EIPSCANNER_ROOT}/src)

# Upstream code, not ours: do not hold it to this project's warning settings.
target_compile_options(EIPScannerS PRIVATE -w)

message(STATUS "  EIPScanner sources  : ${EIPSCANNER_ROOT}/src")
