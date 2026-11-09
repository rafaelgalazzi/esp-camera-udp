cmake_minimum_required(VERSION 3.20)
project(openiris_udp_bridge VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
add_library(bridge_core INTERFACE)
target_include_directories(bridge_core INTERFACE common pc/include)
target_link_libraries(bridge_core INTERFACE Threads::Threads)
if(WIN32)
  target_link_libraries(bridge_core INTERFACE ws2_32 psapi)
  target_compile_definitions(bridge_core INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
add_executable(openiris-udp-bridge pc/src/main.cpp)
target_link_libraries(openiris-udp-bridge PRIVATE bridge_core)
add_custom_target(bridge-config ALL
  COMMAND ${CMAKE_COMMAND}
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DOUTPUT_DIR=$<TARGET_FILE_DIR:openiris-udp-bridge>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare-portable.cmake"
  DEPENDS openiris-udp-bridge
  VERBATIM)
add_custom_command(TARGET openiris-udp-bridge POST_BUILD
  COMMAND ${CMAKE_COMMAND}
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DOUTPUT_DIR=$<TARGET_FILE_DIR:openiris-udp-bridge>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare-portable.cmake"
  VERBATIM)
install(TARGETS openiris-udp-bridge RUNTIME DESTINATION .)
install(CODE "
  set(SOURCE_DIR \"${CMAKE_CURRENT_SOURCE_DIR}\")
  set(OUTPUT_DIR \"\${CMAKE_INSTALL_PREFIX}\")
  include(\"${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare-portable.cmake\")
")
if(MSVC)
  target_compile_options(openiris-udp-bridge PRIVATE /W4 /permissive-)
else()
  target_compile_options(openiris-udp-bridge PRIVATE -Wall -Wextra -Wpedantic)
endif()
enable_testing()
add_executable(protocol-tests tests/protocol_tests.cpp)
target_link_libraries(protocol-tests PRIVATE bridge_core)
add_test(NAME protocol COMMAND protocol-tests)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py $<TARGET_FILE:openiris-udp-bridge>)
  set_tests_properties(integration PROPERTIES TIMEOUT 45)
endif()
