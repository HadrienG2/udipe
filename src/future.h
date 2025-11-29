#pragma once

//! \file
//! \brief Implementation of \ref udipe_future_t
//!
//! Under the hood, futures are implemented using a type that is isomorphic to
//! \ref udipe_result_t, but uses a futex instead of a dumb enum value. This
//! futex leverages the existence of the \ref UDIPE_NO_COMMAND sentinel value of
//! the \ref udipe_command_id_t result tag in order to let threads efficiently
//! wait for the result to come up.

#include <udipe/future.h>
#include <udipe/result.h>

#include "arch.h"

#include <assert.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>


/// \copydoc udipe_future_t
struct udipe_future_s {
    /// Result of the command, if any
    ///
    /// Once the underlying command is done running to completion, its result
    /// will be written down to this field.
    alignas(FALSE_SHARING_GRANULARITY) udipe_result_payload_t payload;

    /// Futex that can be used to wait for the command to run to completion
    ///
    /// It is initialized to \ref UDIPE_NO_COMMAND and used as follows:
    ///
    /// - The client thread waits for it to move away from \ref
    ///   UDIPE_NO_COMMAND, with acquire ordering upon completion.
    /// - Once the worker thread is done, it sets `payload` to the command's
    ///   result, then this futex to the appropriate \ref udipe_command_id_t
    ///   with release ordering, and finally it wakes the futex.
    _Atomic uint32_t futex;
};
static_assert(alignof(udipe_future_t) == FALSE_SHARING_GRANULARITY,
              "Each future potentially synchronizes different workers and "
              "client threads, and should therefore reside on its own "
              "false sharing granule");
static_assert(sizeof(udipe_future_t) == FALSE_SHARING_GRANULARITY,
              "Should not need more than one false sharing granule per future");
static_assert(
    offsetof(udipe_future_t, futex) + sizeof(uint32_t) <= CACHE_LINE_SIZE,
    "Should fit on a single cache line for optimal memory access performance "
    "on CPUs where the FALSE_SHARING_GRANULARITY upper bound is pessimistic"
);
static_assert(sizeof(udipe_result_t) <= CACHE_LINE_SIZE,
              "Should always be true because future is isomorphic to result");

// TODO: Implement operations
