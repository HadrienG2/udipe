# - Our minimal support target is the CMake version distributed by the Ubuntu
#   LTS that we use in production, currently 24.04.
# - Our latest tested target is the CMake version distributed by Arch Linux,
#   4.1.2 at the time of writing. (TODO: Update regularly)
cmake_minimum_required(VERSION 3.28.3..4.1.2)
include(GNUInstallDirs)

project(Udipe
        VERSION 1.0
        DESCRIPTION "Solving the riddle of high-throughput UDP"
        LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# TODO: Options, find_package() etc

add_library(libudipe src/log.c)
add_library(Udipe::libudipe ALIAS libudipe)
target_include_directories(libudipe
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:include>)
set_target_properties(libudipe PROPERTIES PUBLIC_HEADER include/udipe.h)

# TODO: Add unit tests doxygen CPack etc

install(TARGETS libudipe EXPORT UdipeTargets)
include(cmake/StupidConfigBoilerplate.cmake)
