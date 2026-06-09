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
///   storage pages on one side, which contain multiple futures (31 futures per
///   storage page on x86_64 at the time of writing), and the user-requested
///   allocation of individual futures on the other side.
/// - Reduce the number of system calls needed to handle everyday future
///   operations like a sane number of concurrent network operations.
///
/// As these goals are achieved by retaining some resources, which become out of
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
        /// Unallocated epollfds with pre-attached eventfds
        ///
        /// See \ref epoll_event_cache_t for more information.
        epoll_event_cache_t epolls_with_events;
    #endif

    /// Udipe context which this thread-local cache belongs to
    ///
    /// This pointer is needed during the thread exit procedure...
    ///
    /// - To access the global cache for spilling purpose if the host thread's
    ///   TSS destructor wins the race away from the `READY` state (i.e. it
    ///   exits before the context's global cache is finalized). At this point,
    ///   you can safely assume that the context is mostly initialized and e.g.
    ///   logging is still available.
    /// - To call refcounted_tss_release() at the end and finish liberating the
    ///   \ref udipe_context_t if this drops the last reference to it. At this
    ///   point, you must be careful that every member of the context struct
    ///   except for `thread_future_cache` may be finalized and should not be
    ///   used. This means in particular that logging is not an option at that
    ///   time.
    ///
    /// Outside of these circumstances, the thread-local cache is normally
    /// reached via a \ref refcounted_tss_t from the \ref udipe_context_t, which
    /// means that if you have access to this struct, you also have access to
    /// the context, and therefore do not need this pointer.
    udipe_context_t* context;

    /// State machine + flag used to coordinate future spilling and liberation
    ///
    /// A thread's cache stops being useful and its contents must be discarded
    /// when either of the following two events happens:
    ///
    /// - The host thread exits, which means it won't need its contents anymore.
    ///   When this happens, resources from the cache should be spilled into the
    ///   parent context, making them available for reuse by other threads.
    /// - The host context is finalized, which means its contents are not safe
    ///   to use anymore. When this happens, inner resources can be liberated
    ///   directly without going through the global cache.
    ///
    /// Unfortunately, there is no guarantee that these two events won't happen
    /// concurrently (a thread exits at the same time as another thread begins
    /// destroying the \ref udipe_context_t), which means that some
    /// synchronization is needed for thread-safety.
    ///
    /// This atomic word provides the required synchronization by centralizing
    /// two important pieces of informations in a single place:
    ///
    /// 1. A 4-states machine, in which 2 states are mutually exclusive:
    ///     - \ref THREAD_CACHE_READY is the "normal" state which the
    ///       thread-local cache enters upon initialization. It will stay in
    ///       this state for most of its entire lifetime, then leave it when one
    ///       of the aforementioned events will happen.
    ///     - \ref THREAD_CACHE_SPILLING is entered upon thread exit if the
    ///       thread cache is `READY` at that time. It indicates that the TSS
    ///       destructor is in the process of spilling the thread cache's
    ///       contents into the context-global cache. The context destruction
    ///       process must wait for the end of this spilling process by using a
    ///       wait_by_address() loop to wait for a transition to the `DESTROYED`
    ///       state, before it can proceed to destroy the context-global cache.
    ///     - \ref THREAD_CACHE_LIBERATING is entered upon context destruction
    ///       if the thread cache is `READY` at that time. It indicates that the
    ///       context destructor is in the process of liberating the thread
    ///       cache's contents, before it proceeds with destruction of the rest
    ///       of the context. The TSS destructor does not need to wait for the
    ///       end of this process, but it must acknowledge that it is happening
    ///       by refraining from using the `context` pointer for e.g. logging.
    ///     - \ref THREAD_CACHE_DESTROYED is entered when the contents of the
    ///       thread-local cache have been either spilled or liberated, and the
    ///       thread that performed this operation is done and will not access
    ///       this cache again. This state transition must be signaled via
    ///       wake_by_address_all().
    /// 2. The aforementioned state machine features a race to exit the `READY`
    ///    state which one thread will win and the other thread will lose. But
    ///    the host `future_thread_cache_t` struct can only be fully liberated
    ///    once the thread that lost the race is done with it as well. This is
    ///    tracked by setting a separated flag called \ref
    ///    THREAD_CACHE_OTHER_DONE. Once this flag is set and the state machine
    ///    has reached the `DESTROYED` state, it is guaranteed that no other
    ///    thread will access this struct again and it can be liberated.
    ///
    /// The astute reader will notice that this synchronization protocol does
    /// not prevent race conditions between a thread that allocates a new future
    /// and a thread that finalizes the host udipe context. This is by design.
    /// Races between udipe_finalize() and other udipe methods are forbidden by
    /// API contract, as any attempt to allow them would greatly complicate the
    /// udipe implementation and reduce its efficiency only to allow a small
    /// amount of extra usage patterns which are arguably not very useful.
    _Atomic uint32_t futex;
} future_thread_cache_t;

/// Initial state of a \ref future_thread_cache_t
///
/// See \ref future_thread_cache_t::futex for more information.
#define THREAD_CACHE_READY ((uint32_t)0)

/// State of a \ref future_thread_cache_t that is being spilled to the parent
/// \ref future_context_cache_t
///
/// See \ref future_thread_cache_t::futex for more information.
#define THREAD_CACHE_SPILLING ((uint32_t)1)

// TODO: Finish defining all the constants

/// Set up a thread-local futures cache
// TODO docs, implement
// TODO highlight that this returns a pointer due to TSS usage and sharing
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
future_thread_cache_t* future_thread_cache_initialize(udipe_context_t* context);

/// Handle the context finalization half of thread-local cache finalization
///
/// As explained in the documentation of \ref future_thread_cache_t::futex, the
/// exact behavior of this function depends on the relative ordering of
/// udipe_finalize() and thread exit. But at the time this function returns, it
/// is guaranteed that...
///
/// - All contents of the target \ref future_thread_cache_t have either been
///   liberated or spilled into the associated \ref future_context_cache_t, i.e.
///   the cache does not contain any meaningful content anymore.
/// - If the thread has not exited yet, its TSS destructor will not access the
///   \ref udipe_context_t again for any other purpose than calling
///   refcounted_tss_release() on `thread_future_cache` and eventually
///   liberating it once the last reference is dropped.
/// - Once this functions has been called and the associated thread has exited,
///   what remains of the \ref future_thread_cache_t struct will be deallocated.
///
/// \param cache must be a `future_thread_cache_t*` that was previously set up
///              by future_thread_cache_initialize() and was not finalized via
///              future_thread_cache_finalize_from_context() yet. After a call
///              this function, the cache pointer will be set to `NULL` and the
///              cache should not be accessed again by the `udipe_finalize()`
///              implementation.
//
// TODO implement
UDIPE_NON_NULL_ARGS
void future_thread_cache_finalize_from_context(future_thread_cache_t** cache);

/// Handle the thread exit half of thread-local cache finalization
///
/// As explained in the documentation of \ref future_thread_cache_t::futex, the
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
///              cache should not be accessed again by this thread.
//
// TODO implement
UDIPE_NON_NULL_ARGS
void future_thread_cache_finalize_from_thread(future_thread_cache_t** cache);


// TODO: Unit tests
