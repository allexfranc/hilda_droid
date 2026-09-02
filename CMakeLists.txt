cmake_minimum_required(VERSION 4.2)
project(hilda_droid C)

set(CMAKE_C_STANDARD 99)

add_executable(
        hilda_droid
        src/main.c
        src/address_finder.c
)

target_include_directories(hilda_droid PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(
        payload SHARED
        src/payload.c
        src/address_finder.c
)

find_library(log-lib log)
target_link_libraries(payload ${log-lib})
