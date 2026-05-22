#pragma once

//! \file
//! \brief Future state that is specific to \ref TYPE_JOIN

#include "collective_upstream.h"

#ifdef __linux__
    #include "epoll_latch_event.h"
#endif


/// Joined future state
///
/// This is the \ref udipe_future_t::specific variant used for \ref TYPE_JOIN.
/// It tracks the state needed to wait for all specified upstream futures to
/// reach \ref OUTCOME_SUCCESS or at least one of them to reach a failing
/// outcome. And when this happens, it makes it possible to signal availability
/// of the final status after it has been set.
typedef struct future_join_state_s {
    /// Set of upstream futures awaited by this collective future
    ///
    /// See \ref collective_upstream_t for more information.
    collective_upstream_t upstream;

    #ifdef __linux__
        /// Event object used to keep `status_sync.latched_epoll` perma-ready
        /// after the future has reached its final state.
        ///
        /// See \ref epoll_latch_event_t for more information.
        epoll_latch_event_t epoll_latch;
    #endif
} future_join_state_t;
