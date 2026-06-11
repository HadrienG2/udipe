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
#include "refcounted_tss.h"

#include <hwloc.h>


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
    ///
    /// Sadly, due to dubious `tss_t` API design choices inherited from POSIX's
    /// `pthread_key_` API, liberation of this struct needs to be deferred until
    /// all threads that ever used this context have exited.
    ///
    /// This results in a near memory leak of the size of the entire
    /// `udipe_context_t`, which is not so nice. But the alternative of
    /// extracting this struct into a separate memory allocation is hard to get
    /// right and will cause extra CPU overhead due to indirection.
    ///
    /// Bearing this in mind, we tolerate the memory overhead for now, only
    /// reducing it by keeping the `udipe_context_t` in a partially finalized
    /// state where only this field is still initialized until all threads have
    /// exited and we can finally liberate the underlying allocation.
    ///
    /// Overall, the presence of this annoying field affects the \ref
    /// udipe_context_t destruction process in the following manner:
    ///
    /// - Thread-local caches must be destroyed first by iterating through the
    ///   relevant array of the `global_future_cache` without locking the
    ///   associated mutex first.
    /// - udipe_finalize() must finish with a call to refcounted_tss_discard()
    ///   on this member, and is only allowed to liberate the heap allocation
    ///   holding the `udipe_context_t` if this call returns `true`. Otherwise
    ///   it must leave liberation up to the TSS destructors of other threads as
    ///   they call refcounted_tss_release() on their side.
    refcounted_tss_t future_local_cache_key;

    /// hwloc topology
    ///
    /// Used to query the CPU topology (cache sizes, NUMA etc) and pin threads
    /// to CPU cores.
    hwloc_topology_t topology;

    /// Context-global future resource cache
    ///
    /// The future allocator will query this cache if `local_future_cache` does
    /// not have some resources that it needs.
    future_context_cache_t future_global_cache;

    /// Allocator of \ref shared_connect_options_t
    ///
    /// See \ref connect.h for more info on how this works and how to use it.
    connect_options_allocator_t connect_options;
};
