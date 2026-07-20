cmake_minimum_required(VERSION 3.20)
project(crate VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(crate_core
    src/config.cpp
    src/io.cpp
    src/state.cpp
    src/runtime.cpp
)
target_include_directories(crate_core PUBLIC include)
target_compile_options(crate_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

add_executable(crate src/main.cpp)
target_link_libraries(crate PRIVATE crate_core)

install(TARGETS crate RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_executable(crate_config_tests tests/config_test.cpp)
    target_link_libraries(crate_config_tests PRIVATE crate_core)
    add_test(NAME config COMMAND crate_config_tests)
    add_executable(crate_state_tests tests/state_test.cpp)
    target_link_libraries(crate_state_tests PRIVATE crate_core)
    add_test(NAME state COMMAND crate_state_tests)
    add_executable(crate_io_tests tests/io_test.cpp)
    target_link_libraries(crate_io_tests PRIVATE crate_core)
    add_test(NAME io COMMAND crate_io_tests)
endif()
