### Basic CMake configuration ###

# - Our minimal support target is the CMake version distributed by the Ubuntu
#   LTS that we use in production, currently 24.04.
# - Our latest tested target is the CMake version distributed by Arch Linux,
#   4.1.2 at the time of writing. (TODO: Update regularly)
cmake_minimum_required(VERSION 3.28.3..4.1.2)

project(Udipe
        VERSION 1.0
        DESCRIPTION "Solving the riddle of high-throughput UDP"
        HOMEPAGE_URL https://gitlab.in2p3.fr/grasland/udipe
        LANGUAGES C)

set_property(GLOBAL PROPERTY C_STANDARD 11)
set_property(GLOBAL PROPERTY C_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY C_EXTENSIONS ON)
set_property(GLOBAL PROPERTY C_VISIBILITY_PRESET hidden)

include(GNUInstallDirs)

add_compile_options(-Wall -Wextra -Wconversion -Wduplicated-cond
                    -Wduplicated-branches -Wnull-dereference -Wdouble-promotion
                    -Wno-unused-value)


### Optional features ###

option(UDIPE_BUILD_DOXYGEN "Build the Doxygen documentation" OFF)

option(UDIPE_WERROR "Treat GCC and doxygen warnings as errors" OFF)
if (UDIPE_WERROR)
    add_compile_options(-Werror)
endif()

# These options are only available in standalone builds, not vendored ones
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    option(UDIPE_BUILD_DOXYGEN_INTERNAL
           "Allow UDIPE_BUILD_DOXYGEN to cover implementation details"
           OFF)
    mark_as_advanced(UDIPE_BUILD_DOXYGEN_INTERNAL)
    if(UDIPE_BUILD_DOXYGEN_INTERNAL)
        set(UDIPE_BUILD_DOXYGEN ON)
    endif()

    option(UDIPE_BUILD_EXAMPLES
           "Build the code examples"
           ON)

    include(CTest)
    option(UDIPE_BUILD_TESTS "Build the tests" OFF)
    if(BUILD_TESTING)
        set(UDIPE_BUILD_TESTS ON)
    endif()
    if(UDIPE_BUILD_TESTS)
        # Some code examples act as doctests
        set(UDIPE_BUILD_EXAMPLES ON)
    endif()
endif()


### Dependency management ###

if(UDIPE_BUILD_DOXYGEN)
    find_package(Doxygen REQUIRED)
endif()


### libudipe ###

add_library(udipe)
add_library(Udipe::udipe ALIAS udipe)
set(udipe_relative_headers include/udipe.h
                           include/udipe/context.h
                           include/udipe/log.h
                           include/udipe/pointer.h
                           include/udipe/visibility.h)
target_sources(udipe
               PUBLIC FILE_SET HEADERS
                      BASE_DIRS include
                      FILES ${udipe_relative_headers}
               PRIVATE src/context.c
                       src/context.h
                       src/error.c
                       src/error.h
                       src/log.c
                       src/log.h)
target_include_directories(udipe
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
install(TARGETS udipe EXPORT UdipeTargets FILE_SET HEADERS)


### Doxygen documentation ###

if (UDIPE_BUILD_DOXYGEN)
    set(DOXYGEN_EXTRACT_STATIC 1)
    set(DOXYGEN_OPTIMIZE_OUTPUT_FOR_C 1)
    set(DOXYGEN_REPEAT_BRIEF 0)
    set(DOXYGEN_WARN_IF_UNDOC_ENUM_VAL 1)
    if (UDIPE_WERROR)
        set(DOXYGEN_WARN_AS_ERROR)
    endif()

    set(documented_source ${udipe_relative_headers})
    if (UDIPE_BUILD_DOXYGEN_INTERNAL)
        set(DOXYGEN_INTERNAL_DOCS 1)
        set(DOXYGEN_SOURCE_BROWSER 1)
        get_target_property(private_source udipe SOURCES)
        list(APPEND documented_source ${private_source})
    else()
        set(DOXYGEN_MAX_INITIALIZER_LINES 2)
    endif()
    set(DOXYGEN_EXAMPLE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/examples)

    doxygen_add_docs(doxygen ${documented_source} ALL USE_STAMP_FILE)
endif()


### Code examples ###

if(UDIPE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()


### Unit tests ###

if(UDIPE_BUILD_TESTS)
    add_subdirectory(tests)
endif()


# TODO: Add benchmarks etc


### Packaging for downstream users ###

include(cmake/StupidConfigBoilerplate.cmake)
include(cmake/StupidCPackBoilerplate.cmake)
