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

add_executable(activity_tests activity_tests.cpp)
target_include_directories(activity_tests PRIVATE ../src)
target_compile_features(activity_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(activity_tests PRIVATE /W4 /utf-8)
endif()
add_test(NAME activity_and_local_copy COMMAND activity_tests)
set_tests_properties(activity_and_local_copy PROPERTIES TIMEOUT 30)

add_executable(pane_drop_tests
    pane_drop_tests.cpp
    ../thirdparty/imgui/imgui.cpp
    ../thirdparty/imgui/imgui_draw.cpp
    ../thirdparty/imgui/imgui_tables.cpp
    ../thirdparty/imgui/imgui_widgets.cpp
)
target_include_directories(pane_drop_tests PRIVATE ../src ../thirdparty/imgui)
target_compile_features(pane_drop_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(pane_drop_tests PRIVATE /W4 /utf-8)
endif()
add_test(NAME pane_drag_and_drop COMMAND pane_drop_tests)
set_tests_properties(pane_drag_and_drop PROPERTIES TIMEOUT 30)
