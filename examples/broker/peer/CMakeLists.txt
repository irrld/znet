cmake_minimum_required(VERSION 3.29)
project(broker-peer LANGUAGES CXX)

add_executable(broker-peer main.cc)
znet_apply_cxx_standard(broker-peer)
target_link_libraries(broker-peer PRIVATE znet cxxopts)
target_include_directories(broker-peer PRIVATE ../common)
