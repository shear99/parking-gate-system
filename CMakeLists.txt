cmake_minimum_required(VERSION 3.20)
project(parking_gate_portfolio LANGUAGES CXX)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(gate-protocol protocol/src/protocol.cpp)
target_include_directories(gate-protocol PUBLIC protocol/include)
target_compile_features(gate-protocol PUBLIC cxx_std_20)
target_link_libraries(gate-protocol PUBLIC nlohmann_json::nlohmann_json)

add_library(gate-a6-codec edge-agent/src/a6_codec.cpp)
target_include_directories(gate-a6-codec PUBLIC edge-agent/include)
target_link_libraries(gate-a6-codec PUBLIC gate-protocol)
