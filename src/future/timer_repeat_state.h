#pragma once

//! \file
//! \brief Future state that is specific to \ref TYPE_UNORDERED

#include <udipe/result.h>

#include "epoll_latch_event.h"
#include "inner_fd.h"


/// Repeating timer state and result
///
/// This is the \ref udipe_future_t::specific variant used for \ref
/// TYPE_TIMER_REPEAT. It tracks the state needed to report how many timer ticks
/// elapsed and how to await subsequent timer ticks.
typedef struct future_timer_repeat_state_s {
    /// Result of the asynchronous operation
    ///
    /// This field is set before signaling \ref OUTCOME_SUCCESS. It indicates
    /// how many clock ticks were missed and how to await further clock ticks if
    /// desired.
    ///
    /// Must be written under `lazy_lock` protection. Inner future must not be
    /// recycled on udipe_finish(), as it will be fed to the caller which is
    /// responsible for liberating it.
    udipe_timer_repeat_payload_t payload;

    #ifdef __linux__
        /// timerfd that tracks recuring deadlines
        ///
        /// This inner fd is attached to `status_sync.latched_epoll`. See \ref
        /// inner_fd_t for more information about this cascading file descriptor
        /// pattern.
        ///
        /// It must be read under `lazy_lock` protection, and eventually
        /// detached from `status_sync.latched_epoll` and attached to the
        /// `latched_epoll` of the successor future (if any) once a result is
        /// ready.
        ///
        /// It must be destroyed when the future is liberated, for now. We may
        /// switch to disarming and recycling if timerfd creation/destruction
        /// ever becomes a bottleneck, but that seems unlikely under correct
        /// usage since there is no envisioned use case where one would need
        /// lots of periodic futures with different periodicities.
        //
        // TODO: Prove the above assertion through benchmarking and profiling
        //       profiling of real-world workloads.
        // TODO: Find a windows equivalent, based on Win32 thread pool timers?
        //       That seems necessary to be able to count missed deadlines,
        //       which is a very nice timerfd feature that we'd rather keep even
        //       for those poor Windows souls.
        inner_fd_t timerfd;

        /// Event object used to keep `status_sync.latched_epoll` perma-ready
        /// after the future has reached its final state.
        ///
        /// See \ref epoll_latch_event_t for more information.
        epoll_latch_event_t epoll_latch;
    #endif
} future_timer_repeat_state_t;
