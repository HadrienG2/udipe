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

    /// State machine + flag used to coordinate future spilling and liberation
    ///
    /// A thread's cache stops being useful and its contents must be discarded
    /// when either of the following two events happens:
    ///
    /// - The thread exits, which means it won't need its contents anymore. When
    ///   this happens, resources from this cache should be spilled into the
    ///   host context, making them available for reuse by other threads.
    /// - The host context is finalized, which means its contents are not safe
    ///   to use anymore. When this happens, inner resources should be liberated
    ///   directly without going through the global cache.
    ///
    /// Unfortunately, there is no guarantee that these two events won't happen
    /// concurrently (a thread exits as another begins destroying the \ref
    /// udipe_context_t), which means that some synchronization is needed for
    /// thread-safety.
    ///
    /// This atomic word provides the required synchronization here, by
    /// combining two pieces of informations:
    ///
    /// 1. A state machine with four distinct states:
    ///     - \ref THREAD_CACHE_READY is the "normal" initialized state where
    ///       the thread-local cache is ready to process allocation requests.
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
    ///       end of this process, but it must acknowledge what's happening by
    ///       refraining from using the `context` pointer.
    ///     - \ref THREAD_CACHE_DESTROYED is entered when the contents of the
    ///       thread-local cache have been either spilled or liberated, and the
    ///       thread that performed this operation is done and will not access
    ///       this cache again. This state transition must be signaled via
    ///       wake_by_address_all().
    /// 2. A \ref THREAD_CACHE_OTHER_DONE flag which indicates whether the
    ///    process that did not win the state machine race is done using this
    ///    thread-local cache as well TODO finish explaining liberation
    //
    // TODO: Finish rewrite, erase old docs below, define all the new constants.
    //
    /// Once flag used to coordinate spilling and liberation
    ///
    /// A thread's cache stops being useful and its contents must be discarded
    /// when either of the following two events happens:
    ///
    /// - The thread exits, which means it won't need its contents anymore. When
    ///   this happens, resources from this cache should be spilled into the
    ///   host context, making them available for reuse by other threads.
    /// - The host context is finalized, which means its contents are not safe
    ///   to use anymore. When this happens, inner resources should be liberated
    ///   directly without going through the global cache.
    ///
    /// Unfortunately, there is no guarantee that these two events won't happen
    /// concurrently in two different threads, which means that some
    /// synchronization is needed for thread-safety.
    ///
    /// This `once_flag` thus provides the required synchronization to ensure
    /// that only one of these processes will happen, and attempts to start the
    /// other process after the first process has begun will make it wait for
    /// the first process to finish.
    ///
    /// The astute reader will notice that this synchronization does _not_
    /// prevent race conditions between a thread that allocates a new future and
    /// a thread that finalizes the host udipe context. This is by design. Races
    /// between udipe_finalize() and other udipe methods are forbidden by API
    /// contract, as any attempt to allow them would greatly complicate the
    /// udipe implementation and reduce its efficiency only to allow a small
    /// amount of extra usage patterns which are arguably not very useful.
    //
    // FIXME: Replace once_flag with a custom futex-based abstraction, which
    //        merges information from reference_flags. The more time passes, the
    //        worse fit the C11 once_flag seems for the way we use it here:
    //
    //        - The very idea of using a once_flag with two different callbacks
    //          is likely to be surprising to someone who's rather used to its
    //          more common lazy initialization applications.
    //        - Its callback cannot take a void* context parameter, forcing the
    //          use of clunky and inefficient thread_local hacks
    //        - It makes thread exit wait for context finalization, when
    //          actually we only need to make context finalization wait for
    //          thread exit.
    //
    //        After replacing this, grep once_flag|call_once|spill_or_liberate
    //        in this header + context_cache.h and fix it up.
    once_flag spill_or_liberate;

    /// Udipe context which this thread-local cache belongs to
    ///
    /// This pointer is needed during the thread exit procedure...
    ///
    /// - To access the global cache for spilling purpose if this thread wins
    ///   the `spill_or_liberate` race (i.e. it exits before the context's
    ///   global cache is finalized). At this point, you can safely assume that
    ///   the context is mostly initialized and e.g. logging is still available.
    /// - To call refcounted_tss_release() at the end and finish liberating the
    ///   \ref udipe_context_t if this drops the last reference to it. At this
    ///   point, you must be careful that every member of the context except for
    ///   `thread_future_cache` may be finalized and should not be used. This
    ///   means in particular that logging is not an option at that time.
    ///
    /// Outside of these circumstances, the thread-local cache is normally
    /// reached via a \ref refcounted_tss_t from the \ref udipe_context_t, which
    /// means that if you have access to this struct, you also have access to
    /// the context, and therefore do not need this pointer.
    udipe_context_t* context;

    /// Bitfield used to track references to this struct
    ///
    /// As explained in the `spill_or_liberate` docs, pointers to this struct
    /// are held by the udipe context (to permit resource liberation on context
    /// finalization) and the OS thread-local storage implementation (to permit
    /// resource spilling on thread exit).
    ///
    /// Therefore this struct can only be liberated once both context
    /// finalization AND thread exit have happened, and this atomic bitfield is
    /// used to keep track of these two liberation conditions:
    ///
    /// - \ref CONTEXT_FINALIZED_FLAG tracks whether the context was finalized.
    /// - \ref THREAD_EXITED_FLAG tracks whether the thread has exited.
    ///
    /// Flags must be set with `atomic_fetch_or_explicit()` in
    /// `memory_order_release` mode as the very last operation of a thread on a
    /// given `future_thread_cache_t`. Once the last flag is set, the cache can
    /// be liberated after an `atomic_thread_fence()` with
    /// `memory_order_acquire`.
    atomic_size_t reference_flags;
} future_thread_cache_t;

/// Truth that the \ref udipe_context_t from which a \ref future_thread_cache_t
/// originates has been finalized, and thus won't try to liberate it.
///
/// See \ref future_thread_cache_t::reference_flags.
#define CONTEXT_FINALIZED_FLAG ((size_t)1)

/// Truth that the thread which a \ref future_thread_cache_t belongs to has
/// exited, and thus the TSS destructor cannot be called anymore.
///
/// See \ref future_thread_cache_t::reference_flags.
#define THREAD_EXITED_FLAG ((size_t)2)

/// Set up a thread-local futures cache
// TODO docs, implement
// TODO highlight that this returns a pointer due to TSS usage and sharing
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
future_thread_cache_t* future_thread_cache_initialize(udipe_context_t* context);

/// Handle the context finalization half of thread-local cache finalization
///
/// As explained in the documentation of \ref
/// future_thread_cache_t::spill_or_liberate, the exact behavior of this
/// function depends on the relative ordering of udipe_finalize() and thread
/// exit. But at the time this function returns, it is guaranteed that...
///
/// - All contents of the target \ref future_thread_cache_t have either been
///   liberated or spilled into the associated \ref future_context_cache_t.
/// - If the thread has not exited yet, its TSS destructor will not later
///   attempt to access the \ref udipe_context_t again, including its \ref
///   future_context_cache_t.
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
/// As explained in the documentation of \ref
/// future_thread_cache_t::spill_or_liberate, the exact behavior of this
/// function depends on the relative ordering of udipe_finalize() and thread
/// exit. But at the time this function returns, it is guaranteed that...
///
/// - All contents of the target \ref future_thread_cache_t have either been
///   liberated or spilled into the associated \ref future_context_cache_t.
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
