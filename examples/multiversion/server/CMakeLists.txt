cmake_minimum_required(VERSION 3.29)
project(multiversion-server LANGUAGES CXX)

add_executable(multiversion-server main.cc)
set_target_properties(multiversion-server PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(multiversion-server PRIVATE znet)
target_include_directories(multiversion-server PRIVATE ../common)

