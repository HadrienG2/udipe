#pragma once

//! \file
//! \brief Name-based filtering of benchmarks and unit tests
//!
//! Microbenchmarks and unit tests use this code module to let you select which
//! tests/benchmarks within a set will run.

#include <udipe/pointer.h>

#include <stdbool.h>


/// Name-based filter
///
/// For now, this is just a substring that is searched within the test/benchmark
/// name to decide if it will be kept or not. It may become a more sophisticated
/// compiled regex later on.
///
/// Build it with name_filter_initialize(), apply it with name_filter_apply()
/// and liberate it with name_filter_finalize().
typedef char* name_filter_t;

/// Set up a name filter based on a user-specified textual key
///
/// This function must be called within the scope of with_logger().
///
/// \param key is a user-specified string that should be taken as the first and
///            only optional positional CLI argument of test and benchmark
///            binaries, with "" as the default value.
/// \returns a name filter that must be liberated with name_filter_liberate()
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
name_filter_t name_filter_initialize(const char* key);

/// Check if a test/benchmark name passes the name filter
///
/// This function must be called within the scope of with_logger().
///
/// \param filter is a name filter that was previously built with
///               name_filter_initialize() and hasn't been liberated with
///               name_filter_liberate() yet
/// \param name is the name of the test/benchmark. For parametrized
///             tests/benchmarks, it should be the full name including
///             parameters, so that only one set of parameters can be run.
/// \returns the truth that `name` passes `filter` and should execute
UDIPE_NON_NULL_ARGS
bool name_filter_matches(name_filter_t filter, const char* name);

/// Shortcut for calling a parameterless function if its name passes the filter
///
/// \param filter is a name filter that was previously built with
///               name_filter_initialize() and hasn't been liberated with
///               name_filter_liberate() yet
/// \param func is the name of a parameterless function that should be called if
///             its name passes the filter.
#define NAME_FILTERED_CALL(filter, func)  \
    if (name_filter_matches(filter, #func)) func()

/// Liberate a name filter
///
/// This function must be called within the scope of with_logger().
///
/// \param filter is a name filter that was previously built with
///               name_filter_initialize() and hasn't been liberated with
///               name_filter_liberate() yet
UDIPE_NON_NULL_ARGS
void name_filter_finalize(name_filter_t* filter);


#ifdef UDIPE_BUILD_TESTS
    /// Unit tests
    ///
    /// This function runs all the unit tests for this module. It must be called
    /// within the scope of with_logger().
    void name_filter_unit_tests();
#endif