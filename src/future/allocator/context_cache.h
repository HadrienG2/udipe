#pragma once

//! \file
//! \brief Context-global future cache
//!
//! This code module implements the global cache of the future allocator. It is
//! shared between all threads that use a given \ref udipe_context_t, and
//! therefore used sparingly in performance-critical code paths. Those favor
//! using \ref future_thread_cache_t instead.

#include <udipe/context.h>
#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

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
    /// Generally speaking, all other fields can only be accessed when this
    /// mutex is locked, with one important exception at udipe_finalize() time
    /// which is explained in the documentation of the `thread_caches` field.
    alignas(FALSE_SHARING_GRANULARITY) mtx_t mutex;

    /// Unallocated `udipe_future_t*` pointers
    ///
    /// This field can only be accessed when `mutex` is locked.
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
    ///
    /// This field can only be accessed when `mutex` is locked.
    future_storage_page_t* first_storage_page;

    /// Growable array of thread-local cache managed by this context cache
    ///
    /// Used to track thread-local caches for the purpose of liberating the
    /// resources of threads that haven't exited yet when the context is
    /// destroyed (and liberate the empty thread-local caches of other threads
    /// which exited before).
    ///
    /// This field can only be accessed from a function that has a public
    /// `udipe_` function in its call stack, i.e. it must not be accessed
    /// asynchronously by internal machinery like the implementation of
    /// thread-local caches.
    ///
    /// Access _usually_ requires `mutex` to be locked like other fields, with
    /// one important exception. `mutex` must **not** be locked at the time
    /// where thread-local caches are being finalized by udipe_finalize(),
    /// because this could result in the following deadlock:
    ///
    /// - One user thread starts calling udipe_finalize(), which eventually
    ///   results in `mutex` being locked and thread cache liberation starting.
    /// - Another user thread starts exiting, resulting in a call to the
    ///   future_thread_cache_finalize_from_thread() TSS destructor.
    /// - Under \ref future_thread_cache_t::futex protection, said TSS
    ///   destructor starts spilling the thread-local cache's contents to the
    ///   global context cache. To this end, it must acquire `mutex`, which is
    ///   currently locked, so it blocks waiting for `mutex` to free up
    /// - Meanwhile, context cache liberation reaches the point where it tries
    ///   to finalize this thread-local cache. So it tries to wih the \ref
    ///   future_thread_cache_t::futex state machine race too, but fails as it
    ///   got there last. As a result, it must wait for the thread to finish
    ///   spilling the contents of the thread-local cache to this global cache.
    /// - At this point, udipe_finalize() is blocked waiting for the TSS
    ///   destructor to finish spilling its contents to the global context cache
    ///   and the TSS destructor is blocked waiting for udipe_finalize() to
    ///   finish and unlock the mutex so that it can perform said spilling,
    ///   so we have a deadlock on our hands.
    ///
    /// As mentioned above, we resolve this deadlock hazard by not locking
    /// `mutex` before finalizing thread caches in udipe_finalize(). This works
    /// out because `mutex` locking is not necessary for synchronization there:
    ///
    /// - The API contract of udipe_finalize() prevents the user from calling it
    ///   concurrently with any other `udipe_` function.
    /// - The synchronization contract of this field prevents it from being
    ///   accessed outside of a user-facing `udipe_` function call.
    /// - The `futex` of each thread-local cache ensures that either
    ///   udipe_finalize() comes first and prevents the associated TSS
    ///   destructor from spilling to the global cache, or the TSS destructor
    ///   comes first and is properly awaited by udipe_finalize() before the
    ///   target global context cache is destroyed.
    ///
    /// See also `thread_caches_length` and `thread_caches_capacity`.
    future_thread_cache_t** thread_caches;

    /// Current length of the `thread_caches` array
    ///
    /// The `thread_caches` array can grow until `thread_caches_capacity` is
    /// reached, then reallocation must occur.
    ///
    /// Can only be accessed from a function that has a public `udipe_` function
    /// in its call stack, and requires `mutex` to be locked except in the
    /// implementation of udipe_finalize(). See `thread_caches` for more info.
    size_t thread_caches_length;

    /// Capacity of the `thread_caches` allocation
    ///
    /// Once `thread_caches_length` reaches this value, the backing store
    /// targeted by `thread_caches` must be enlarged using realloc().
    ///
    /// Can only be accessed from a function that has a public `udipe_` function
    /// in its call stack, and requires `mutex` to be locked except in the
    /// implementation of udipe_finalize(). See `thread_caches` for more info.
    size_t thread_caches_capacity;
} future_context_cache_t;

// TODO docs, implement
UDIPE_NODISCARD
future_context_cache_t future_context_cache_initialize();

// TODO docs, implement, document preconditions
void future_context_cache_register_thread(future_context_cache_t* context_cache,
                                          future_thread_cache_t* thread_cache);

// TODO: docs, implement
void future_context_cache_finalize_threads(future_context_cache_t* cache);

// TODO docs, implement
// TODO: Should probably use a utility function that unregisters all threads,
//       which must be called at a point where the mutex is unlocked.
UDIPE_NON_NULL_ARGS
void future_context_cache_finalize(future_context_cache_t* cache);


// TODO: Unit tests
