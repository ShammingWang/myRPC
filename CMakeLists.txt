cmake_minimum_required(VERSION 3.16)

project(myRPC VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(simple_tcp_server
    src/main.cpp
    src/server.cpp
    src/session.cpp
    src/socket_utils.cpp
)
