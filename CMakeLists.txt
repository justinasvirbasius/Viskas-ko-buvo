cmake_minimum_required(VERSION 3.24)
project(ein_full_path_relay VERSION 0.5.0 LANGUAGES CXX)

option(EIN_BUILD_DEMO "Build the trace demonstration" ON)
option(EIN_BUILD_TESTS "Build deterministic correctness tests" ON)

add_library(ein_full_path_relay
    src/graphics_shell.cpp
    src/lan_administrator.cpp
    src/lan_discovery.cpp
    src/lan_protocol.cpp
    src/massive_classifier.cpp
)
add_library(EIN::FullPathRelay ALIAS ein_full_path_relay)
target_include_directories(ein_full_path_relay PUBLIC include)
target_compile_features(ein_full_path_relay PUBLIC cxx_std_20)
target_compile_options(ein_full_path_relay PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion>
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->
)
if(WIN32)
    target_link_libraries(ein_full_path_relay PUBLIC ws2_32)
endif()

if(EIN_BUILD_DEMO)
    add_executable(ein_relay_demo src/demo.cpp)
    target_link_libraries(ein_relay_demo PRIVATE EIN::FullPathRelay)
endif()

if(EIN_BUILD_TESTS)
    enable_testing()
    add_executable(ein_relay_tests tests/path_relay_tests.cpp)
    target_link_libraries(ein_relay_tests PRIVATE EIN::FullPathRelay)
    add_test(NAME ein_relay_tests COMMAND ein_relay_tests)
    add_executable(ein_classifier_tests tests/massive_classifier_tests.cpp)
    target_link_libraries(ein_classifier_tests PRIVATE EIN::FullPathRelay)
    add_test(NAME ein_classifier_tests COMMAND ein_classifier_tests)
    add_executable(ein_lan_tests tests/lan_administrator_tests.cpp)
    target_link_libraries(ein_lan_tests PRIVATE EIN::FullPathRelay)
    add_test(NAME ein_lan_tests COMMAND ein_lan_tests)
    if(EIN_BUILD_DEMO)
        add_test(NAME ein_relay_demo COMMAND ein_relay_demo)
    endif()
endif()
