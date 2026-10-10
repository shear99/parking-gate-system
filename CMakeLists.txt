cmake_minimum_required(VERSION 3.20)
project(parking_gate_portfolio LANGUAGES CXX)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

# Deliberately compile only pure decision cores. No network or serial runtime.
add_library(gate-protocol protocol/src/protocol.cpp)
target_include_directories(gate-protocol PUBLIC protocol/include)
target_compile_features(gate-protocol PUBLIC cxx_std_20)
target_link_libraries(gate-protocol PUBLIC nlohmann_json::nlohmann_json)

add_library(gate-coordinator-core coordinator/src/coordinator_core.cpp)
target_include_directories(gate-coordinator-core PUBLIC coordinator/include)
target_link_libraries(gate-coordinator-core PUBLIC gate-protocol Threads::Threads)

add_library(gate-agent-core edge-agent/src/agent_core.cpp edge-agent/src/a6_codec.cpp)
target_include_directories(gate-agent-core PUBLIC edge-agent/include)
target_link_libraries(gate-agent-core PUBLIC gate-protocol Threads::Threads)

include(CTest)
if(BUILD_TESTING)
  foreach(test_name IN ITEMS agent_core coordinator_core concurrency)
    add_executable(${test_name}_test tests/${test_name}_test.cpp)
    target_link_libraries(${test_name}_test PRIVATE gate-agent-core gate-coordinator-core Threads::Threads)
    # Existing tests use assert: preserve assertions in Release builds too.
    if(MSVC)
      target_compile_options(${test_name}_test PRIVATE /UNDEBUG)
    else()
      target_compile_options(${test_name}_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name}_test)
  endforeach()
endif()
