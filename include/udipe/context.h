#pragma once

//! \file
//! \brief Core `libudipe` context
//!
//! This header is the home of \ref udipe_context_t, the core context object
//! that you will need for any nontrivial interaction with the `libudipe` API.
//!
//! It also provides the following related tools:
//!
//! - udipe_initialize(), the function that builds \ref udipe_context_t, which
//!   you must call during the initialization stage of your application
//! - \ref udipe_config_t, the configurable parameters of udipe_initialize()
//! - udipe_finalize(), the function that destroys \ref udipe_context_t, which
//!   you must call during the finalization stage of your application.

//! \example context_lifecycle.c
//!
//! This is a minimal `libudipe` application skeleton: set up a \ref
//! udipe_context_t using udipe_initialize() in the default configuration, then
//! tear it down using udipe_finalize().

#include "allocator.h"
#include "log.h"
#include "pointer.h"
#include "visibility.h"


/// Core `libudipe` configuration
///
/// This data structure is used to configure the behavior of udipe_initialize().
/// It is designed such that zero-initializing it with memset() should result in
/// sane defaults for many applications.
typedef struct udipe_config_s {
    /// Logging configuration
    ///
    /// This member controls `libudipe`'s logging behavior. By default, status
    /// messages are logged to `stderr` when they have priority >= \link
    /// #UDIPE_INFO `INFO` \endlink, and in `Debug` builds messages of priority
    /// \link #UDIPE_DEBUG `DEBUG` \endlink are logged too.
    udipe_log_config_t log;

    /// Memory management configuration
    ///
    /// This member controls `libudipe`'s memory management behavior. By
    /// default, worker threads attempt to achieve good cache locality while
    /// handling a fair amount of concurrent requests by dedicating an L1-sized
    /// cache budget to each request and an L2-sized cache budget to the set of
    /// all concurrently handled requests.
    udipe_allocator_config_t allocator;
} udipe_config_t;

/// Core `libudipe` context
///
/// A pointer to this opaque data structure is built by udipe_initialize() and
/// can subsequently be passed to most `libudipe` API entry points for the
/// purpose of performing UDP network operations.
///
/// Its content is an opaque implementation detail of `libudipe` that you should
/// not attempt to read or modify.
///
/// Once you are done with `libudipe`, you can pass this object back to
/// udipe_finalize() to destroy it.
typedef struct udipe_context_s udipe_context_t;

/// Initialize a \link #udipe_context_t `libudipe` context \endlink
///
/// You should normally only need to call this function once at the start of
/// your application. It is configured using a \ref udipe_config_t data
/// structure, which is designed to be zero-initialization safe, and it produces
/// the opaque \link #udipe_context_t `udipe_context_t*` \endlink pointer that
/// you will need to use most other functions from `libudipe`
///
/// You must not attempt to read or modify the resulting \ref udipe_context_t
/// object in any way until you are done with `libudipe`, at which point you
/// must pass it to udipe_finalize() to safely destroy it before the application
/// terminates.
///
/// This function currently only has fatal error cases, which it handles using
/// `exit(EXIT_FAILURE)`. It is therefore guaranteed to return a non-null
/// pointer if it returns at all.
UDIPE_PUBLIC
UDIPE_NON_NULL_RESULT
udipe_context_t* udipe_initialize(udipe_config_t config);

/// Finalize a \link #udipe_context_t `libudipe` context \endlink
///
/// This function cancels all unfinished `libudipe` transactions, waits for
/// uninterruptible asynchronous work to complete, and liberates the resources
/// formerly allocated by udipe_initialize().
///
/// Although udipe_finalize() may take a short amount of time to complete, its
/// pointer invalidation effect should be considered instantaneous: starting
/// from the moment where you _start_ calling this function, you must not call
/// any `libudipe` function with this `udipe_context_t*` parameter from any of
/// your application threads.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
void udipe_finalize(udipe_context_t* context);
