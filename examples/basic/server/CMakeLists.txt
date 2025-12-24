cmake_minimum_required(VERSION 3.29)
project(basic-server LANGUAGES CXX)

add_executable(basic-server main.cc)
set_target_properties(basic-server PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(basic-server PRIVATE znet)
target_include_directories(basic-server PRIVATE ../common)

