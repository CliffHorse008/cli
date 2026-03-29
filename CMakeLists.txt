cmake_minimum_required(VERSION 3.16)
project(embcli C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

add_library(embcli
    src/embcli.c
    src/embcli_telnet.c
)

target_include_directories(embcli
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(embcli PUBLIC _POSIX_C_SOURCE=200809L)

find_package(Threads REQUIRED)
target_link_libraries(embcli PUBLIC Threads::Threads)

add_executable(embcli_demo
    demo/main.c
    demo/demo_app.c
)
target_link_libraries(embcli_demo PRIVATE embcli)

add_executable(embcli_demo_selftest
    demo/selftest.c
    demo/demo_app.c
)
target_link_libraries(embcli_demo_selftest PRIVATE embcli Threads::Threads)

add_test(
    NAME embcli_demo_selftest
    COMMAND embcli_demo_selftest 2423 20 4 10
)
