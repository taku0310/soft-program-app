add_executable(test_cycle_monitor test_cycle_monitor.c ../src/scheduler/cycle_monitor.c)
target_include_directories(test_cycle_monitor PRIVATE ../src)
add_test(NAME cycle_monitor COMMAND test_cycle_monitor)
