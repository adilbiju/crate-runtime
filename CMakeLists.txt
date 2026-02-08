cmake_minimum_required(VERSION 3.20)
project(crate VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(crate_core
    src/config.cpp
)
target_include_directories(crate_core PUBLIC include)
target_compile_options(crate_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
