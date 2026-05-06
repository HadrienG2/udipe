#pragma once

//! \file
//! \brief Upstream future tracking for collective futures (join, unordered)

#include <udipe/future.h>

#include <stdint.h>


/// Set of upstream futures which collective futures depend on
///
/// Collective joined and unordered futures all need to lazily poll a set of
/// other futures, which is tracked via this struct.
typedef struct collective_upstream_s {
    /// Array of upstream futures that this future is awaiting
    ///
    /// This array must not be accessed after the point where \ref
    /// future_status_t::state is set to \ref STATE_RESULT, as the user is
    /// allowed to liberate the associated futures after this point.
    ///
    /// Must have been checked to contain no duplicates and no `NULL`s at
    /// collective future construction time. As long as we do not expose an
    /// output fd accessor that lets users call `dup()`, `epoll_ctl()` should
    /// take care of the former for us.
    udipe_future_t* const* array;

    /// Number of upstream futures in `array`
    ///
    /// This is needed in order to be able to detach all upstream futures when a
    /// collective operation gets canceled. It can also be used for
    /// bounds-checking assertions in debug builds.
    //
    // TODO: Make sure len is not above UINT32_MAX when creating a collective.
    uint32_t length;

    /// Number of upstream futures that have not yet reached \ref
    /// OUTCOME_SUCCESS
    ///
    /// This counter is initialized to `len` then decremented every time one of
    /// the upstream futures reaches \ref OUTCOME_SUCCESS. If it reaches 0,
    /// all upstream futures have reached \ref OUTCOME_SUCCESS.
    ///
    /// - Join futures do not switch to \ref OUTCOME_SUCCESS until all upstream
    ///   futures have reached \ref OUTCOME_SUCCESS.
    /// - Unordered futures emit a new future whenever this counter is
    ///   decremented, with a `next` pointer that points to a successor future
    ///   if this counter has not reached zero yet. If this counter has reached
    ///   zero, `next` is set to `NULL`, marking the end of the unordered chain.
    ///
    /// When one of the upstream futures reaches a non-successful outcome, this
    /// counter becomes useless and is allowed to take any value.
    ///
    /// This counter must be read and written under `lazy_lock` protection.
    uint32_t remaining;
} collective_upstream_t;
