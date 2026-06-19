#pragma once

//! \file
//! \brief Future state that is specific to \ref TYPE_UNORDERED

#include <udipe/result.h>

#ifdef __linux__
    #include "inpoll_latch_event.h"
    #include "inner_fd.h"
#endif


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
        /// This inner fd is attached to `status_sync.latched_inpoll`. See \ref
        /// inner_fd_t for more information about this cascading file descriptor
        /// pattern.
        ///
        /// It must be read under `lazy_lock` protection, and eventually
        /// detached from `status_sync.latched_inpoll` and attached to the
        /// `latched_inpoll` of the successor future (if any) once a result is
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
        // TODO: Find a Windows equivalent that retains the important ability to
        //       count missed deadlines. The current plan is to do it using a
        //       Win32 thread pool timer, which is recuring if `interval` is an
        //       integral number of milliseconds and single-shot otherwise. In
        //       both cases, the timer callback determines the amount of missed
        //       timer deadlines using GetSystemTimePreciseAsFileTime() and a
        //       variable that tracks the last lookup time, then sets up the
        //       continuation future accordingly then marks this future as
        //       ready. But in the single-shot case, the next timer period is
        //       also determined and configured as the next deadline of the
        //       single-shot thread pool timer before the future is marked as
        //       ready. This complication is needed because Windows timers only
        //       natively support millisecond periodicities, which is why users
        //       are strongly advised to use those periodicities when possible.
        inner_fd_t timerfd;

        /// Event object used to keep `status_sync.latched_inpoll` perma-ready
        /// after the future has reached its final state.
        ///
        /// See \ref inpoll_latch_event_t for more information.
        inpoll_latch_event_t inpoll_latch;
    #endif
} future_timer_repeat_state_t;
