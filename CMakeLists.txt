cmake_minimum_required(VERSION 3.10)
project(file_storage LANGUAGES CXX)

add_executable(code main.cpp)
set_target_properties(code PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_compile_options(code PRIVATE -O2 -Wall -Wextra -Wpedantic)

