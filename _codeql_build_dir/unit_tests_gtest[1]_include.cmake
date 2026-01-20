if(EXISTS "/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/unit_tests_gtest")
  if(NOT EXISTS "/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/unit_tests_gtest[1]_tests.cmake" OR
     NOT "/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/unit_tests_gtest[1]_tests.cmake" IS_NEWER_THAN "/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/unit_tests_gtest" OR
     NOT "/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/unit_tests_gtest[1]_tests.cmake" IS_NEWER_THAN "${CMAKE_CURRENT_LIST_FILE}")
    include("/usr/local/share/cmake-3.31/Modules/GoogleTestAddTests.cmake")
    gtest_discover_tests_impl(
      TEST_EXECUTABLE [==[/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/unit_tests_gtest]==]
      TEST_EXECUTOR [==[]==]
      TEST_WORKING_DIR [==[/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir]==]
      TEST_EXTRA_ARGS [==[]==]
      TEST_PROPERTIES [==[]==]
      TEST_PREFIX [==[unit_]==]
      TEST_SUFFIX [==[]==]
      TEST_FILTER [==[]==]
      NO_PRETTY_TYPES [==[FALSE]==]
      NO_PRETTY_VALUES [==[FALSE]==]
      TEST_LIST [==[unit_tests_gtest_TESTS]==]
      CTEST_FILE [==[/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/unit_tests_gtest[1]_tests.cmake]==]
      TEST_DISCOVERY_TIMEOUT [==[5]==]
      TEST_DISCOVERY_EXTRA_ARGS [==[]==]
      TEST_XML_OUTPUT_DIR [==[]==]
    )
  endif()
  include("/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/unit_tests_gtest[1]_tests.cmake")
else()
  add_test(unit_tests_gtest_NOT_BUILT unit_tests_gtest_NOT_BUILT)
endif()
