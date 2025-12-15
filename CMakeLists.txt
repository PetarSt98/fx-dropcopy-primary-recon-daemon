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

find_package(aeron REQUIRED)

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
