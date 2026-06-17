#pragma once

//! \file
//! \brief Thread-local futures cache
//!
//! This code module implements the thread-local resource cache of the future
//! allocator. It is the main cache used for frequent future allocation and
//! liberation operations, only falling back to the global \ref
//! future_context_cache_t in specific circumstances (see its documentation).

#include <udipe/context.h>
#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "pointer_cache.h"
#include "sync_caches.h"

#include <stdatomic.h>
#include <stdint.h>
#include <threads.h>


/// Thread-local future resource cache
///
/// This is the cache which a thread mainly interacts with during the future
/// allocation and liberation process. Its main functions are to...
///
/// - Resolve the API impedance mismatch between the allocation of future
///   storage pages on one side, which brings in multiple futures (31 futures
///   per storage page on x86_64 at the time of writing), and the user-requested
///   allocation of individual futures on the other side.
/// - Reduce the number of system calls needed to handle everyday future
///   operations like a sane number of concurrent network operations.
///
/// These goals are achieved by retaining some resources, which become out of
/// reach from other threads, a secondary function of this cache is to permit
/// the spilling of non-liberable resources like futures to the global \ref
/// future_context_cache_t when e.g. the current thread exits. Otherwise, these
/// resources would be lost, resulting in a resource leak.
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
        /// Unallocated inpolls with pre-attached eventfds
        ///
        /// See \ref inpoll_event_cache_t for more information.
        inpoll_event_cache_t inpolls_with_events;
    #endif

    /// Udipe context which this thread-local cache belongs to
    ///
    /// This pointer is needed during the thread exit procedure...
    ///
    /// - To access the global cache for spilling purpose if the host thread
    ///   starts exiting before the context starts being destroyed. At this
    ///   point, you can safely assume that the context is mostly initialized
    ///   and e.g. logging is still available, because if the context destructor
    ///   starts concurrently it will wait for the spilling to complete.
    /// - To call refcounted_tss_release() at the end and finish liberating the
    ///   \ref udipe_context_t if this drops the last reference to it. At this
    ///   point, you must be careful that every member of the context struct
    ///   except for `future_local_cache_key` may be finalized and should not be
    ///   used. This means in particular that no logging is possible.
    ///
    /// Outside of these circumstances, the thread-local cache is normally
    /// reached via a \ref refcounted_tss_t from the \ref udipe_context_t, which
    /// means that if you have access to this struct, you also have access to
    /// the context, and therefore do not need this pointer.
    udipe_context_t* context;

    /// Status flags used to coordinate future spilling and liberation
    ///
    /// A thread cache stops being useful and its contents must be diposed of
    /// when either of the following happens:
    ///
    /// - The host thread exits, which means it won't need its thread-local
    ///   cache's contents anymore. When this happens, resources from the cache
    ///   should be spilled into the parent context's global cache, making them
    ///   available for reuse by other threads.
    /// - The host context is finalized, which means its contents are not safe
    ///   to use anymore. When this happens, inner resources can be liberated
    ///   directly without going through the global cache.
    ///
    /// Unfortunately, there is no guarantee that these two events won't happen
    /// concurrently (a thread exits at the same time as another thread begins
    /// destroying the \ref udipe_context_t), which means that some
    /// synchronization is needed for thread-safety.
    ///
    /// This atomic word provides the required synchronization as it tracks the
    /// logical OR of all required boolean truths:
    ///
    /// - \ref THREAD_CACHE_THREAD_DYING is set at the beginning of the TSS
    ///   destructor. If it is set before `CONTEXT_DYING`, the cache's contents
    ///   will be spilled to the context-global cache by the TSS destructor. If
    ///   udipe_finalize() starts concurrently as this is happening, it must
    ///   wait for this process to complete. To do so, it will need to first
    ///   wait for the `EMPTIED` flag to be set, then busy-wait for the
    ///   `THREAD_DONE` flag to be set as well (see below for the explanation).
    ///   Once all of this is done, udipe_finalize() can proceed with liberating
    ///   this thread-local cache and the context-global cache.
    /// - \ref THREAD_CACHE_CONTEXT_DYING is set at the beginning of
    ///   udipe_finalize(). If it is set before `THREAD_DYING`, the cache's
    ///   contents will be destroyed directly. The TSS destructor does not need
    ///   to wait for that to happen, but it must acknowledge that the context
    ///   is being destroyed by refraining from accessing the `context` pointer
    ///   in any other way than calling refcounted_tss_release() on the \ref
    ///   udipe_context_t::future_local_cache_key + liberating the context
    ///   struct if this releases the last remaining reference to it. In
    ///   particular, this means that no logging can be performed in this case.
    /// - \ref THREAD_CACHE_EMPTIED is set when all contents from this
    ///   thread-local cache have been removed through either spilling or
    ///   liberating. When this flag is set by the TSS destructor, if
    ///   `CONTEXT_DYING` is set indicating that udipe_finalize() is waiting
    ///   for the TSS destructor, an OS notification is sent via
    ///   wake_by_address_all(), which can be awaited on the udipe_finalize()
    ///   side via a wait_on_address() loop that waits for this flag to be set.
    ///   If udipe_finalize() did need to perform this waiting, then it will
    ///   also need to busy-wait for `THREAD_DONE` to be set for reasons
    ///   explained below.
    /// - \ref THREAD_CACHE_THREAD_DONE is set at the end of the TSS destructor,
    ///   once it is done accessing this struct. If the implementation of
    ///   udipe_finalize() lost the race and needed to await the `EMPTIED` flag,
    ///   it must also busy-wait for this flag to be set because there is no
    ///   other way to wait for the call to wake_by_address_all() to finish and
    ///   the context must not be destroyed until then as this would destroy the
    ///   logger that wake_by_address_all() is using. The thread-local cache
    ///   struct can be liberated once this flag and `CONTEXT_DONE` are set.
    /// - \ref THREAD_CACHE_CONTEXT_DONE is set once the context destructor is
    ///   done liberating the contents of this cache and will not access it
    ///   again. The threal-local cache struct can be liberated once both this
    ///   flag and `THREAD_DONE` are set.
    ///
    /// The astute reader will notice that this synchronization protocol does
    /// not prevent race conditions between a thread that allocates a new future
    /// and a thread that finalizes the host udipe context. This is by design.
    /// Races between udipe_finalize() and other udipe methods are forbidden by
    /// API contract, as any attempt to allow them would greatly complicate the
    /// udipe implementation and reduce its efficiency only to allow a small
    /// amount of extra usage patterns which are arguably not very useful.
    _Atomic uint32_t flags;
} future_thread_cache_t;

/// \ref future_thread_cache_t status flag that is set when the host thread is
/// exiting, which means that cache contents can be spilled
///
/// See \ref future_thread_cache_t::flags for more information.
#define THREAD_CACHE_THREAD_DYING ((uint32_t)1)

/// \ref future_thread_cache_t status flag that is set when the context is
/// being finalized, which means that cache contents can be liberated
///
/// See \ref future_thread_cache_t::flags for more information.
#define THREAD_CACHE_CONTEXT_DYING ((uint32_t)2)

/// \ref future_thread_cache_t status flag that is set when its contents have
/// been removed through either spilling or liberation
///
/// See \ref future_thread_cache_t::flags for more information.
#define THREAD_CACHE_EMPTIED ((uint32_t)4)

/// \ref future_thread_cache_t status flag that is set when the TSS destructor
/// is done with it and will not access it anymore
///
/// See \ref future_thread_cache_t::flags for more information.
#define THREAD_CACHE_THREAD_DONE ((uint32_t)8)

/// \ref future_thread_cache_t status flag that is set when the udipe_finalize()
/// is done with it and will not access it anymore
///
/// See \ref future_thread_cache_t::flags for more information.
#define THREAD_CACHE_CONTEXT_DONE ((uint32_t)16)

/// Set up a thread-local futures cache
///
/// This function returns a pointer to a heap-allocated data structure because
/// 1/TSS variables can only hold pointers and 2/ownership of this data
/// structure is shared between the host \ref udipe_context_t and the TSS
/// destructor, which means that liberation requires care.
///
/// The resulting \ref future_thread_cache_t will only be liberated when both of
/// its owners have released control over it:
///
/// - As the host \ref udipe_context_t is destroyed by udipe_finalize(), it must
///   call future_thread_cache_finalize_from_context().
/// - As the host thread exits and its TSS destructor is called, it must call
///   future_thread_cache_finalize_from_thread().
///
/// Once both of the following have happened, the \ref future_thread_cache_t
/// will be liberated, which will possibly also result in the liberation of the
/// host \ref udipe_context_t if it held the last reference to it.
///
/// This function must be called within the scope of with_logger().
///
/// \param context must point to a context that was previously created by
///                udipe_initialize() and wasn't destroyed by udipe_finalize()
///                yet.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
future_thread_cache_t* future_thread_cache_initialize(udipe_context_t* context);

/// Handle the thread exit half of thread-local cache finalization
///
/// As explained in the documentation of \ref future_thread_cache_t::flags, the
/// exact behavior of this function depends on the relative ordering of
/// udipe_finalize() and thread exit. But at the time this function returns, it
/// is guaranteed that...
///
/// - All contents of the target \ref future_thread_cache_t have either been
///   spilled into the associated \ref future_context_cache_t or are in the
///   process of being liberated.
/// - Once this functions has been called and the underlying udipe context has
///   been finalized, what remains of the \ref future_thread_cache_t struct will
///   be deallocated.
//
/// \param cache must be a `future_thread_cache_t*` that was previously set up
///              by future_thread_cache_initialize() and was not finalized via
///              future_thread_cache_finalize_from_thread() yet. After a call
///              this function, the cache pointer will be set to `NULL` and the
///              cache should not be accessed again by its home thread.
UDIPE_NON_NULL_ARGS
void future_thread_cache_finalize_from_thread(future_thread_cache_t** cache);

/// Handle the context finalization half of thread-local cache finalization
///
/// As explained in the documentation of \ref future_thread_cache_t::flags, the
/// exact behavior of this function depends on the relative ordering of
/// udipe_finalize() and thread exit. But at the time this function returns, it
/// is guaranteed that...
///
/// - All contents of the target \ref future_thread_cache_t have either been
///   liberated or spilled into the associated \ref future_context_cache_t, i.e.
///   the cache does not contain any meaningful content anymore.
/// - If the thread has not exited yet, its TSS destructor will not access the
///   \ref udipe_context_t again for any other purpose than calling
///   refcounted_tss_release() on `future_local_cache_key` and eventually
///   liberating the context once the last reference to it is gone.
/// - Once this functions has been called and the associated thread has exited,
///   what remains of the \ref future_thread_cache_t struct will be deallocated.
///
/// This function must be called within the scope of with_logger().
///
/// \param cache must be a `future_thread_cache_t*` that was previously set up
///              by future_thread_cache_initialize() and was not finalized via
///              future_thread_cache_finalize_from_context() yet. After a call
///              this function, the cache pointer will be set to `NULL` and the
///              cache should not be accessed again by the udipe_finalize()
///              implementation.
UDIPE_NON_NULL_ARGS
void future_thread_cache_finalize_from_context(future_thread_cache_t** cache);


// TODO: Unit tests
