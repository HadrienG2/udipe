#pragma once

//! \file
//! \brief Core context
//!
//! This code module defines \ref udipe_context_t, the core API context of
//! `libudipe`, along with its basic lifecycle methods udipe_initialize() and
//! udipe_finalize(). Most other `libudipe` functions use this struct to access
//! core `libudipe` state like the message logger, the network thread pool, etc.

#include <udipe/context.h>

#include "command.h"
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

    /// Futex that tracks which of the `connect_options` are available for use
    ///
    /// This futex is a bitmap where each bit is set to 1 to indicate that the
    /// matching entry of the `connect_options` array is available, or 0 to
    /// indicate that it is currently used as part of some outstanding
    /// connection request to worker threads.
    ///
    /// It should be initialized to an `UINT32_MAX` pattern to indicate that all
    /// 32 entries of `connect_options` are initially available.
    ///
    /// A client thread that is connecting to some remote host and therefore
    /// needs to allocate a \ref shared_connect_options_t for the associated
    /// worker thread \ref command_t must allocate it as follows:
    ///
    /// 1. Atomically load this tracking word (relaxed is fine at this point)
    /// 2. Check if all option structs are used up (word == 0)
    /// 3. If so, go into a FUTEX_WAIT loop until it's not all-zeros anymore,
    ///    then move on to point 4.
    /// 4. If option structs are available, randomly select one, then use
    ///    fetch_and() to try to allocate it (relaxed is still fine at this
    ///    point), then check the result to make sure that no racing client
    ///    concurrently allocated the same options struct.
    /// 5. If allocation succeeded, then do an acquire memory fence and proceed
    ///    to use the struct as documented in \ref shared_connect_options_t.
    /// 6. If allocation failed due to a race with another client thread, then
    ///    go back to 2 and try again or sleep if all structs are now in use.
    ///
    /// A worker thread that is done using some options struct and figures out
    /// that is was its last user must liberate it as follows:
    ///
    /// 1. Use fetch_and() with release memory ordering to atomically set the
    ///    associated bit of this bitfield.
    /// 2. If all options were formerly used up (word == 0), then use
    ///    single-waiter FUTEX_WAKE to wake one of the waiting client threads.
    _Atomic uint32_t connect_options_availability;

    /// Pool of connection options, shared between all client threads
    ///
    /// See \ref shared_connect_options_t and the `connect_options_futex` member
    /// of this struct for more info.
    ///
    /// \internal
    ///
    /// If needed, the size of this array can be reduced, but it cannot
    /// easily grow above 32 due to Linux futex limitations.
    ///
    /// That should be fine for realistic `libudipe` use cases because
    /// attempting to concurrently connect to more than 32 distinct peers should
    /// be exceptional after the initial application startup phase, and even
    /// then 32-way concurrency should be enough to get good system utilization.
    shared_connect_options_t connect_options[32];
};
