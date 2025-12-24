cmake_minimum_required(VERSION 3.29)
project(multiversion-client-v1 LANGUAGES CXX)

add_executable(multiversion-client-v1 main.cc)
set_target_properties(multiversion-client-v1 PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(multiversion-client-v1 PRIVATE znet)
target_include_directories(multiversion-client-v1 PRIVATE ../common)

