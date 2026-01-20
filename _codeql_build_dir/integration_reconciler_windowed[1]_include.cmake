if(EXISTS "/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/integration_reconciler_windowed")
  if(NOT EXISTS "/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/integration_reconciler_windowed[1]_tests.cmake" OR
     NOT "/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/integration_reconciler_windowed[1]_tests.cmake" IS_NEWER_THAN "/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/integration_reconciler_windowed" OR
     NOT "/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/integration_reconciler_windowed[1]_tests.cmake" IS_NEWER_THAN "${CMAKE_CURRENT_LIST_FILE}")
    include("/usr/local/share/cmake-3.31/Modules/GoogleTestAddTests.cmake")
    gtest_discover_tests_impl(
      TEST_EXECUTABLE [==[/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/integration_reconciler_windowed]==]
      TEST_EXECUTOR [==[]==]
      TEST_WORKING_DIR [==[/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir]==]
      TEST_EXTRA_ARGS [==[]==]
      TEST_PROPERTIES [==[]==]
      TEST_PREFIX [==[integration_reconciler_windowed_]==]
      TEST_SUFFIX [==[]==]
      TEST_FILTER [==[]==]
      NO_PRETTY_TYPES [==[FALSE]==]
      NO_PRETTY_VALUES [==[FALSE]==]
      TEST_LIST [==[integration_reconciler_windowed_TESTS]==]
      CTEST_FILE [==[/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/integration_reconciler_windowed[1]_tests.cmake]==]
      TEST_DISCOVERY_TIMEOUT [==[5]==]
      TEST_DISCOVERY_EXTRA_ARGS [==[]==]
      TEST_XML_OUTPUT_DIR [==[]==]
    )
  endif()
  include("/home/runner/work/fx-dropcopy-primary-recon-daemon/fx-dropcopy-primary-recon-daemon/_codeql_build_dir/integration_reconciler_windowed[1]_tests.cmake")
else()
  add_test(integration_reconciler_windowed_NOT_BUILT integration_reconciler_windowed_NOT_BUILT)
endif()
