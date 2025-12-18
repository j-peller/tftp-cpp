cmake_minimum_required(VERSION 4.2)
project(tftp 
    VERSION 0.1.0 
    LANGUAGES CXX
)

include(GNUInstallDirs)

add_library(tftp_core)
add_library(tftp::tftp_core ALIAS tftp_core)

target_sources(tftp_core
    PRIVATE
        src/socket.cpp
        src/packet.cpp
        src/server.cpp
    PUBLIC
        FILE_SET 
            HEADERS
        BASE_DIRS 
            include
        FILES
            include/tftp/socket.h
            include/tftp/packet.h
            include/tftp/server.h
)

target_compile_features(tftp_core PUBLIC cxx_std_17)

target_include_directories(tftp_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

find_package(Threads REQUIRED)
target_link_libraries(tftp_core PUBLIC Threads::Threads)


install(
    TARGETS tftp_core
    EXPORT tftpTargets
    FILE_SET HEADERS
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install the export set
install(EXPORT tftpTargets
    FILE tftpTargets.cmake
    NAMESPACE tftp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tftp
)

# Create and install the Config file
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tftpConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/tftpConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tftp
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/tftpConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/tftpConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/tftpConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tftp
)
