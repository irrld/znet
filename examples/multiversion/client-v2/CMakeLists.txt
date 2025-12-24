cmake_minimum_required(VERSION 3.29)
project(multiversion-client-v2 LANGUAGES CXX)

add_executable(multiversion-client-v2 main.cc)
set_target_properties(multiversion-client-v2 PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(multiversion-client-v2 PRIVATE znet)
target_include_directories(multiversion-client-v2 PRIVATE ../common)

