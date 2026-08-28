# SPDX-License-Identifier: Apache-2.0
#
# Builds the vendored OpENer (EIPStackGroup) sources into a static `opener`
# target.
#
# OpENer's own CMake project is deliberately not used via add_subdirectory().
# It builds an `OpENer` executable whose SAMPLE_APP library defines
# ApplicationInitialization(), AfterAssemblyDataReceived(),
# BeforeAssemblyDataSend() and the rest - exactly the symbols our adapter has
# to define.  Linking both would be a duplicate-symbol conflict, and working
# around it would mean patching the submodule.  Compiling the stack sources
# directly is the smaller, more stable coupling: we own the application layer,
# upstream owns everything below it, and updating the submodule is a pointer
# bump rather than a merge.

set(OPENER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/OpENer)
set(OPENER_SRC  ${OPENER_ROOT}/source/src)

if(NOT EXISTS ${OPENER_SRC}/opener_api.h)
  message(FATAL_ERROR
    "OpENer sources are missing. Run:\n"
    "  git submodule update --init --recursive\n"
    "or configure with -DSOFTPLC_WITH_OPENER=OFF to build the adapter with "
    "its loopback backend.")
endif()

# --- device identity ------------------------------------------------------
# These become the CIP Identity object's attributes.  Vendor ID 1 is
# Rockwell's placeholder from the OpENer sample; a shipping device needs an
# ODVA-assigned vendor ID and its own product code here.
set(OpENer_Device_Config_Vendor_Id   1     CACHE STRING "CIP Vendor ID")
set(OpENer_Device_Config_Device_Type 12    CACHE STRING "CIP Device Type (12 = communications adapter)")
set(OpENer_Device_Config_Product_Code 65001 CACHE STRING "CIP Product Code")
set(OpENer_Device_Config_Device_Name "SoftPLC EtherNet/IP Adapter" CACHE STRING "CIP Product Name")
set(OpENer_Device_Major_Version 2 CACHE STRING "Device major revision")
set(OpENer_Device_Minor_Version 3 CACHE STRING "Device minor revision")

configure_file(
  ${OPENER_SRC}/ports/devicedata.h.in
  ${CMAKE_BINARY_DIR}/opener_generated/devicedata.h
  @ONLY)

# --- sources --------------------------------------------------------------
# main.c and ports/POSIX/sample_application/* are excluded on purpose: this
# process supplies its own main() and its own application call-backs.
set(OPENER_SOURCES
  ${OPENER_SRC}/cip/appcontype.c
  ${OPENER_SRC}/cip/cipassembly.c
  ${OPENER_SRC}/cip/cipclass3connection.c
  ${OPENER_SRC}/cip/cipcommon.c
  ${OPENER_SRC}/cip/cipconnectionmanager.c
  ${OPENER_SRC}/cip/cipconnectionobject.c
  ${OPENER_SRC}/cip/cipdlr.c
  ${OPENER_SRC}/cip/cipelectronickey.c
  ${OPENER_SRC}/cip/cipepath.c
  ${OPENER_SRC}/cip/cipethernetlink.c
  ${OPENER_SRC}/cip/cipidentity.c
  ${OPENER_SRC}/cip/cipioconnection.c
  ${OPENER_SRC}/cip/cipmessagerouter.c
  ${OPENER_SRC}/cip/cipqos.c
  ${OPENER_SRC}/cip/cipstring.c
  ${OPENER_SRC}/cip/cipstringi.c
  ${OPENER_SRC}/cip/ciptcpipinterface.c
  ${OPENER_SRC}/cip/ciptypes.c

  ${OPENER_SRC}/enet_encap/cpf.c
  ${OPENER_SRC}/enet_encap/encap.c
  ${OPENER_SRC}/enet_encap/endianconv.c

  ${OPENER_SRC}/utils/doublylinkedlist.c
  ${OPENER_SRC}/utils/enipmessage.c
  ${OPENER_SRC}/utils/random.c
  ${OPENER_SRC}/utils/xorshiftrandom.c

  ${OPENER_SRC}/ports/generic_networkhandler.c
  ${OPENER_SRC}/ports/socket_timer.c
  ${OPENER_SRC}/ports/nvdata/conffile.c
  ${OPENER_SRC}/ports/nvdata/nvdata.c
  ${OPENER_SRC}/ports/nvdata/nvqos.c
  ${OPENER_SRC}/ports/nvdata/nvtcpip.c

  ${OPENER_SRC}/ports/POSIX/networkhandler.c
  ${OPENER_SRC}/ports/POSIX/networkconfig.c
  ${OPENER_SRC}/ports/POSIX/opener_error.c
)

add_library(opener STATIC ${OPENER_SOURCES})

target_include_directories(opener PUBLIC
  ${CMAKE_BINARY_DIR}/opener_generated
  ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/eip/opener_conf  # our opener_user_conf.h
  ${OPENER_SRC}
  ${OPENER_SRC}/cip
  ${OPENER_SRC}/enet_encap
  ${OPENER_SRC}/ports
  ${OPENER_SRC}/ports/nvdata
  ${OPENER_SRC}/ports/POSIX
  ${OPENER_SRC}/utils
)

# These mirror OpENer's own POSIX platform macro
# (buildsupport/POSIX/OpENer_PLATFORM_INCLUDES.cmake).  They are part of the
# stack's build contract, not our choices: RESTRICT in particular appears in
# public prototypes, so getting it wrong is a parse error rather than a subtle
# behaviour change.
target_compile_definitions(opener PUBLIC
  OPENER_POSIX
  RESTRICT=restrict
  _POSIX_C_SOURCE=200112L
  _GNU_SOURCE
  # The scanner prepends a 32-bit run/idle header to O->T data.  This must
  # match what the originator sends or every consumed frame is misaligned by
  # four bytes; ODVA scanners send it, so it stays on.
  OPENER_CONSUMED_DATA_HAS_RUN_IDLE_HEADER=1
  PC_OPENER_ETHERNET_BUFFER_SIZE=512
  OPENER_TRACE_LEVEL=3          # errors and warnings only
)

# Upstream code, not ours: build it clean rather than holding it to this
# project's warning settings, which would make a submodule bump noisy.
target_compile_options(opener PRIVATE -w -fcommon)

# Upstream's own executable links libcap, but nothing in the source set we
# compile references it - that dependency belongs to their sample main().
target_link_libraries(opener PUBLIC Threads::Threads)

message(STATUS "  OpENer sources      : ${OPENER_SRC}")
