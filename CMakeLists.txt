### Basic CMake configuration ###

# - Our minimal support target is the CMake version distributed by the Ubuntu
#   LTS that we use in production, currently 24.04.
# - Our latest tested target is the CMake version distributed by Arch Linux,
#   4.2.0 at the time of writing. (TODO: Update regularly)
cmake_minimum_required(VERSION 3.28.3..4.2.0)

project(Udipe
        VERSION 1.0
        DESCRIPTION "Solving the riddle of high-throughput UDP"
        HOMEPAGE_URL https://hadrieng2.github.io/udipe
        LANGUAGES C)

set_property(GLOBAL PROPERTY C_STANDARD 11)
set_property(GLOBAL PROPERTY C_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY C_EXTENSIONS ON)
if(NOT WIN32)
    set_property(GLOBAL PROPERTY C_VISIBILITY_PRESET hidden)
endif()

include(GNUInstallDirs)

if(NOT WIN32)
    # Rationale for disabling some warnings:
    #
    # - unused-parameter doesn't work when some function parameters are only
    #   used for debug assertions and unused in Release builds.
    # - unused-value breaks the comma trick for documenting debug assertions.
    # - unused-variable and unused-but-set-variable similarly don't work well
    #   with debug assertions over a function result, where you want the
    #   function to execute even in Release builds.
    add_compile_options(-Wall -Wextra -Wduplicated-branches -Wduplicated-cond
                        -Wnull-dereference -Wdouble-promotion
                        -Wno-unused-parameter -Wno-unused-value
                        -Wno-unused-variable
                        -Wno-unused-but-set-variable)
endif()


### Optional features ###

option(UDIPE_WERROR "Treat GCC and doxygen warnings as errors" OFF)
if (UDIPE_WERROR AND NOT WIN32)
    add_compile_options(-Werror)
endif()

# These options are only available in standalone builds, not vendored ones
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    option(UDIPE_BUILD_BENCHMARKS
           "Build the performance benchmarks"
           ON)

    option(UDIPE_BUILD_DOXYGEN "Build the Doxygen documentation" OFF)
    option(UDIPE_BUILD_DOXYGEN_INTERNAL
           "Allow UDIPE_BUILD_DOXYGEN to cover implementation details"
           OFF)
    mark_as_advanced(UDIPE_BUILD_DOXYGEN_INTERNAL)
    if(UDIPE_BUILD_DOXYGEN_INTERNAL)
        set(UDIPE_BUILD_DOXYGEN ON)
    endif()

    option(UDIPE_BUILD_EXAMPLES
           "Build the usage examples"
           OFF)

    include(CTest)
    option(UDIPE_BUILD_TESTS "Build the tests" OFF)
    if(BUILD_TESTING)
        set(UDIPE_BUILD_TESTS ON)
    endif()
    if(UDIPE_BUILD_TESTS)
        # The benchmarking harness is complex enough to warrant some tests
        set(UDIPE_BUILD_BENCHMARKS ON)
        # Code examples do double duty as doctests
        set(UDIPE_BUILD_EXAMPLES ON)
    endif()
endif()


### Dependencies ###

# Need hwloc, and pkg-config to find hwloc
find_package(PkgConfig REQUIRED)
pkg_check_modules(hwloc REQUIRED IMPORTED_TARGET hwloc>=2.0)

# Need doxygen to build the API reference documentation
if(UDIPE_BUILD_DOXYGEN)
    find_package(Doxygen REQUIRED)
endif()


### libudipe ###

add_library(udipe)
add_library(Udipe::udipe ALIAS udipe)
set(udipe_public_headers include/udipe.h
                         include/udipe/buffer.h
                         include/udipe/command.h
                         include/udipe/connect.h
                         include/udipe/context.h
                         include/udipe/future.h
                         include/udipe/log.h
                         include/udipe/pointer.h
                         include/udipe/result.h
                         include/udipe/time.h
                         include/udipe/visibility.h)
target_sources(udipe
               PUBLIC FILE_SET HEADERS
                      BASE_DIRS include
                      FILES ${udipe_public_headers}
                            include/udipe/benchmark.h
                            include/udipe/unit_tests.h
               PRIVATE src/arch.h
                       src/address_wait.c
                       src/address_wait.h
                       src/benchmark.c
                       src/benchmark.h
                       src/benchmark/distribution.c
                       src/benchmark/distribution.h
                       src/benchmark/distribution_log.c
                       src/benchmark/distribution_log.h
                       src/benchmark/numeric.c
                       src/benchmark/numeric.h
                       src/benchmark/outlier_filter.c
                       src/benchmark/outlier_filter.h
                       src/benchmark/statistics.c
                       src/benchmark/statistics.h
                       src/bit_array.c
                       src/bit_array.h
                       src/bits.c
                       src/bits.h
                       src/buffer.c
                       src/buffer.h
                       src/buffer.c
                       src/buffer.h
                       src/command.c
                       src/command.h
                       src/connect.h
                       src/connect.c
                       src/context.c
                       src/context.h
                       src/error.c
                       src/error.h
                       src/format.h
                       src/future.h
                       src/log.c
                       src/log.h
                       src/memory.c
                       src/memory.h
                       src/name_filter.c
                       src/name_filter.h
                       src/thread_name.c
                       src/thread_name.h
                       src/unit_tests.c
                       src/unit_tests.h
                       src/visibility.h)
if (UDIPE_BUILD_TESTS)
    target_compile_definitions(udipe PUBLIC UDIPE_BUILD_BENCHMARKS)
endif()
if (UDIPE_BUILD_TESTS)
    target_compile_definitions(udipe PUBLIC UDIPE_BUILD_TESTS)
endif()
target_include_directories(udipe
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_link_libraries(udipe PRIVATE m PkgConfig::hwloc)
install(TARGETS udipe EXPORT UdipeTargets FILE_SET HEADERS)


### Doxygen documentation ###

if (UDIPE_BUILD_DOXYGEN)
    set(DOXYGEN_EXTRACT_STATIC YES)
    set(DOXYGEN_OPTIMIZE_OUTPUT_FOR_C YES)
    set(DOXYGEN_REPEAT_BRIEF NO)
    set(DOXYGEN_WARN_IF_UNDOC_ENUM_VAL YES)
    if (UDIPE_WERROR)
        # TODO: Re-enable once command interface is documented
        # set(DOXYGEN_WARN_AS_ERROR YES)
    endif()

    set(documented_source ${udipe_public_headers})
    if (UDIPE_BUILD_DOXYGEN_INTERNAL)
        set(DOXYGEN_INTERNAL_DOCS YES)
        set(DOXYGEN_SOURCE_BROWSER YES)
        get_target_property(private_source udipe SOURCES)
        list(APPEND documented_source ${private_source})
    else()
        set(DOXYGEN_MAX_INITIALIZER_LINES 2)
    endif()
    set(DOXYGEN_EXAMPLE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/examples)

    doxygen_add_docs(doxygen ${documented_source} ALL USE_STAMP_FILE)
endif()


### Performance benchmarks ###

if(UDIPE_BUILD_BENCHMARKS)
    add_subdirectory(benches)
endif()


### Code examples ###

if(UDIPE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()


### Unit tests ###

if(UDIPE_BUILD_TESTS)
    add_subdirectory(tests)
endif()


### Packaging for downstream users ###

include(cmake/StupidConfigBoilerplate.cmake)
include(cmake/StupidCPackBoilerplate.cmake)
