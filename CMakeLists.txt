cmake_minimum_required(VERSION 3.16)

project(myRPC VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(include)

add_library(mrpc_client_lib
    src/client/rpc_client.cpp
    src/codec/rpc_codec.cpp
    src/codec/rpc_serialization.cpp
)

add_executable(simple_tcp_server
    src/server/main.cpp
    src/connection/connection.cpp
    src/codec/rpc_codec.cpp
    src/codec/rpc_serialization.cpp
    src/dispatcher/rpc_dispatcher.cpp
    src/observability/observability.cpp
    src/server/server.cpp
    src/server/socket_utils.cpp
    src/worker/worker_pool.cpp
)

add_executable(mrpc_bench_client
    bench/cpp_bench_client.cpp
)

add_executable(simple_client
    examples/simple_client.cpp
)

target_link_libraries(simple_client PRIVATE mrpc_client_lib)
