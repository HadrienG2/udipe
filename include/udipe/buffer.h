#pragma once

//! \file
//! \brief Buffering configuration
//!
//! This header is the home of \ref udipe_buffer_config_t, the data structure
//! that configures the buffering policy of `libudipe` worker threads, along
//! with related type and constant definitions.

//! \example configure_buffering.c
//!
//! This example demonstrates a non-default \ref udipe_buffer_config_t setup
//! that configures all worker threads to work with 42 buffers of 9000 bytes.

#include <stddef.h>


/// Maximum number of buffers that a worker thread can manage
///
/// Any attempt to set up a worker thread that manages more than this amount
/// of buffers will fail.
///
/// If automatic configuration logic determines that the optimal amount of
/// buffers is above this limit, then it will log a warning and stick with
/// `UDIPE_MAX_BUFFERS` buffers.
#define UDIPE_MAX_BUFFERS ((size_t)64)

/// Tunable buffering parameters for one worker thread
///
/// This is the value returned by the \ref udipe_buffer_config_callback_t for
/// each worker thread, used to tune each thread's memory management policy.
typedef struct udipe_buffer_config_s {
    /// Size of an individual I/O buffer in bytes
    ///
    /// This controls the size of the buffers within which a worker thread will
    /// hold incoming or outgoing UDP datagrams, or batches thereof when the
    /// GRO/GSO optimization is enabled.
    ///
    /// A value of 0 requests the default buffer size, which is adjusted such
    /// that each buffer fits within the L1 cache of any CPU on which the worker
    /// thread may execute.
    ///
    /// A nonzero value requests a specific buffer size. This buffer size must
    /// be greater than the UDP MTU for any UDP socket that the worker thread is
    /// destined to interact with (9216 bytes being the upper MTU limit for
    /// typical Ethernet equipment if you want a safe default).
    ///
    /// The actual buffer size will be rounded up to the next multiple of the
    /// host system's smallest page size.
    size_t buffer_size;

    /// Number of I/O buffers that a worker thread manages
    ///
    /// This indirectly controls the number of concurrent I/O requests that a
    /// worker thread can start before being forced to wait for pending requests
    /// to complete. It cannot be larger than \ref UDIPE_MAX_BUFFERS.
    ///
    /// A value of 0 requests the default buffer count, which is adjusted such
    /// that there is at least one buffer and the buffers collectively fit...
    ///
    /// - Within the L2 cache of any CPU on which the worker thread may execute,
    ///   if said L2 cache is private (as on x86 CPUs).
    /// - Within an even share of the L2 cache if it is shared across multiple
    ///   CPU cores (as on most Arm CPUs).
    size_t buffer_count;
} udipe_buffer_config_t;

/// Worker thread memory management configuration callback
///
/// You may specify such a callback as part of \ref udipe_buffer_configurator_t
/// in order to tune the buffering policy of individual `libudipe` worker
/// threads.
///
/// It will be invoked by each worker thread on startup (and must therefore be
/// thread-safe since worker threads start concurrently), and it is responsible
/// for returning a \ref udipe_buffer_config_t that adjusts the worker thread's
/// buffering policy. See the documentation of this struct for more info on
/// available tunable parameters.
///
/// The input `void*` parameter is of your choosing. It is specified via \ref
/// udipe_buffer_configurator_t::context and passed in as is to each invocation
/// of the callback. You can use it to pass any information that your callback
/// needs to compute its memory management configuration. For example...
///
/// - When you want to configure all threads in the same manner, you can use it
///   to pass in a \link #udipe_buffer_config_t `const udipe_buffer_config_t*`
///   \endlink that points to the parameters shared by all threads.
/// - When you want to configure threads in a cache locality aware manner, you
///   can use it to pass in external context (e.g. a `hwloc_topology_t`) that is
///   used to figure out relevant cache parameters for the active thread.
///
/// The intent behind this callback-based design is to let you...
///
/// - Adapt to the fact that the number of worker threads that `libudipe` will
///   spawn, and their pinning to CPU cores or lack thereof, is an opaque
///   implementation detail of `libudipe`.
/// - Adjust the tuning parameters on a per-thread basis, which can make sense
///   on systems with heterogeneous CPU cores.
typedef udipe_buffer_config_t (*udipe_buffer_config_callback_t)(void* /* context */);

/// Memory management configuration
///
/// This struct can be used to control the memory management policy of
/// `libudipe`.
typedef struct udipe_buffer_configurator_s {
    /// Worker thread memory management configuration callback
    ///
    /// If this is left at `NULL`, then the default memory management policy
    /// specified in the documentation of the members of \ref
    /// udipe_buffer_config_t will be used.
    udipe_buffer_config_callback_t callback;

    /// Arbitrary context information associated with `callback`
    ///
    /// You can use this to pass any information you need to `callback` as it
    /// gets called on every worker thread. Beware that this information will be
    /// accessed by all worker threads concurrently, and must therefore be
    /// used in a thread-safe manner.
    ///
    /// Leave this at `NULL` if you do not specify a `callback`.
    void* context;
} udipe_buffer_configurator_t;
