#pragma once

//! \file
//! \brief Implementation of \ref udipe_future_t
//!
//! This header contains the definition of the \ref udipe_future_t type declared
//! in the public `future.h` header, and the associated source file contains the
//! definition of some methods declared there.
//!
//! However, as the future implementation has grown quite large, the rest of it
//! has been extracted into smaller code modules in the `future/` sub-directory
//! of this source tree.

#include <udipe/future.h>

#include <udipe/context.h>
#include <udipe/nodiscard.h>
#include <udipe/pointer.h>
#include <udipe/result.h>

#include "future/join_state.h"
#include "future/status.h"
#include "future/status_sync.h"
#include "future/timer_repeat_state.h"
#include "future/unordered_state.h"

#include "arch.h"

#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>


/// \copydoc udipe_future_t
struct udipe_future_s {
    /// State that is specific to a particular future type
    ///
    /// At most one of these fields will be set. Which will be set (if any)
    /// depends on the \ref future_type_t that is configured inside of this
    /// future's \ref future_status_t::type.
    alignas(FALSE_SHARING_GRANULARITY) union {
        /// Network command result
        ///
        /// This union variant is used for network operations, corresponding to
        /// a \ref future_type_t between \ref TYPE_NETWORK_START inclusive to
        /// \ref TYPE_NETWORK_END exclusive.
        ///
        /// Upon completion or internal failure of the network operation, the
        /// udipe implementation will set this to the associated result before
        /// signaling the outcome with `memory_order_release`.
        ///
        /// The precise \ref future_type_t that you are dealing with will tell
        /// you which variant of this payload union has been set.
        //
        // FIXME: Need a way to access the associated control block in order to
        //        detach from the upstream future on destruction and propagate
        //        cancelation signals to the worker thread.
        udipe_network_payload_t network;

        /// Custom command result
        ///
        /// This union variant is used for custom operations, corresponding to a
        /// \ref future_type_t of \ref TYPE_CUSTOM.
        ///
        /// Aside from the fact that it is set by a user thread, rather than by
        /// the udipe implementation, it works just like the `network` variant.
        udipe_custom_payload_t custom_payload;

        /// Joined future state
        ///
        /// This union variant corresponds to \ref TYPE_JOIN. See \ref
        /// future_join_state_t for more information.
        future_join_state_t join;

        /// Unordered future state and result
        ///
        /// This union variant corresponds to \ref TYPE_UNORDERED. See \ref
        /// future_unordered_state_t for more information.
        future_unordered_state_t unordered;

        /// Repeating timer state and result
        ///
        /// This union variant corresponds to \ref TYPE_TIMER_REPEAT. See \ref
        /// future_timer_repeat_state_t for more information.
        future_timer_repeat_state_t timer_repeat;
    } specific;

    /// udipe context which this future belongs to
    ///
    /// Used to ensure that future methods do not need an additional context
    /// parameter after future allocation.
    udipe_context_t* context;

    /// Synchronization object signaling future status changes
    ///
    /// See \ref status_sync_t for more information.
    status_sync_t status_sync;

    /// Status word
    ///
    /// This innocent-looking 32-bit word actually contains most of the
    /// synchronization-critical state of a future, bitpacked via \ref
    /// future_status_word_t::as_word so that it can be used for atomic
    /// read-modify-write operations and futex syscalls.
    ///
    /// A future's status word does double duty as a futex that can sometimes
    /// (but not always) be awaited with wait_for_address() to await
    /// `status_word` changes. When a future supports this signaling protocol,
    /// it must be requested first by setting the `notify_address` field of the
    /// status word, before beginning the wait for status changes via
    /// wait_for_address().
    ///
    /// Please refer to \ref future_status_t for more information about what
    /// information is stored into this word.
    ///
    /// As status changes are often preceded by other future state changes, bear
    /// in mind that changes to `status_word` must often be carried out with
    /// `memory_order_release` and status word readouts must often be carried
    /// out with `memory_order_acquire`.
    _Atomic uint32_t status_word;
};
static_assert(
    alignof(udipe_future_t) == FALSE_SHARING_GRANULARITY,
     "Each future potentially synchronizes different workers and client "
     "threads, and should therefore reside on its own false sharing granule"
);
static_assert(
    sizeof(udipe_future_t) == FALSE_SHARING_GRANULARITY,
    "Should not need more than one false sharing granule per future"
);
static_assert(
    offsetof(udipe_future_t, status_sync) + sizeof(uint32_t) <= CACHE_LINE_SIZE,
    "Should fit on a single cache line for optimal memory access performance "
    "on CPUs where the FALSE_SHARING_GRANULARITY upper bound is pessimistic"
);
static_assert(
    sizeof(udipe_result_t) <= CACHE_LINE_SIZE,
    "Should be true if above is because future is largely a superset of result"
);


/// \name Misc implementation details without a better home in future/
/// \{

/// Backend of udipe_finish()
///
/// This function behaves like the user-facing udipe_finish() function, but...
///
/// - It assumes the initial future status word has already been read and
///   expects it as an input.
/// - It makes result extraction optional for the benefit of udipe_cancel(),
///   which finishes asynchronous operations without awaiting their results.
/// - It must be called within a logging scope.
///
/// \param future must be a future that was returned by an asynchronous function
///               (those whose name begins with `udipe_start_`) and has not been
///               liberated by udipe_finish() or udipe_cancel() since. It will
///               be destroyed by this function and must not be used afterwards.
/// \param latest_status must contain the latest value of the future's status word that
///                      is known to the caller.
/// \param result may point to a caller-allocated \ref udipe_result_t to which
///               the asynchronous operation's result will be extracted, or it
///               may be NULL to indicate lack of interest in the asynchronous
///               operation's result.
UDIPE_NON_NULL_SPECIFIC_ARGS(1)
void future_finish(udipe_future_t* future,
                   future_status_t latest_status,
                   udipe_result_t* result);

/// Notify other threads that an eager future has reached a final status
///
/// Eager futures are those that are directly signaled by a udipe or user
/// thread, like network and custom futures. They stand in contrast with lazy
/// futures, which are signaled by internal OS mechanisms, like TIMER_ONCE
/// futures on all OSes and collective futures based on inpoll on Linux.
///
/// This function must be called within a logging scope.
///
/// \param future must be a future of an eager type.
/// \param status must be the status of this future at the time where the final
///               outcome was signaled.
UDIPE_NON_NULL_ARGS
void future_notify_eager_outcome(udipe_future_t* future,
                                 future_status_t status);

/// Check the status of a custom future which should not have completed, and
/// report whether it was canceled
///
/// This function must be called within a logging scope.
///
/// \param status is the status of the custom future of interest
/// \returns whether the future was canceled (true) or not (false)
UDIPE_NODISCARD
bool future_custom_check_canceled(future_status_t status);

/// \}


#ifdef UDIPE_BUILD_TESTS
    /// Unit tests for custom futures
    ///
    /// This function runs the unit tests for custom future operations. It must
    /// be called within a logging scope.
    void future_custom_unit_tests();
#endif
