cmake_minimum_required(VERSION 3.29)
project(p2p-steps LANGUAGES CXX)

add_executable(p2p-steps main.cc)
znet_apply_cxx_standard(p2p-steps)
target_link_libraries(p2p-steps PRIVATE znet cxxopts)
