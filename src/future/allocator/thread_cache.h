#pragma once

//! \file
//! \brief Thread-local resource cache
//!
//! This code module implements the thread-local resource cache of the future
//! allocator. Operations targeting this cache are maximally unsynchronized and
//! NUMA-local, which should result in optimal performance and scalability,
//! therefore the allocator tries to only hit this cache during normal
//! operation.

#include <udipe/context.h>

#include "pointer_cache.h"
#include "sync_caches.h"

#include <threads.h>


/// Thread-local future resource cache
//
// TODO docs, consider stealing some from the module docs
typedef struct future_thread_cache_s {
    /// Unallocated `udipe_future_t*` pointers
    ///
    /// See \ref future_pointer_cache_t for more information.
    future_pointer_cache_t futures;

    /// Unallocated unsignaled event objects
    ///
    /// See \ref event_cache_t for more information.
    event_cache_t events;

    #ifdef __linux__
        /// Unallocated epollfds with pre-attached eventfds
        ///
        /// See \ref epoll_event_cache_t for more information.
        epoll_event_cache_t epolls_with_events;
    #endif

    /// Once flag used to coordinate spilling and liberation
    ///
    /// A thread's cache stops being useful and its contents must be discarded
    /// when either of the following two events happens:
    ///
    /// - The thread exits, which means it won't need its contents anymore. When
    ///   this happens, resources from this cache should be spilled into the
    ///   host context, making them available for reuse by other threads.
    /// - The host context is destroyed, which means its contents are not safe
    ///   to use anymore. When this happens, inner resources should be liberated
    ///   directly without going through the global cache.
    ///
    /// Unfortunately, there is no guarantee that these two events won't happen
    /// simultaneously in two different threads, which means that some
    /// synchronization is needed for thread-safety.
    ///
    /// This `once_flag` thus provides the required synchronization to ensure
    /// that only one of these processes will happen, and attempts to start the
    /// other process after the first process has begun will make it wait for
    /// the first process to finish.
    ///
    /// The astute reader will notice that this synchronization does _not_
    /// prevent race conditions between a thread that allocates a new future and
    /// a thread that destroys the host udipe context. This is by design. Races
    /// between udipe_finalize() and other udipe methods are forbidden by API
    /// contract, as any attempt to allow them would greatly complicate the
    /// udipe implementation and reduce its efficiency only to allow a small
    /// amount of extra usage patterns which are arguably not very useful.
    once_flag spill_or_liberate;

    /// Udipe context which this thread-local cache belongs to
    ///
    /// This pointer can only safely be used within a code path guarded by
    /// `spill_or_liberate`, and is set to `NULL` inside all `spill_or_liberate`
    /// code paths to help detect misuse. But the associated write isn't atomic
    /// and therefore cannot be relied on for synchronization.
    ///
    /// The context has a `tss_t` pointing back to us, which is how threads that
    /// allocate or liberate futures end up locating this thread-local cache.
    ///
    /// The back-reference to the parent context provided by this pointer is
    /// only needed/used when spilling the contents of this thread-local cache
    /// into the global context cache, which happens when a thread exits before
    /// the host context is destroyed. In all other cases, this thread-local
    /// cache is accessed via the context's `tss_t`, which means to access this
    /// cache the caller must have access to the parent context to begin with.
    udipe_context_t* context;

    /// Bitfield used to track references to this struct
    ///
    /// As explained in the `spill_or_liberate` docs, pointers to this struct
    /// are held by the udipe context (to permit resource liberation on context
    /// destruction) and the OS thread-local storage implementation (to permit
    /// resource spilling on thread exit).
    ///
    /// Therefore this struct can only be liberated once both context
    /// destruction AND thread exit have happened, and this atomic bitfield is
    /// used to keep track of these two liberation conditions:
    ///
    /// - \ref CONTEXT_DESTROYED_FLAG tracks whether the context was destroyed.
    /// - \ref THREAD_EXITED_FLAG tracks whether the thread has exited.
    ///
    /// Flags must be set with `atomic_fetch_or_explicit()` in
    /// `memory_order_release` mode. Once the last flag is set, the cache can be
    /// liberated after an `atomic_thread_fence()` with `memory_order_acquire`.
    _Atomic size_t reference_flags;
} future_thread_cache_t;

/// Truth that the \ref udipe_context_t from which a \ref future_thread_cache_t
/// originates has been destroyed, and thus won't try to liberate it.
///
/// See \ref future_thread_cache_t::reference_flags.
#define CONTEXT_DESTROYED_FLAG ((size_t)1)

/// Truth that the thread which a \ref future_thread_cache_t belongs to has
/// exited, and thus the TSS destructor cannot be called anymore.
///
/// See \ref future_thread_cache_t::reference_flags.
#define THREAD_EXITED_FLAG ((size_t)2)

// TODO: Add TSS destructor, point to global cache for constructor
