#pragma once

//! \file
//! \brief Context-global future resource cache
//!
//! This code module implements the global resource cache of the future
//! allocator, shared by all threads that use a given \ref udipe_context_t.
//!
//! Its use requires non-scalable and NUMA-hostile synchronization, which is why
//! 1/an attempt is always made to get resources from the thread-local cache
//! first before hitting this slower cache; and 2/when the global cache does
//! need to be hit, resources are transferred from it to the thread-local cache
//! in bulk.

#include "pointer_cache.h"
#include "storage_page.h"
#include "thread_cache.h"

#include <stddef.h>
#include <threads.h>


/// Context-global future resource cache
//
// TODO docs, consider stealing some from the code module docs
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
    /// Used to liberate resources of thread-local caches that haven't exited
    /// yet when the context is destroyed.
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

// TODO: Add constructors for both the global context and thread-local contexts
//       bound to this global context + destructor for the global context.
