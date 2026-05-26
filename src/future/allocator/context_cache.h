#pragma once

//! \file
//! \brief Context-global future cache
//!
//! This code module implements the global cache of the future allocator. It is
//! shared between all threads that use a given \ref udipe_context_t, and
//! therefore used as infrequently as possible for performance reasons. Instead
//! \ref future_thread_cache_t is used whenever possible for every
//! performance-critical operation.

#include "pointer_cache.h"
#include "storage_page.h"
#include "thread_cache.h"

#include <stddef.h>
#include <threads.h>


/// Context-global future cache
///
/// This is the bottom caching layer of the future allocator. For performance
/// reasons, threads do not normally reach for this cache, instead they mainly
/// leverage a thread-local \ref future_thread_cache_t.
///
/// This lower cache layer is however occasionally needed to to...
///
/// - Permit inter-thread future sharing with coarse granularity (pages of up to
///   510 future pointers on x86_64 at the time of writing), in situations where
///   failing to do so would be a resource leak: when a user thread exit, when
///   users disregard documentation advice by having one thread that only starts
///   operations and another thread that only awaits them...
/// - Manage future storage pages, which are global resources that can only be
///   liberated once all threads have exited (future pointers that target those
///   pages can be freely shared before that).
/// - Keep track of all existing thread-local caches so that they can be emptied
///   when the host context is destroyed.
typedef struct future_context_cache_s {
    /// Mutex used to synchronize access to the context cache
    ///
    alignas(FALSE_SHARING_GRANULARITY) mtx_t mutex;

    /// Unallocated `udipe_future_t*` pointers
    ///
    /// See \ref future_pointer_cache_t for more information.
    future_pointer_cache_t futures;

    /// Start of the linked list of future storage pages from this context
    ///
    /// Can be extended at will with future_storage_allocate(), but can only be
    /// liberated with future_storage_liberate_all() when the context is
    /// destroyed. Until then, future reuse happen at the granularity of
    /// individual pointers to this storage, via the \ref future_pointer_cache_t
    /// of this global context and individual thread-local contexts.
    future_storage_page_t* first_storage_page;

    /// Growable array of thread-local cache managed by this context cache
    ///
    /// Used to track thread-local caches for the purpose of liberating the
    /// resources of threads that haven't exited yet when the context is
    /// destroyed (and liberate the empty thread-local caches of other threads
    /// which exited before).
    ///
    /// `mutex` must not be held at the time where liberation requests are made,
    /// because this could result in deadlock if a thread is concurrently in the
    /// process of exiting and acquires `mutex` to spill futures in the process
    /// of doing so.
    ///
    /// See also `thread_caches_length` and `thread_caches_capacity`.
    future_thread_cache_t** thread_caches;

    /// Current length of the `thread_caches` array
    ///
    /// The array can grow until `thread_caches_capacity` is reached, then
    /// reallocation must occur.
    size_t thread_caches_length;

    /// Capacity of the `thread_caches` allocation
    ///
    /// Once `thread_caches_length` reaches this value, the backing store must
    /// be enlarged using realloc().
    size_t thread_caches_capacity;
} future_context_cache_t;

// TODO: Add global context constructor + destructor


// TODO: Unit tests
