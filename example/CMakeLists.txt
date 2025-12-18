cmake_minimum_required(VERSION 4.2)

project(tftp-server-example
    LANGUAGES CXX
)

find_package(tftp 0.1 REQUIRED)
if (tftp_FOUND)
    message(STATUS "tftp found: ${tftp_VERSION}")
else()
    message(FATAL_ERROR "tftp not found")
endif()

add_executable(tftp_server_example)
target_sources(tftp_server_example
    PRIVATE
        main.cpp
)

target_link_libraries(tftp_server_example 
    PRIVATE
        tftp::tftp_core
)