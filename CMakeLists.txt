cmake_minimum_required(VERSION 3.10)
project(rdma-tutorial C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Werror")
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_C_FLAGS_DEBUG "-g -DDEBUG")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(SOURCES
    src/main.c
    src/tcp_helper.c
    src/rdma_core/rdma_setup.c
    src/rdma_core/rdma_verbs.c
    src/echo_server.c
    src/echo_client.c
)

add_executable(rdma-tutorial ${SOURCES})

target_include_directories(rdma-tutorial PRIVATE src)

target_link_libraries(rdma-tutorial
    ibverbs
    pthread
    rdmacm
)
