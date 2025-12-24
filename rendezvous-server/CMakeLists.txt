cmake_minimum_required(VERSION 3.29)
project(rendezvous-server LANGUAGES CXX)

add_executable(rendezvous-server main.cc)
set_target_properties(rendezvous-server PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(rendezvous-server PRIVATE znet)

