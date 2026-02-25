cmake_minimum_required(VERSION 3.16)
project(hw6)

set(CMAKE_C_STANDARD 99)

add_subdirectory(libtdmm)

add_executable(hw6 main.c)
target_link_libraries(hw6 tdmm)
