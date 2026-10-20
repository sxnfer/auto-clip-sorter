# Standalone unit tests for Clip Atlas core logic (no OBS required).
#   cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests
cmake_minimum_required(VERSION 3.22)
project(clip_atlas_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)

set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/core")

# Globbed so parallel work packages never need to edit this file.
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS "${CORE_DIR}/*.cpp")
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test-*.cpp")

add_executable(clip_atlas_tests ${TEST_SOURCES} ${CORE_SOURCES})
target_include_directories(clip_atlas_tests PRIVATE "${CORE_DIR}")
target_link_libraries(clip_atlas_tests PRIVATE Catch2::Catch2WithMain)
if(MSVC)
  target_compile_options(clip_atlas_tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(clip_atlas_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
include(Catch)
catch_discover_tests(clip_atlas_tests)
