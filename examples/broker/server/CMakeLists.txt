cmake_minimum_required(VERSION 3.29)
project(broker-server LANGUAGES CXX)

add_executable(broker-server main.cc)
znet_apply_cxx_standard(broker-server)
target_link_libraries(broker-server PRIVATE znet cxxopts)
target_include_directories(broker-server PRIVATE ../common)
