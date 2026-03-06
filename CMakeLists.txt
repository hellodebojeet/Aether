cmake_minimum_required(VERSION 3.10)
project(proxy)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -Wall -D_GNU_SOURCE")

add_executable(proxy proxy_server_with_cache.c proxy_parse.c)
target_link_libraries(proxy pthread)