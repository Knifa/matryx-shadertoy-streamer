cmake_minimum_required(VERSION 3.18)
project(matryx_shadertoy_streamer)

include(FetchContent)
FetchContent_Declare(
  argparse
  GIT_REPOSITORY https://github.com/p-ranav/argparse.git
  GIT_TAG v2.9
)
FetchContent_MakeAvailable(argparse)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq)
pkg_check_modules(websockets REQUIRED libwebsockets)
pkg_check_modules(uv REQUIRED libuv)

find_package(JPEG REQUIRED)

add_executable(matryx_shadertoy_streamer
  ./src/main.cpp
)

target_link_libraries(matryx_shadertoy_streamer PRIVATE
  argparse::argparse
  JPEG::JPEG
  pthread
  ${websockets_LIBRARIES}
  ${ZMQ_LIBRARIES}
)

target_include_directories(matryx_shadertoy_streamer PRIVATE
  argparse::argparse
  JPEG::JPEG
  ${websockets_INCLUDE_DIRS}
  ${ZMQ_INCLUDE_DIRS}
)

target_compile_features(matryx_shadertoy_streamer PRIVATE cxx_std_20)
target_compile_options(matryx_shadertoy_streamer PRIVATE
  -fcolor-diagnostics
  -mtune=native
  -O3
  -Wall
)
