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
    ///   to liberate the contents of this thread-local cache. So it tries to do
    ///   so by winning the \ref future_thread_cache_t::futex state machine
    ///   race, but fails to do so as it got there last. As a result, it must
    ///   wait for the thread to finish spilling the contents of the
    ///   thread-local cache to this global cache.
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

/// Set up a context-global future cache
///
/// This is done as part of udipe_initialize(), initializing the bottom caching
/// layer of the newly created context's future allocator.
///
/// The resulting \ref future_context_cache_t object must eventually be
/// finalized by future_context_cache_finalize() as part of the context
/// finalization process.
///
/// This function must be called within the scope of with_logger().
///
/// \returns a context-global future cache that must eventually be destroyed
///          using future_context_cache_finalize().
// TODO implement
UDIPE_NODISCARD
future_context_cache_t future_context_cache_initialize();

/// Register a thread-local future cache into a context-global future cache
///
/// This function is part of the implementation of
/// future_thread_cache_initialize() and should never be called directly.
///
/// \internal
///
/// This function is called when a new thread-local future cache is created, in
/// order to ensure that when the context-global future cache is eventually
/// liberated, it will take care to empty and invalidate all associated
/// thread-local caches.
///
/// This function must be called within the scope of with_logger(), and it must
/// have a public `udipe_` function in its call stack (i.e. it must not be
/// called asynchronously by a background thread).
///
/// \param context_cache must be a context cache that was set up with
///                      future_context_cache_initialize() and wasn't destroyed
///                      with future_context_cache_finalize() yet.
/// \param thread_cache must be a thread cache that was mostly set up by
///                     future_thread_cache_initialize() and wasn't destroyed
///                     with future_thread_cache_finalize_from_context() or
///                     passed to this function yet.
// TODO docs
UDIPE_NON_NULL_ARGS
void future_context_cache_register_thread(future_context_cache_t* context_cache,
                                          future_thread_cache_t* thread_cache);

/// Finalize all thread-local future caches before destroying the context cache
///
/// This function is part of the implementation of
/// future_context_cache_finalize() and should never be called directly.
///
/// \internal
///
/// This function is called when a context cache is destroyed. It ensures that
/// all thread-local caches get emptied and invalidated before the global cache
/// that they spill into is destroyed, and also schedules them for eventual
/// liberation if/when the associated thread has exited.
///
/// Importantly, this function must be called at a time where the context
/// cache's mutex is **not** being locked, otherwise deadlock may ensue.
///
/// This function must be called within the scope of with_logger().
///
/// \param cache must be a context cache that was set up with
///              future_context_cache_initialize() and wasn't destroyed with
///              future_context_cache_finalize() yet.
// TODO: implement
UDIPE_NON_NULL_ARGS
void future_context_cache_finalize_threads(future_context_cache_t* cache);

/// Destroy a context-global future cache
///
/// This is done as part of udipe_finalize(), finalizing the bottom caching
/// layer of the future allocator along with all attached thread-local caches.
/// Futures cannot be allocated again after calling this function.
///
/// This function must be called within the scope of with_logger().
///
/// \param cache must be a context cache that was set up with
///              future_context_cache_initialize() and wasn't destroyed with
///              future_context_cache_finalize() yet.
// TODO implement based on above function.
UDIPE_NON_NULL_ARGS
void future_context_cache_finalize(future_context_cache_t* cache);


// TODO: Unit tests
