cmake_minimum_required(VERSION 4.1.0)

project(sonic LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "Vcpkg toolchain file" FORCE)
endif()

option(SONIC_BUILD_TESTS "Build unit tests for hive" ON)
option(SONIC_TESTS_USES_ASAN "Build tests with address sanitizer enabled" OFF)
option(SONIC_TESTS_USES_TSAN "Build tests with thread sanitizer enabled" OFF)

if(SONIC_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory("src")
