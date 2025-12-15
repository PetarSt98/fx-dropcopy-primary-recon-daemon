cmake_minimum_required(VERSION 3.20)
project(fx_dropcopy_recon CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)

add_library(fx_core
    src/ingest/spsc_ring.hpp
    src/ingest/fix_parser.cpp
    src/core/exec_event.hpp
    src/core/wire_exec_event.hpp
    src/core/reconciler.cpp
    src/util/rdtsc.hpp
    src/util/log.hpp
    src/util/soh.hpp
    src/util/arena.hpp
)

target_include_directories(fx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Try config-based discovery first; fall back to manual lookup if Aeron does not
# ship a CMake package config in the current environment (e.g., custom install
# from source).
find_package(aeron CONFIG QUIET)
if(NOT aeron_FOUND)
  message(STATUS "Aeron config package not found; attempting manual discovery")

  set(_AERON_ROOT_CANDIDATES
      $ENV{AERON_ROOT}
      $ENV{AERON_HOME}
      ${CMAKE_PREFIX_PATH}
      /usr/local
      /usr
      /opt/homebrew
      /opt/local
  )

  set(AERON_INCLUDE_DIR "")
  set(AERON_CLIENT_LIB "")
  set(AERON_HEADER_PATH "")

  foreach(_root IN LISTS _AERON_ROOT_CANDIDATES)
    if(_root)
      foreach(_hdr_candidate
              "${_root}/include/aeron/Aeron.h"
              "${_root}/include/uk/co/real_logic/aeron/Aeron.h"
              "${_root}/include/Aeron.h"
              "${_root}/aeron/Aeron.h")
        if(EXISTS "${_hdr_candidate}" AND NOT AERON_HEADER_PATH)
          set(AERON_HEADER_PATH "${_hdr_candidate}")
          get_filename_component(_hdr_dir "${_hdr_candidate}" DIRECTORY)
          get_filename_component(_hdr_parent "${_hdr_dir}" DIRECTORY)
          # Prefer the parent of the aeron/ directory when present so <aeron/Aeron.h>
          # resolves correctly.
          if(_hdr_candidate MATCHES "/include/aeron/Aeron.h$")
            set(AERON_INCLUDE_DIR "${_hdr_parent}")
          else()
            set(AERON_INCLUDE_DIR "${_hdr_dir}")
          endif()
        endif()
      endforeach()

      foreach(_lib_dir "${_root}" "${_root}/lib" "${_root}/lib64" "${_root}/lib/x86_64-linux-gnu")
        foreach(_lib_name libaeron_client.so libaeron_client.dylib libaeron_client.a libaeron.so libaeron.dylib libaeron.a)
          if(EXISTS "${_lib_dir}/${_lib_name}" AND NOT AERON_CLIENT_LIB)
            set(AERON_CLIENT_LIB "${_lib_dir}/${_lib_name}")
          endif()
        endforeach()
      endforeach()
    endif()
  endforeach()

  if(NOT AERON_INCLUDE_DIR OR NOT AERON_CLIENT_LIB)
    message(FATAL_ERROR "Aeron client not found. Set aeron_DIR, AERON_ROOT, or install the library so aeron/Aeron.h and libaeron_client are discoverable.")
  endif()

  # Resolve the header for debugging clarity and to catch layout surprises early.
  find_file(AERON_HEADER_PATH
            NAMES aeron/Aeron.h uk/co/real_logic/aeron/Aeron.h Aeron.h
            HINTS "${AERON_INCLUDE_DIR}" "${AERON_INCLUDE_DIR}/include" "${AERON_INCLUDE_DIR}/aeron"
            NO_CACHE)

  if(NOT AERON_HEADER_PATH)
    message(FATAL_ERROR "Aeron header not found under ${AERON_INCLUDE_DIR}; expected aeron/Aeron.h")
  endif()

  message(STATUS "Aeron include dir: ${AERON_INCLUDE_DIR}")
  message(STATUS "Aeron client lib: ${AERON_CLIENT_LIB}")
  message(STATUS "Aeron header: ${AERON_HEADER_PATH}")
  message(STATUS "Aeron header found: ${AERON_HEADER_PATH}")

  # If Aeron is installed "flat" (e.g. /usr/local/include/Aeron.h) but sources
  # include <aeron/Aeron.h>, generate a small shim include dir that provides
  # aeron/*.h wrappers mapping to the flat headers.
    set(AERON_INCLUDE_DIRS "${AERON_INCLUDE_DIR}")
    if(AERON_HEADER_PATH MATCHES "/include/Aeron.h$" AND NOT EXISTS "${AERON_INCLUDE_DIR}/aeron/Aeron.h")
    set(AERON_SHIM_DIR "${CMAKE_BINARY_DIR}/aeron_include_shim")
    file(MAKE_DIRECTORY "${AERON_SHIM_DIR}/aeron")

    # Always provide aeron/Aeron.h
    file(WRITE "${AERON_SHIM_DIR}/aeron/Aeron.h" "#pragma once\n#include <Aeron.h>\n")

    # Optional: wrap all other flat headers too
    file(GLOB _AERON_FLAT_HEADERS "${AERON_INCLUDE_DIR}/*.h")
    foreach(_h IN LISTS _AERON_FLAT_HEADERS)
        get_filename_component(_hn "${_h}" NAME)
        if(NOT _hn STREQUAL "Aeron.h")
        file(WRITE "${AERON_SHIM_DIR}/aeron/${_hn}" "#pragma once\n#include <${_hn}>\n")
        endif()
    endforeach()

    list(PREPEND AERON_INCLUDE_DIRS "${AERON_SHIM_DIR}")
    endif()

  # Prove the include path is usable before the main build to avoid late failures.
  set(_AERON_TRY_FLAGS "")
  foreach(_inc IN LISTS _AERON_INCLUDE_DIRS)
    string(APPEND _AERON_TRY_FLAGS " -I\"${_inc}\"")
  endforeach()
  string(STRIP "${_AERON_TRY_FLAGS}" _AERON_TRY_FLAGS)

    include(CheckCXXSourceCompiles)

    set(CMAKE_REQUIRED_INCLUDES ${AERON_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_FLAGS "-std=c++23")
    check_cxx_source_compiles(
    "#include <aeron/Aeron.h>\nint main(){return 0;}\n"
    AERON_HEADER_COMPILES
    )
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_FLAGS)

    if(NOT AERON_HEADER_COMPILES)
    message(FATAL_ERROR
        "Failed to compile a test TU including <aeron/Aeron.h> with include paths: ${AERON_INCLUDE_DIRS}"
    )
    endif()

  add_library(aeron::aeron_client UNKNOWN IMPORTED)
  set_target_properties(aeron::aeron_client PROPERTIES
    IMPORTED_LOCATION "${AERON_CLIENT_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${AERON_INCLUDE_DIRS}"
  )
endif()

add_executable(fx_ingest_demo src/api/demo_main.cpp src/ingest/fix_parser.cpp src/core/reconciler.cpp)
target_link_libraries(fx_ingest_demo PRIVATE fx_core)

add_executable(fx_exec_recond
    src/api/recond_main.cpp
    src/ingest/aeron_subscriber.cpp
    src/core/reconciler.cpp
)
target_link_libraries(fx_exec_recond PRIVATE fx_core aeron::aeron_client)

add_executable(unit_tests
    tests/all_tests.cpp
    tests/ring_tests.cpp
    tests/fix_parser_tests.cpp
    tests/from_wire_tests.cpp
    tests/test_main.hpp
    src/ingest/fix_parser.cpp
    src/core/reconciler.cpp
)
target_include_directories(unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(unit_tests PRIVATE fx_core)

add_executable(benchmarks bench/bench_main.cpp src/ingest/fix_parser.cpp src/core/reconciler.cpp)
target_link_libraries(benchmarks PRIVATE fx_core)

enable_testing()
add_test(NAME unit_tests COMMAND unit_tests)
