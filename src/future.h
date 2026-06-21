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
#include <udipe/result.h>

#include "future/join_state.h"
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
        /// a \ref future_type_t is in range from \ref TYPE_NETWORK_START
        /// inclusive to \ref TYPE_NETWORK_END exclusive.
        ///
        /// Upon completion or internal failure of the network operation, the
        /// udipe implementation will set this to the associated result before
        /// signaling the outcome with `memory_order_release`.
        ///
        /// The precise \ref future_type_t that you are dealing with will tell
        /// you which variant of this payload union has been set.
        udipe_network_payload_t network;

        /// Custom command result
        ///
        /// This union variant is used for custom operations, corresponding to
        /// \ref TYPE_CUSTOM.
        ///
        /// Aside from the fact that it is set by a user thread, rather than by
        /// the udipe implementation, it works just like the `network` variant.
        udipe_custom_payload_t custom;

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


#ifdef UDIPE_BUILD_TESTS
    /// Unit tests
    ///
    /// This function runs all the unit tests for this module. It must be called
    /// within the scope of with_logger().
    void future_unit_tests();
#endif
