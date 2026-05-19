#pragma once

//! \file
//! \brief Core context
//!
//! This code module defines \ref udipe_context_t, the core API context of
//! `libudipe`, along with its basic lifecycle functions udipe_initialize() and
//! udipe_finalize(). Most other `libudipe` functions use this struct to access
//! core `libudipe` state like the message logger, the network thread pool, etc.

#include <udipe/context.h>

#include "future/allocator/context_cache.h"

#include "connect.h"
#include "log.h"

#include <hwloc.h>
#include <stdatomic.h>
#include <threads.h>


/// \copydoc udipe_context_t
struct udipe_context_s {
    /// Message logger
    ///
    /// Any public `libudipe` function or network thread should begin by using
    /// the with_logger() macro to set up a logging scope with this logger. This
    /// allows logging functions to subsequently be used in order to report
    /// normal and suspicious events throughout the application lifecycle for
    /// the sake of easier application and `libudipe` debugging.
    logger_t logger;

    /// Handle to the thread-local future resource cache
    ///
    /// This key refers to a \ref future_thread_cache_t, which the future
    /// allocator will start by querying. If some resources are missing there,
    /// `global_future_cache` will be looked up.
    //
    // TODO: Set this up in context constructor
    tss_t thread_future_cache;

    /// hwloc topology
    ///
    /// Used to query the CPU topology (cache sizes, NUMA etc) and pin threads
    /// to CPU cores.
    hwloc_topology_t topology;

    /// Context-global future resource cache
    ///
    /// The future allocator will query this cache if `local_future_cache` does
    /// not have some resources that it needs.
    //
    // TODO: Set this up in context constructor
    future_context_cache_t global_future_cache;

    /// Allocator of \ref shared_connect_options_t
    ///
    /// See \ref connect.h for more info on how this works and how to use it.
    connect_options_allocator_t connect_options;
};

// TODO: Add constructor for context setup
