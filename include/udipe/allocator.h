#pragma once

//! \file
//! \brief Memory allocator
//!
//! This header is the home of \ref udipe_allocator_config_t, the data structure
//! that configures the memory management policy of `libudipe`, along with
//! related type and constant definitions.

#include <stddef.h>


/// \internal
///
/// \brief Maximum number of buffer availability tracking words in \ref
/// allocator_s
///
/// This indirectly dictates the maximum amount of buffers that \ref allocator_s
/// can manage, see also UDIPE_MAX_BUFFER_COUNT.
///
/// This can be tuned up whenever a real-world use case emerges where a larger
/// value would be useful. But overall, the current algorithm only performs well
/// for small values of this parameter. If it ever needs to get large, the
/// allocator algorithm most likely also needs to change.
#define UDIPE_MAX_USAGE_WORDS 1


/// Maximum number of buffers that a worker thread can manage
///
/// Any attempt to set up a worker thread that manages more than this amount
/// of buffers will fail.
///
/// If automatic configuration logic determines that the optimal amount of
/// buffers is above this limit, then it will log a warning and proceed with
/// UDIPE_MAX_BUFFER_COUNT buffers instead.
#define UDIPE_MAX_BUFFER_COUNT (UDIPE_MAX_USAGE_WORDS * sizeof(size_t) * 8)


/// Tunable memory management parameters for one worker thread
///
/// This is the value returned by the \ref udipe_allocator_callback_t for each
/// worker thread, which is used to tune said thread's memory management policy.
typedef struct udipe_thread_allocator_config_s {
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
    /// to complete. It cannot be larger than MAX_BUFFER_COUNT.
    ///
    /// A value of 0 requests the default buffer count, which is adjusted such
    /// that there is at least one buffer and the buffers collectively fit...
    ///
    /// - Within the L2 cache of any CPU on which the worker thread may execute,
    ///   if said L2 cache is private (as on x86 CPUs).
    /// - Within an even share of the L2 cache if it is shared across multiple
    ///   CPU cores (as on most Arm CPUs).
    size_t buffer_count;
} udipe_thread_allocator_config_t;


/// Worker thread memory management configuration callback
///
/// You may specify such a callback as part of \ref udipe_config_t in order to
/// tune the memory management policy of individual `libudipe` worker threads.
///
/// It will be invoked by each worker thread on startup (and must therefore be
/// thread-safe since worker threads start concurrently), and it is responsible
/// for returning a \ref udipe_thread_allocator_config_t that adjusts the worker
/// thread's memory management policy. See the documentation of this struct for
/// more info on available tunable parameters.
///
/// The input `void*` parameter is of your choosing. It is specified via \ref
/// udipe_allocator_config_s::context and passed in as is to each invocation of
/// the callback. You can use it to pass any information that your callback
/// needs to compute its memory management configuration. For example this can
/// be used to pass in an `hwloc_topology_t` or equivalent in cache locality
/// aware designs.
///
/// The intent behind this callback-based design is to let you...
///
/// - Adapt to the fact that the number of worker threads that `libudipe` will
///   spawn, and their pinning to CPU cores or lack thereof, is an opaque
///   implementation detail of `libudipe`.
/// - Adjust the tuning parameters on a per-thread basis, which can make sense
///   on systems with heterogeneous CPU cores.
typedef udipe_thread_allocator_config_t (*udipe_allocator_callback_t)(void* /* context */);


/// Memory management configuration
///
/// This struct can be used to control the memory management policy of
/// `libudipe`.
typedef struct udipe_allocator_config_s {
    /// Worker thread memory management configuration callback
    ///
    /// If this is left at `NULL`, then the default memory management policy
    /// specified in the documentation of the members of \ref
    /// udipe_thread_allocator_config_t will be used.
    ///
    /// See \ref udipe_thread_allocator_config_t for more info.
    udipe_allocator_callback_t callback;

    /// Arbitrary context information associated with `callback`
    ///
    /// You can use this to pass any information you need to `callback` as it
    /// gets called on every worker thread. Beware that this information will be
    /// accessed by all worker threads concurrently, and must therefore be
    /// accessed in a thread-safe manner.
    ///
    /// Leave this at `NULL` if you do not specify a `callback`.
    void* context;
} udipe_allocator_config_t;
