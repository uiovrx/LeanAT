cmake_minimum_required(VERSION 3.20)
project(LeanAT VERSION 0.1.0 LANGUAGES CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(LEANAT_WITH_SYSTEMC "Build real SystemC adapters and tests" OFF)
option(LEANAT_BUILD_TESTS "Build regression tests" ON)
option(LEANAT_BUILD_CONFORMANCE "Build the original profile acceptance harness (requires Python)" OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB LEANAT_RUNTIME_SOURCES CONFIGURE_DEPENDS runtime/src/*.cpp)
add_library(leanat_at STATIC ${LEANAT_RUNTIME_SOURCES})
add_library(LeanAT::Runtime ALIAS leanat_at)
set_target_properties(leanat_at PROPERTIES EXPORT_NAME Runtime)
target_compile_features(leanat_at PUBLIC cxx_std_17)
target_include_directories(leanat_at PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(MSVC)
  target_compile_options(leanat_at PRIVATE /W4 /permissive-)
else()
  target_compile_options(leanat_at PRIVATE -Wall -Wextra -Wpedantic)
endif()

file(GLOB LEANAT_STDLIB_SOURCES CONFIGURE_DEPENDS stdlib/src/*.cpp)
add_library(leanat_stdlib STATIC ${LEANAT_STDLIB_SOURCES})
add_library(LeanAT::Stdlib ALIAS leanat_stdlib)
set_target_properties(leanat_stdlib PROPERTIES EXPORT_NAME Stdlib)
target_include_directories(leanat_stdlib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/stdlib/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(leanat_stdlib PUBLIC leanat_at)

if(LEANAT_BUILD_TESTS)
  enable_testing()
  file(GLOB LEANAT_CPP_TESTS CONFIGURE_DEPENDS tests/cpp/test_*.cpp)
  foreach(test_source IN LISTS LEANAT_CPP_TESTS)
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp)
    target_link_libraries(${test_name} PRIVATE leanat_stdlib)
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 30)
  endforeach()
endif()

if(LEANAT_WITH_SYSTEMC)
  add_subdirectory(systemc)
endif()

if(LEANAT_BUILD_TESTS AND LEANAT_BUILD_CONFORMANCE)
  add_subdirectory(tests/conformance)
endif()

add_executable(leanat_descriptor tools/cpp/descriptor_main.cpp)
target_link_libraries(leanat_descriptor PRIVATE leanat_at)
add_executable(leanat_opcode_replay tools/cpp/opcode_replay_main.cpp)
target_link_libraries(leanat_opcode_replay PRIVATE leanat_stdlib)
if(MINGW)
  target_compile_options(leanat_opcode_replay PRIVATE -Wa,-mbig-obj)
elseif(MSVC)
  target_compile_options(leanat_opcode_replay PRIVATE /bigobj)
endif()
add_executable(leanat_transport_replay tools/cpp/transport_replay_main.cpp)
target_link_libraries(leanat_transport_replay PRIVATE leanat_at)
install(TARGETS leanat_descriptor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(TARGETS leanat_at leanat_stdlib EXPORT LeanATTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY runtime/include/ stdlib/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
set(LEANAT_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/LeanAT)
configure_package_config_file(cmake/LeanATConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/LeanATConfig.cmake
  INSTALL_DESTINATION ${LEANAT_CMAKE_INSTALL_DIR})
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/LeanATConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(EXPORT LeanATTargets NAMESPACE LeanAT::
  DESTINATION ${LEANAT_CMAKE_INSTALL_DIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/LeanATConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/LeanATConfigVersion.cmake
  DESTINATION ${LEANAT_CMAKE_INSTALL_DIR})
