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

# --------------------------------------------------------------------------
# Vendored fix, applied to the submodule working tree at configure time.
#
# IOConnection's send timer loses real time twice over, which showed up as an
# O->T period of 11 982 us against a 10 ms RPI (docs/eip-rpi-evaluation.md).
# It is carried as a patch rather than a fork because a submodule bump should
# stay a pointer change; if upstream fixes it, `git apply --check` starts
# failing and this block is the thing to delete.
#
# Applying it dirties the submodule working tree - `git status` will show
# third_party/EIPScanner as modified after a configure. That is expected.
# --------------------------------------------------------------------------
set(EIPSCANNER_PATCH ${CMAKE_CURRENT_SOURCE_DIR}/patches/eipscanner-io-timer.patch)

# Idempotent: a patch that is already applied reverses cleanly, and configuring
# twice must not fail.
execute_process(
  COMMAND git apply --reverse --check ${EIPSCANNER_PATCH}
  WORKING_DIRECTORY ${EIPSCANNER_ROOT}
  RESULT_VARIABLE EIPSCANNER_PATCH_PRESENT
  OUTPUT_QUIET ERROR_QUIET)

if(EIPSCANNER_PATCH_PRESENT EQUAL 0)
  message(STATUS "  EIPScanner patch    : already applied")
else()
  execute_process(
    COMMAND git apply ${EIPSCANNER_PATCH}
    WORKING_DIRECTORY ${EIPSCANNER_ROOT}
    RESULT_VARIABLE EIPSCANNER_PATCH_RC
    ERROR_VARIABLE EIPSCANNER_PATCH_ERR)
  if(NOT EIPSCANNER_PATCH_RC EQUAL 0)
    message(FATAL_ERROR
      "Could not apply ${EIPSCANNER_PATCH}:\n${EIPSCANNER_PATCH_ERR}\n"
      "If the submodule was bumped, check whether upstream fixed this and "
      "delete the patch, or refresh it against the new revision.")
  endif()
  message(STATUS "  EIPScanner patch    : applied")
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
