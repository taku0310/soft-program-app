cmake_minimum_required(VERSION 3.20)
project(plc_runtime C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes)

find_package(Threads REQUIRED)
find_library(MOSQUITTO_LIB mosquitto)

set(SOURCES
    src/plc_runtime.c
    src/ethernet_ip/cip_adapter.c
    src/ethernet_ip/connection_manager.c
    src/ethernet_ip/message_router.c
    src/mqtt/mqtt_publisher.c
    src/scheduler/cpu_affinity.c
    src/scheduler/cycle_monitor.c
    src/ipc/shared_memory.c
)

add_executable(plc_runtime ${SOURCES})
target_include_directories(plc_runtime PRIVATE src)
target_link_libraries(plc_runtime PRIVATE Threads::Threads ${MOSQUITTO_LIB} rt)

enable_testing()
add_subdirectory(tests)
