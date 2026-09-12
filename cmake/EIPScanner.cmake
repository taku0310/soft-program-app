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
# Two of them:
#
#   eipscanner-io-timer  IOConnection's send timer loses real time twice over,
#                        which showed up as an O->T period of 11 982 us
#                        against a 10 ms RPI (docs/eip-rpi-evaluation.md).
#
#   eipscanner-hardening Two things, both about input nobody controls.
#
#                        Buffer::operator>> read past the end of the datagram,
#                        and the vector overload copied an attacker-supplied
#                        length without checking it. One malformed UDP
#                        datagram to port 2222 segfaulted the scanner process
#                        (SIGSEGV, measured - see the acceptance report).
#
#                        And a rejected ForwardOpen threw away the response
#                        that said why, while the one log line that carried
#                        the general status streamed it as a character and
#                        printed nothing. "Wrong assembly instance" and
#                        "device powered down" looked identical from above.
#
# They are carried as patches rather than a fork because a submodule bump
# should stay a pointer change; if upstream fixes one, `git apply --check`
# starts failing and that patch is the thing to delete.
#
# Applying it dirties the submodule working tree - `git status` will show
# third_party/EIPScanner as modified after a configure. That is expected, and
# it is not something to commit: the file belongs to upstream's repository and
# the submodule pointer is deliberately left alone.
#
# Do not "clean it up" by hand either. This runs at *configure* time only, so
# reverting the file and then building incrementally recompiles it unpatched
# without re-running configure and without saying anything - and an unpatched
# scanner fails in the least visible way there is, by sending 20% slow. If it
# has been reverted, delete the build directory or re-run cmake.
# --------------------------------------------------------------------------
set(EIPSCANNER_PATCHES
  ${CMAKE_CURRENT_SOURCE_DIR}/patches/eipscanner-io-timer.patch
  ${CMAKE_CURRENT_SOURCE_DIR}/patches/eipscanner-hardening.patch)

foreach(EIPSCANNER_PATCH ${EIPSCANNER_PATCHES})
  if(NOT EXISTS ${EIPSCANNER_PATCH})
    message(FATAL_ERROR
      "Missing ${EIPSCANNER_PATCH}.\n"
      "Building the Scanner needs the patches/ directory. In a container build "
      "that means `COPY patches ./patches` in the Dockerfile - see the note in "
      "the top-level CMakeLists.txt.")
  endif()
endforeach()

# git apply works outside a git repository, which matters: in a container build
# third_party/EIPScanner arrives as plain files with no .git.  The binary does
# have to exist, though, and a slim build image has no reason to carry it.
find_package(Git QUIET)
if(NOT Git_FOUND)
  message(FATAL_ERROR
    "git is required to apply ${EIPSCANNER_PATCH} and was not found.\n"
    "Install it in the build environment (in a container build, add git to the "
    "builder stage's apt-get install line).")
endif()

# Idempotent: a patch that is already applied reverses cleanly, and configuring
# twice must not fail. Each patch is checked on its own, so adding one to a
# tree that already carries the others works.
foreach(EIPSCANNER_PATCH ${EIPSCANNER_PATCHES})
  get_filename_component(EIPSCANNER_PATCH_NAME ${EIPSCANNER_PATCH} NAME_WE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${EIPSCANNER_PATCH}
    WORKING_DIRECTORY ${EIPSCANNER_ROOT}
    RESULT_VARIABLE EIPSCANNER_PATCH_PRESENT
    OUTPUT_QUIET ERROR_QUIET)

  if(EIPSCANNER_PATCH_PRESENT EQUAL 0)
    message(STATUS "  EIPScanner patch    : ${EIPSCANNER_PATCH_NAME} already applied")
  else()
    execute_process(
      COMMAND ${GIT_EXECUTABLE} apply ${EIPSCANNER_PATCH}
      WORKING_DIRECTORY ${EIPSCANNER_ROOT}
      RESULT_VARIABLE EIPSCANNER_PATCH_RC
      ERROR_VARIABLE EIPSCANNER_PATCH_ERR)
    if(NOT EIPSCANNER_PATCH_RC EQUAL 0)
      if(EIPSCANNER_PATCH_ERR MATCHES "not a git repository")
        # A submodule's .git is a file containing a path to the real gitdir. Copy
        # the tree somewhere that path does not resolve - a container build
        # context is the usual way - and git sees a repository it cannot open.
        message(FATAL_ERROR
          "Could not apply ${EIPSCANNER_PATCH}:\n${EIPSCANNER_PATCH_ERR}\n"
          "${EIPSCANNER_ROOT} carries a .git that points outside this tree. In a "
          "container build, exclude git metadata from the build context - the "
          "repository's .dockerignore does this; check it was not lost.")
      endif()
      message(FATAL_ERROR
        "Could not apply ${EIPSCANNER_PATCH}:\n${EIPSCANNER_PATCH_ERR}\n"
        "If the submodule was bumped, check whether upstream fixed this and "
        "delete the patch, or refresh it against the new revision.")
    endif()
    message(STATUS "  EIPScanner patch    : ${EIPSCANNER_PATCH_NAME} applied")
  endif()
endforeach()

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
