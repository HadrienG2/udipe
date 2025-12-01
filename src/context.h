#pragma once

//! \file
//! \brief Core context
//!
//! This code module defines \ref udipe_context_t, the core API context of
//! `libudipe`, along with its basic lifecycle functions udipe_initialize() and
//! udipe_finalize(). Most other `libudipe` functions use this struct to access
//! core `libudipe` state like the message logger, the network thread pool, etc.

#include <udipe/context.h>

#include "connect.h"
#include "log.h"

#include <hwloc.h>
#include <stdatomic.h>


/// \copydoc udipe_context_t
//
// TODO: Optimize layout for cache locality of typical requests once the main
//       functionality has been implemented.
struct udipe_context_s {
    /// Message logger
    ///
    /// Any public `libudipe` function or network thread should begin by using
    /// the with_logger() macro to set up a logging scope with this logger. This
    /// allows logging functions to subsequently be used in order to report
    /// normal and suspicious events throughout the application lifecycle for
    /// the sake of easier application and `libudipe` debugging.
    logger_t logger;

    /// hwloc topology
    ///
    /// Used to query the CPU topology (cache sizes, NUMA etc) and pin threads
    /// to CPU cores.
    hwloc_topology_t topology;

    /// Allocator of \ref shared_connect_options_t
    ///
    /// See \ref connect.h for more info on how this works and how to use it.
    connect_options_allocator_t connect_options;
};
