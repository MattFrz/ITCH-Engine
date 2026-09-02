# ---------------------------------------------------------------------------
# Core library.
#
# Two books live here side by side and that is deliberate:
#
#   itch::OrderBook               research / historical path (std::map, std::list,
#                                 std::unordered_map) - the validated reference
#   itch::book::LowLatencyOrderBook  network-to-book path (pools, flat tick grid,
#                                 open addressing) - differentially tested
#                                 against the reference on every run
#
# No Python anywhere in this target: the low-latency path must be buildable and
# runnable without pybind11 or an interpreter.
# ---------------------------------------------------------------------------

option(ITCH_ENABLE_DPDK "Build the DPDK network backend (requires DPDK >= 21.11)" OFF)
option(ITCH_ENABLE_NATIVE "Compile with -march=native (NOT portable - see docs)" OFF)
option(ITCH_ENABLE_LTO "Link-time optimization for the performance build" OFF)
option(ITCH_ENABLE_ASAN "Address + undefined behaviour sanitizers" OFF)
option(ITCH_ENABLE_TSAN "Thread sanitizer" OFF)
option(ITCH_BUILD_BENCH "Build the benchmark binaries" ON)
option(ITCH_BUILD_APPS "Build the replay/live command line tools" ON)

add_library(itch_engine_core STATIC
    src/order_book.cpp
    src/price_level.cpp
    src/udp_multicast_backend.cpp
    src/multicast_publisher.cpp
    src/dpdk_backend.cpp
    src/affinity.cpp
)
target_include_directories(itch_engine_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(WIN32)
  # The multicast receiver uses Winsock on Windows so the whole pipeline still
  # builds and unit-tests there; the tuned receive path (recvmmsg, SO_BUSY_POLL,
  # SO_TIMESTAMPING) is Linux only and documented as such.
  target_link_libraries(itch_engine_core PUBLIC ws2_32)
endif()

if(MSVC)
  target_compile_options(itch_engine_core PRIVATE /W4)
  # MSVC needs this to report __cplusplus correctly and to keep the
  # preprocessor standards-conforming.
  target_compile_options(itch_engine_core PUBLIC /Zc:__cplusplus /Zc:preprocessor)
  # std::fopen is not "unsafe" here and fopen_s is not portable.
  target_compile_definitions(itch_engine_core PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(itch_engine_core PRIVATE -Wall -Wextra)
endif()

# --- build-type flavours ----------------------------------------------------
#
# Release      -O2, portable, what CI builds and what the tests run against.
# Performance  -O3 (+ optional -march=native, + optional LTO). Explicitly NOT
#              the default: -march=native produces a binary that can fault with
#              an illegal instruction on any machine older than the builder, so
#              it must be an opt-in on a machine you own.
# Debug        -O0 -g, assertions live.
# ASAN / TSAN  correctness builds; never used for a latency number.

if(MSVC)
  set(CMAKE_CXX_FLAGS_PERFORMANCE "/O2 /Ob3 /DNDEBUG" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS_ASAN "/Od /Zi /fsanitize=address" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS_TSAN "/Od /Zi" CACHE STRING "" FORCE)
else()
  set(CMAKE_CXX_FLAGS_PERFORMANCE "-O3 -DNDEBUG" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS_ASAN
      "-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS_TSAN "-O1 -g -fsanitize=thread -fno-omit-frame-pointer" CACHE STRING "" FORCE)
endif()
set(CMAKE_EXE_LINKER_FLAGS_PERFORMANCE "" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_ASAN "${CMAKE_EXE_LINKER_FLAGS_DEBUG}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_TSAN "${CMAKE_EXE_LINKER_FLAGS_DEBUG}" CACHE STRING "" FORCE)
set(CMAKE_STATIC_LINKER_FLAGS_PERFORMANCE "" CACHE STRING "" FORCE)
set(CMAKE_STATIC_LINKER_FLAGS_ASAN "" CACHE STRING "" FORCE)
set(CMAKE_STATIC_LINKER_FLAGS_TSAN "" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_PERFORMANCE "" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_ASAN "" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_TSAN "" CACHE STRING "" FORCE)

function(itch_apply_build_options target)
  if(ITCH_ENABLE_NATIVE AND NOT MSVC)
    target_compile_options(${target} PRIVATE -march=native -mtune=native)
  endif()
  if(ITCH_ENABLE_LTO)
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
  if(ITCH_ENABLE_ASAN AND MSVC)
    target_compile_options(${target} PRIVATE /fsanitize=address)
  endif()
  if(ITCH_ENABLE_ASAN AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
  if(ITCH_ENABLE_TSAN AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=thread)
  endif()
  if(ITCH_ENABLE_DPDK)
    target_compile_definitions(${target} PRIVATE ITCH_ENABLE_DPDK=1)
    target_link_libraries(${target} PRIVATE PkgConfig::DPDK)
  endif()
endfunction()

if(ITCH_ENABLE_DPDK)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)
  message(STATUS "DPDK backend enabled: ${DPDK_VERSION}")
endif()

itch_apply_build_options(itch_engine_core)

# --- tests ------------------------------------------------------------------

set(ITCH_TESTS
    test_order_book          # existing research book semantics
    test_order_index         # open-addressing table
    test_moldudp64           # transport: gaps, duplicates, malformed, recovery
    test_itch_parser         # binary decode + encode round trip
    test_low_latency_book    # FIFO semantics of the new book, standalone
    test_differential        # research book vs low-latency book, same stream
    test_pipeline            # capture -> mold -> parser -> book equivalence
    test_execution           # strategy / risk / simulated order gateway
    test_fuzz                # malformed network input must not read out of bounds
)

foreach(t IN LISTS ITCH_TESTS)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE itch_engine_core)
  itch_apply_build_options(${t})
  if(MSVC)
    target_compile_options(${t} PRIVATE /W4)
  else()
    target_compile_options(${t} PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# --- benchmarks -------------------------------------------------------------

if(ITCH_BUILD_BENCH)
  foreach(b bench_pipeline bench_book)
    add_executable(${b} bench/${b}.cpp)
    target_link_libraries(${b} PRIVATE itch_engine_core)
    # The deterministic event generator lives with the tests; the benchmarks
    # drive the same streams so the two are always talking about the same data.
    target_include_directories(${b} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    itch_apply_build_options(${b})
  endforeach()

  # The packed-vs-padded order node is a measurement, not a preference. The
  # default is the 24-byte packed node because that is what measured faster;
  # this second binary is the 32-byte padded variant, so the comparison can be
  # re-run on any machine rather than taken on trust.
  add_executable(bench_book_packed bench/bench_book.cpp)
  target_link_libraries(bench_book_packed PRIVATE itch_engine_core)
  target_include_directories(bench_book_packed PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_definitions(bench_book_packed PRIVATE ITCH_LL_ORDER_PAD32=1)
  itch_apply_build_options(bench_book_packed)
endif()

# --- command line tools -----------------------------------------------------

if(ITCH_BUILD_APPS)
  foreach(a itch_make_capture itch_replay itch_live)
    add_executable(${a} apps/${a}.cpp)
    target_link_libraries(${a} PRIVATE itch_engine_core)
    itch_apply_build_options(${a})
  endforeach()
  # itch_make_capture can generate a synthetic day, which reuses the same
  # deterministic generator the tests and benchmarks use.
  target_include_directories(itch_make_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
endif()

# --- Python bindings (research path only) -----------------------------------
#
# pybind11 is located automatically by asking the Python interpreter for its
# cmake dir (pip install pybind11 is the only setup); an explicit
# -Dpybind11_DIR=... still overrides if provided. Nothing in the low-latency
# path depends on this target existing.
if(NOT pybind11_DIR)
  find_package(Python COMPONENTS Interpreter Development QUIET)
  if(Python_Interpreter_FOUND)
    execute_process(
      COMMAND ${Python_EXECUTABLE} -m pybind11 --cmakedir
      OUTPUT_VARIABLE _pybind11_cmakedir
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(_pybind11_cmakedir)
      set(pybind11_DIR ${_pybind11_cmakedir})
    endif()
  endif()
endif()
find_package(pybind11 CONFIG QUIET)
if(pybind11_FOUND)
  # NO_EXTRAS: skip pybind11's LTO/LTCG flags, which miscompile the module
  # under the MSVC 14.50 insiders toolset (import-time access violation).
  pybind11_add_module(itch_engine_cpp NO_EXTRAS bindings/pybind_module.cpp)
  target_link_libraries(itch_engine_cpp PRIVATE itch_engine_core)
  # Drop the module next to the Python package so `import itch_engine_cpp`
  # works with python/ on sys.path.
  set_target_properties(itch_engine_cpp PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/python
      LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/python
      LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/python)
else()
  message(STATUS "pybind11 not found - building C++ core, tests and tools only")
endif()
