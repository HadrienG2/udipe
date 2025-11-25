#pragma once

//! \file
//! \brief Pointer shenanigans
//!
//! This header provides a couple of utilities that can be used to assert that
//! pointers cannot be `NULL`, in a manner that may affect code optimizations.


/// Assertion that none of a function's pointer arguments can be `NULL`
///
/// This may affect compiler optimizations, and is otherwise a useful
/// documentation hint.
#ifdef __GNUC__
    #define UDIPE_NON_NULL_ARGS __attribute__((nonnull))
#else
    #define UDIPE_NON_NULL_ARGS
#endif

/// Assertion that some of a function's pointer arguments cannot be `NULL`
///
/// The arguments are designated using 1-based indexing, i.e. 1 is the first
/// argument, 2 is the second argument, etc.
///
/// This may affect compiler optimizations, and is otherwise a useful
/// documentation hint.
#ifdef __GNUC__
    #define UDIPE_NON_NULL_SPECIFIC_ARGS(...) __attribute__((nonnull(__VA_ARGS__)))
#else
    #define UDIPE_NON_NULL_SPECIFIC_ARGS(...)
#endif

/// Assertion that a function's pointer result cannot be `NULL`
///
/// This may affect compiler optimizations, and is otherwise a useful
/// documentation hint.
#ifdef __GNUC__
    #define UDIPE_NON_NULL_RESULT __attribute__((returns_nonnull))
#else
    #define UDIPE_NON_NULL_RESULT
#endif
