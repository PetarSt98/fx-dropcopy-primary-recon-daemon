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

  foreach(_root IN LISTS _AERON_ROOT_CANDIDATES)
    if(_root)
      foreach(_hdr_candidate
              "${_root}/include/aeron/Aeron.h"
              "${_root}/include/uk/co/real_logic/aeron/Aeron.h"
              "${_root}/include/Aeron.h"
              "${_root}/aeron/Aeron.h")
        if(EXISTS "${_hdr_candidate}" AND NOT AERON_INCLUDE_DIR)
          get_filename_component(AERON_INCLUDE_DIR "${_hdr_candidate}" DIRECTORY)
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

  add_library(aeron::aeron_client UNKNOWN IMPORTED)
  set_target_properties(aeron::aeron_client PROPERTIES
    IMPORTED_LOCATION "${AERON_CLIENT_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${AERON_INCLUDE_DIR}"
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
