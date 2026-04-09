#pragma once

//! \file
//! \brief Internal utilities around \ref udipe_duration_ns_t
//!
//! This module addresses the impedance mismatch between publid duration-based
//! udipe APIs (like network operation timeouts) and the OS-specific format
//! expected by Linux and Windows APIs.

#include <udipe/duration.h>

#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "log.h"

#include <assert.h>
#include <time.h>


/// Generate a `struct timespec*` pointer parameter for a Unix API timeout
///
/// Many Unix APIs take a `struct timespec* timeout` parameter, where `NULL`
/// indicates an infinite timeout. This function turns the \ref
/// udipe_duration_ns_t duration specification into a parameter suitable for use
/// in such a function.
///
/// It takes as input to a `struct timespec` that should have been allocated on
/// the caller's stack, which will be filled with appropriate data if the
/// timeout is not `NULL`.
///
/// The output pointer will either point to this `struct timespec` or be `NULL`.
///
/// This function must be called in the scope of with_logger().
///
/// \param timespec should point to a `struct timespec` that can be filled with
///                 a Linux-formatted timeout if need be.
/// \param timeout indicates which timeout should be configured in the udipe
///                format. This parameter should not take the value \ref
///                UDIPE_DURATION_DEFAULT, which should have been translated to
///                \ref UDIPE_DURATION_MAX higher up the abstraction stack.
///
/// \returns either `timespec` with an appropriately modified target or `NULL`.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
static inline
struct timespec* make_unix_timeout(struct timespec* timespec,
                                   udipe_duration_ns_t timeout) {
    assert(timeout != UDIPE_DURATION_DEFAULT);
    if (timeout == UDIPE_DURATION_MAX) {
        trace("Setting up an infinite Unix timeout...");
        return NULL;
    } else if (timeout == UDIPE_DURATION_MIN) {
        trace("Setting up an instantaneous Unix timeout...");
        *timespec = (struct timespec){ 0 };
    } else {
        tracef("Setting up a Unix timeout of %zu.%06zu ms...",
               (size_t)(timeout / UDIPE_MILLISECOND),
               (size_t)(timeout % UDIPE_MILLISECOND));
        *timespec = (struct timespec){ .tv_sec = timeout / UDIPE_SECOND,
                                       .tv_nsec = timeout % UDIPE_SECOND };
    }
    return timespec;
}
