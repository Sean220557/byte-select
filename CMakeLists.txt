cmake_minimum_required(VERSION 3.16)
project(byte_select LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(byte_select
    src/pattern.cpp
    src/trainer.cpp
    src/codec.cpp
    src/model_io.cpp
)
target_include_directories(byte_select PUBLIC include)

if(MSVC)
    target_compile_options(byte_select PRIVATE /W4)
else()
    target_compile_options(byte_select PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(bsel src/main.cpp)
target_link_libraries(bsel PRIVATE byte_select)

enable_testing()
add_executable(byte_select_tests tests/tests.cpp)
target_link_libraries(byte_select_tests PRIVATE byte_select)
add_test(NAME byte_select_tests COMMAND byte_select_tests)
