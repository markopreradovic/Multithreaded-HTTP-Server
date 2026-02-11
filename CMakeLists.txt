cmake_minimum_required(VERSION 3.16)

project(MultithreadedHTTPServer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic -Werror -O2)

find_package(Threads REQUIRED)

add_executable(httpserver
        src/main.c
)

target_link_libraries(httpserver PRIVATE
        Threads::Threads     # pthread biblioteka
        rt                   # realtime biblioteka (clock_gettime, itd.)
)