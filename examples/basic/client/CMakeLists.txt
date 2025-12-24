cmake_minimum_required(VERSION 3.29)
project(basic-client LANGUAGES CXX)

add_executable(basic-client main.cc)
set_target_properties(basic-client PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(basic-client PRIVATE znet)
target_include_directories(basic-client PRIVATE ../common)

