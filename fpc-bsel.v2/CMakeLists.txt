cmake_minimum_required(VERSION 3.16)
project(fpc_bsel_v2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(fpc_bsel_lib
    src/bsel.cpp
    src/fpc.cpp
    src/codec.cpp
    src/model.cpp)
target_include_directories(fpc_bsel_lib PUBLIC include)
if(MSVC)
    target_compile_options(fpc_bsel_lib PRIVATE /W4)
else()
    target_compile_options(fpc_bsel_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(fpc-bsel-v2 src/main.cpp)
target_link_libraries(fpc-bsel-v2 PRIVATE fpc_bsel_lib)

enable_testing()
add_executable(fpc-bsel-tests tests/tests.cpp)
target_link_libraries(fpc-bsel-tests PRIVATE fpc_bsel_lib)
add_test(NAME fpc-bsel-tests COMMAND fpc-bsel-tests)
