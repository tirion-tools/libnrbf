cmake_minimum_required(VERSION 3.16)
project(nrbf
    VERSION 0.1.0
    DESCRIPTION "Clean-room .NET BinaryFormatter (NRBF) reader"
    LANGUAGES CXX
)

option(NRBF_BUILD_TESTS "Build libnrbf tests" ON)
option(NRBF_INSTALL "Install libnrbf targets" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(nrbf STATIC src/reader.cpp)
add_library(nrbf::nrbf ALIAS nrbf)

target_include_directories(nrbf
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(nrbf PUBLIC cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nrbf PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(NRBF_BUILD_TESTS)
    enable_testing()
    add_executable(nrbf_dump tests/test_dump.cpp)
    target_link_libraries(nrbf_dump PRIVATE nrbf)
endif()

if(NRBF_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS nrbf EXPORT nrbfTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT nrbfTargets
        FILE nrbfTargets.cmake
        NAMESPACE nrbf::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nrbf)

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/nrbfConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/nrbfConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/nrbfConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nrbf)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/nrbfConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/nrbfConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nrbf)
endif()
