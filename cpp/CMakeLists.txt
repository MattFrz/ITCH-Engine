# Core library (no Python dependency - usable from pure C++ tests/benchmarks).
add_library(itch_engine_core STATIC
    src/order_book.cpp
    src/price_level.cpp
)
target_include_directories(itch_engine_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
  target_compile_options(itch_engine_core PRIVATE /W4 /O2)
else()
  target_compile_options(itch_engine_core PRIVATE -Wall -Wextra -O2)
endif()

# Unit tests (plain asserts, no framework).
add_executable(test_order_book tests/test_order_book.cpp)
target_link_libraries(test_order_book PRIVATE itch_engine_core)
add_test(NAME order_book COMMAND test_order_book)

# Python bindings. pybind11 is located automatically by asking the Python
# interpreter for its cmake dir (pip install pybind11 is the only setup);
# an explicit -Dpybind11_DIR=... still overrides if provided.
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
  message(STATUS "pybind11 not found - building C++ core and tests only")
endif()
