cmake_minimum_required(VERSION 4.1.0)

# Find jemalloc provided by vcpkg
find_package(jemalloc CONFIG REQUIRED)

file(GLOB_RECURSE lib_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cxx")

add_library(libsonic STATIC ${lib_sources})

set_target_properties(libsonic PROPERTIES EXPORT_COMPILE_COMMANDS ON)
target_compile_features(libsonic PRIVATE cxx_std_20)
target_include_directories(libsonic PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(libsonic
  PRIVATE
  jemalloc::jemalloc
)

if(SONIC_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED)

  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

  file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_Test.cc")

  add_executable(libsonic_tests ${test_sources})

  if(SONIC_TESTS_USES_ASAN)

    target_compile_options(libsonic_tests PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang>:-fsanitize=address,undefined>
      $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-omit-frame-pointer> # Essential for clean stack traces
      $<$<CXX_COMPILER_ID:MSVC>:/fsanitize=address>
    )

    target_link_options(libsonic_tests PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang>:-fsanitize=address,undefined>
    )
  elseif(SONIC_TESTS_USES_TSAN)

    target_compile_options(libsonic_tests PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang>:-fsanitize=thread>
      $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-omit-frame-pointer> # Helps get accurate race call-stacks
      $<$<CXX_COMPILER_ID:GNU,Clang>:-g> # Adds debug symbols to see line numbers
    )

    target_link_options(libsonic_tests PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang>:-fsanitize=thread>
    )
  endif()

  target_link_libraries(libsonic_tests
    PRIVATE libsonic
    PRIVATE GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(libsonic_tests)

endif()
