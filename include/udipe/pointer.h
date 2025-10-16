#pragma once

//! \file
//! \brief Pointer shenanigans
//!
//! This header provides a couple of utilities that can be used to assert that
//! pointers cannot be `NULL`, in a manner that may affect code optimizations.


/// Assertion that a function's pointer arguments cannot be `NULL`
///
/// This may affect compiler optimizations, and is otherwise a useful
/// documentation hint.
#define UDIPE_NON_NULL_ARGS __attribute__((nonnull))

/// Assertion that a function's pointer result cannot be `NULL`
///
/// This may affect compiler optimizations, and is otherwise a useful
/// documentation hint.
#define UDIPE_NON_NULL_RESULT __attribute__((returns_nonnull))