#pragma once

//! \file
//! \brief Timer objects
//!
//! This module provides utilities for manipulating timer objects, which get
//! signaled at one or more user-specified time points and get reset per user
//! request, in such a way that other threads can wait for one of multiple timer
//! objects to be signaled.
//!
//! Timers should not be confused with stopwatches, which are used to track the
//! passage of time in a non-blocking manner.

#include <udipe/duration.h>
#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "error.h"
#include "log.h"

#ifdef __linux__
    #include "fd.h"
#elif defined(_WIN32)
    // Must be included first
    #include <windows.h>

    #include <handleapi.h>
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>


/// Invalid timer object identifier
///
/// This identifier is recognized as invalid by the operating system and can be
/// used as a safe placeholder value when you have storage for an timer object
/// that does not currently hold an actual timer object.
#ifdef __linux__
    #define TIMER_INVALID  FD_INVALID
#elif defined(_WIN32)
    #define TIMER_INVALID  NULL
#else
    #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
#endif


#ifdef __linux__
    /// \name Linux `timerfd` manipulation
    /// \{

    /// `timerfd` file descriptor
    ///
    /// Both \ref timer_once_t and \ref linux_timer_repeat_t are aliases to this
    /// powerful special Linux file descriptor type which can handle both
    /// one-shot and repeating timers.
    ///
    /// On Linux, the timerfd can be awaited by waiting for file descriptor
    /// readability via poll, epoll/inpoll, io_uring and friends. Importantly,
    /// the wait method should not involve reading from the file descriptor, as
    /// this would reset the timer to an unsignaled state and thus break the
    /// "broadcast signaling" property of timerfds as a cross-thread
    /// synchronization primitive.
    ///
    /// For repeating timers, the timerfd must be read from to measure how many
    /// timer periods have elapsed, but as outlined above this will reset it to
    /// an unsignaled state. So if you want to broadcast a readiness signal to
    /// multiple threads, you will need a wrapper based on \ref inpoll_t and
    /// \ref inpoll_latch_event_t, as done in the implementation of \ref
    /// TYPE_TIMER_REPEAT futures.
    typedef fd_t timerfd_t;

    /// Set up a timerfd with a certain deadline and an optional repeat interval
    ///
    /// The timerfd will be signaled when the `initial` deadline is reached,
    /// and optionally signaled again at the periodic `interval` if it is
    /// nonzero. It must eventually be destroyed with close_virtual_fd().
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param initial indicates at which time the timerfd should be first
    ///                signaled, using the same conventions as
    ///                udipe_start_timer_once().
    /// \param interval indicates at which time period the timerfd should be
    ///                 signaled again after `initial`, unless it is set to zero
    ///                 in which case it has no effect.
    ///
    /// \returns an initialized timerfd that must later be destroyed with
    ///          close_virtual_fd().
    UDIPE_NODISCARD
    timerfd_t timerfd_initialize(struct timespec initial,
                                 udipe_duration_ns_t interval);

    /// \}
#endif  // __linux__


#ifdef _WIN32
    /// Translate a Unix `timespec` into a Windows `FILETIME`
    ///
    /// \param ts must be a valid `timespec` that represents a number of
    ///           nanoseconds elapsed since the Unix epoch (Midnight, January 1,
    ///           1970, UTC).
    /// \param round_up indicates whether the result should be rounded up to the
    ///                 next Windows clock tick if it falls between two clock
    ///                 ticks. By default the result is rounded down.
    ///
    /// \returns a Windows `FILETIME` i.e. a number of 100ns clock ticks elapsed
    ///          since Midnight, January 1, 1601, UTC.
    static inline
    LARGE_INTEGER win32_filetime_from_timespec(struct timespec ts,
                                               bool round_up) {
        // Check timestamp validity
        assert(ts.tv_nsec < 1000*1000*1000);

        // Translate tv_sec part
        uint64_t ticks = (uint64_t)ts.tv_sec * 1000 * 1000 * 10;

        // Add tv_nsec part
        uint64_t prev_ticks = ticks;
        ticks += ts.tv_nsec / 100
        assert(ticks >= prev_ticks);
        if (round_up && ts.tv_nsec % 100 != 0) {
            prev_ticks = ticks;
            ticks += 1;
            assert(ticks > prev_ticks);
        }

        // Add number of elapsed 100ns ticks between January 1, 1601 and January
        // 1, 1970, according to the implementation of TimeToFileTime() from
        // https://learn.microsoft.com/en-us/windows/win32/sysinfo/converting-a-time-t-value-to-a-file-time
        prev_ticks = ticks;
        ticks += 116444736000000000ULL;
        assert(ticks > prev_ticks);

        // Return the result as a LARGE_INTEGER
        assert(ticks < (uint64_t)INT64_MAX);
        return (LARGE_INTEGER){ .QuadPart = (int64_t)ticks };
    }
#endif  // _WIN32


/// \name One-shot timers
/// \{

/// One-shot timer object
///
/// This object gets signaled at a specific time, and will stay signaled after
/// that until the user resets it to a different time. It is a timerfd on Linux
/// (see \ref timerfd_t for more) and a waitable timer object on Windows.
///
/// On Windows, the waitable timer object can be awaited using methods for
/// awaiting synchronization object readiness including WaitForSingleObject,
/// WaitForMultipleObjects, or SetThreadPoolWait.
///
/// The reason why the underlying OS object is exposed instead of abstracting
/// the waiting method away is that operating systems provide multiple ways to
/// wait for synchronization objects and the optimal choice of waiting method is
/// context-dependent.
#ifdef __linux__
    typedef timerfd_t timer_once_t;
#elif defined(_WIN32)
    typedef HANDLE timer_once_t;
#else
    #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
#endif

/// Set up a one-shot timer
///
/// The timer object will be signaled when the specified `deadline` is reached.
/// It must eventually be destroyed via timer_once_finalize().
///
/// This function must be called within the scope of with_logger().
///
/// \param deadline indicates at which time the timer object should be signaled,
///                 using the same conventions as udipe_start_timer_once().
///
/// \returns an initialized one-shot timer object that must later be destroyed
///          with timer_once_finalize().
UDIPE_NODISCARD
timer_once_t timer_once_initialize(struct timespec deadline);

/// Destroy a one-shot timer object which is not needed anymore
///
/// This function must be called at a time where no client is waiting for the
/// timer object and no client could start waiting for it.
///
/// Unlike with event objects, you are not encouraged to recycle and reuse
/// one-shot timer objects because all foreseen used cases for one-shot timer
/// object reuse are better left to repeating timer objects.
///
/// This function must be called within the scope of with_logger().
///
/// \param timer must point to a one-shot timer object that was initialized with
///              timer_once_initialize() and hasn't been destroyed with
///              timer_once_finalize() yet.
UDIPE_NON_NULL_ARGS
static inline
void timer_once_finalize(timer_once_t* timer) {
    #ifdef __linux__
        debugf("Destroying the one-shot timer object with fd %d...", *timer);
        close_virtual_fd(timer);
    #elif defined(_WIN32)
        debugf("Destroying the one-shot timer object with handle %p...", *timer);
        win32_exit_on_zero(CloseHandle(*timer),
                           "Failed to destroy timer object!");
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
    *timer = TIMER_INVALID;
}

/// \}


#ifdef __linux__
    /// \name Linux repeating timers
    /// \{

    /// Linux repeating timer
    ///
    /// At the time of writing, Linux timerfds are the only available OS API
    /// that can provide repeating timers as a synchronization object, which can
    /// be awaited along with other synchronization objects then later be
    /// atomically reset in a manner that tells you how many deadlines were
    /// missed (which is done by simply reading from the timerfd).
    ///
    /// Windows waitable timer objects get close, but unfortunately lack the
    /// ability to report the number of missed deadlines. And emulating this
    /// feature with thread pool timers is complex and expensive enough that we
    /// decided to just use thread pool timers directly in the implementation of
    /// timer futures instead of exposing an intermediate timer synchronization
    /// object layer. Hence only Linux gets a \ref linux_timer_repeat_t for now.
    ///
    /// The reason why the timerfd is exposed instead of abstracting the waiting
    /// method away is that Linux provide multiple ways to wait for fds and the
    /// optimal choice of waiting method is context-dependent.
    typedef timerfd_t linux_timer_repeat_t;

    /// Set up a Linux repeating timer
    ///
    /// The timer object will be signaled at the specified `initial` instant,
    /// then again every `interval` nanoseconds. It must eventually be destroyed
    /// via linux_timer_repeat_finalize().
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param initial indicates at which time the timer object should be first
    ///                signaled, using the same conventions as
    ///                udipe_start_timer_once().
    /// \param interval indicates at which time interval the timer object should
    ///                 be signaled again after this initial signal.
    ///
    /// \returns an initialized Linux repeating timer object that must later be
    ///          destroyed with linux_timer_repeat_finalize().
    UDIPE_NODISCARD
    static inline
    linux_timer_repeat_t timer_repeat_initialize(struct timespec initial,
                                                 udipe_duration_ns_t interval) {
        return timerfd_initialize(initial, interval);
    }

    /// Destroy a Linux repeating timer object which is not needed anymore
    ///
    /// This function must be called at a time where no client is waiting for
    /// the timer object and no client could start waiting for it.
    ///
    /// Unlike with event objects, you are not encouraged to recycle and reuse
    /// repeating timer objects because there is no foreseen valid use case for
    /// repeatedly creating and destroying lots of these.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param timer must point to a Linux repeating timer object that was
    ///              initialized with linux_timer_repeat_initialize() and hasn't
    ///              been destroyed with linux_timer_repeat_finalize() yet.
    UDIPE_NON_NULL_ARGS
    static inline
    void linux_timer_repeat_finalize(linux_timer_repeat_t* timer) {
        debugf("Destroying the one-shot timer object with fd %d...", *timer);
        close_virtual_fd(timer);
        *timer = TIMER_INVALID;
    }

    /// \}
#endif


// TODO: Unit tests
