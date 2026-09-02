cmake_minimum_required(VERSION 3.20)
project(itch_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build flavours.
#
#   Debug        -O0 -g, assertions live
#   Release      -O2, portable - what CI builds and what the tests run against
#   Performance  -O3 (plus optional -march=native and LTO, both opt-in)
#   Asan / Tsan  correctness builds, never used for a latency number
#
# Performance is NOT the default and -march=native is NOT implied by it: a
# binary built for the machine that compiled it can fault with an illegal
# instruction anywhere else, so that has to be an explicit choice on hardware
# you own. See docs/low_latency_architecture.md.
set(ITCH_BUILD_TYPES Debug Release Performance Asan Tsan)

if(CMAKE_CONFIGURATION_TYPES)
  # Multi-config generators (Visual Studio, Xcode): offer the extra flavours
  # alongside the standard ones.
  list(APPEND CMAKE_CONFIGURATION_TYPES Performance)
  list(REMOVE_DUPLICATES CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "" FORCE)
else()
  # Single-config generators (Ninja, Make): default to Release and offer the
  # flavours in the cache editor.
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build flavour" FORCE)
  endif()
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${ITCH_BUILD_TYPES})
endif()

enable_testing()
add_subdirectory(cpp)
