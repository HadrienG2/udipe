#pragma once

//! \file
//! \brief Core context
//!
//! This code module defines \ref udipe_context_t, the core API context of
//! `libudipe`, along with its basic lifecycle methods udipe_initialize() and
//! udipe_finalize(). Most other `libudipe` functions use this struct to access
//! core `libudipe` state like the message logger, the network thread pool, etc.

#include <udipe/context.h>

#include "log.h"

#include <hwloc.h>


/// \copydoc udipe_context_t
struct udipe_context_s {
    /// Message logger
    ///
    /// Any public `libudipe` method or network thread should begin by using the
    /// with_logger() macro to set up a logging scope with this logger. This
    /// allows logging methods to subsequently be used in order to report normal
    /// and suspicious events throughout the application lifecycle for the sake
    /// of easier application and `libudipe` debugging.
    logger_t logger;

    /// hwloc topology
    ///
    /// Used to query the CPU topology (cache sizes, NUMA etc) and pin threads
    /// to CPU cores.
    hwloc_topology_t topology;
};
