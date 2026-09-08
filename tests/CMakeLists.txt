cmake_minimum_required(VERSION 3.24)
project(FastEnoughTests LANGUAGES CXX)
enable_testing()

add_executable(connection_tests
    connection_tests.cpp
    ../src/adb_discovery.cpp
    ../src/process_runner.cpp
)
target_include_directories(connection_tests PRIVATE ../src)
target_compile_features(connection_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(connection_tests PRIVATE /W4 /utf-8)
endif()
add_test(NAME connection_regressions COMMAND connection_tests)
set_tests_properties(connection_regressions PROPERTIES TIMEOUT 30)
