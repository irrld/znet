cmake_minimum_required(VERSION 3.29)
project(basic-p2p LANGUAGES CXX)

add_executable(basic-p2p main.cc)
set_target_properties(basic-p2p PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(basic-p2p PRIVATE znet)

