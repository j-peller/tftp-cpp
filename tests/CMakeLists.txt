find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

# --- Unit & C++ loopback tests ----------------------------------------------
add_executable(tftp_unit_tests
    test_packet.cpp
)
target_link_libraries(tftp_unit_tests
    PRIVATE
        tftp::tftp_core
        GTest::gtest_main
)
gtest_discover_tests(tftp_unit_tests)

# --- End-to-end tests against the Python peers in tools/ --------------------
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(TFTP_TEST_DEFINITIONS
    PYTHON_EXE="${Python3_EXECUTABLE}"
    TFTP_TEST_CLIENT_SCRIPT="${PROJECT_SOURCE_DIR}/tools/tftp_test_client.py"
    TFTP_TEST_SERVER_SCRIPT="${PROJECT_SOURCE_DIR}/tools/tftp_test_server.py"
)

add_executable(tftp_server_tests
    test_server.cpp
)
target_link_libraries(tftp_server_tests
    PRIVATE
        tftp::tftp_core
        GTest::gtest_main
)
target_compile_definitions(tftp_server_tests PRIVATE ${TFTP_TEST_DEFINITIONS})
gtest_discover_tests(tftp_server_tests)

add_executable(tftp_client_tests
    test_client.cpp
)
target_link_libraries(tftp_client_tests
    PRIVATE
        tftp::tftp_core
        GTest::gtest_main
)
target_compile_definitions(tftp_client_tests PRIVATE ${TFTP_TEST_DEFINITIONS})
gtest_discover_tests(tftp_client_tests)
