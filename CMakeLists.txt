cmake_minimum_required(VERSION 4.2)
project(roadblock_patcher C)

set(CMAKE_C_STANDARD 99)

add_executable(roadblock_patcher main.c)

target_link_libraries(roadblock_patcher ntdll)