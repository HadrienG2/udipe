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

    #include <sys/timerfd.h>
#elif defined(_WIN32)
    // Must be included first
    #include <windows.h>

    #include <handleapi.h>
    #include <synchapi.h>
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>


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


/// \name Timer objects
/// \{

/// Timer object
///
/// This is an object that gets signaled at user-specified time points. It can
/// be configured in up to two ways depending on which OS you are using:
///
/// - On all supported operating systems, it can be configured in one-shot mode
///   with timer_set_once(). In this mode, once a specific deadline is reached,
///   the timer will be signaled, and afterwards the timer will remain signaled
///   until destroyed or reset.
/// - On Linux, a timer can additionally be configured in repeating mode with
///   linux_timer_set_repeat(). In this mode, there is both an initial deadline
///   and a subsequent repetition interval. After being signaled at the initial
///   deadline, the timer will repeatedly be signaled again at the specified
///   time interval, with a way to atomically reset it to an unsignaled state
///   and query how many timer periods have occured since the last reset.
///     - Repeating timers are not supported on Windows because Windows waitable
///       timer objects do not support tracking the amount of missed deadlines
///       or following fine-grained periods, everything must be emulated using
///       thread pool timers, and doing so just to provide the illusion that
///       waitable timers can do it is more complex and expensive than simply
///       using thread pool timers directly in the implementation of repeating
///       timer futures.
///
/// Under the hood, this object is a timerfd on Linux and a waitable timer
/// object on Windows.
///
/// Linux timerfd deadlines can be awaited using any syscall that monitors fd
/// readability in a nondestructive manner (select, poll, epoll/inpoll,
/// io_uring...). But in order to read the missed deadline count and reset a
/// repeating timer, you need to destructively read from this file descriptor,
/// which resets it to an unsignaled state and may therefore break the ability
/// for a single timerfd to wake up multiple threads. This can be worked around
/// by adding a layer of \ref inpoll_latch_event_t indirection, as done in the
/// implementation of repeating timer futures.
///
/// Windows waitable timer objects can be awaited using methods for awaiting
/// synchronization object readiness including WaitForSingleObject,
/// WaitForMultipleObjects, or SetThreadPoolWait.
///
/// The reason why the underlying OS object is exposed instead of abstracting
/// the waiting method away is that operating systems provide multiple ways to
/// wait for synchronization objects and the optimal choice of waiting method is
/// context-dependent.
#ifdef __linux__
    typedef fd_t udipe_timer_t;
#elif defined(_WIN32)
    typedef HANDLE udipe_timer_t;
#else
    #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
#endif

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

/// Set up a timer object
///
/// The timer object will be created in an unsignaled state, and a deadline
/// and/or period must be configured with timer_set_once() or
/// linux_timer_set_repeat() before it starts being signaled at user-specified
/// time points. Once you are done with it, the timer object must be destroyed
/// with timer_finalize().
///
/// This function must be called within the scope of with_logger().
///
/// \returns an initialized timer object that must later be destroyed with
///          timer_finalize().
UDIPE_NODISCARD
static inline
udipe_timer_t timer_initialize() {
    #ifdef __linux__
        int maybe_timerfd = timerfd_create(
            CLOCK_REALTIME,
            // No need for TFD_NONBLOCK as these fds should never be read from
            // until the associated future is destroyed and they are liberated.
            TFD_CLOEXEC
        );
        if (maybe_timerfd == -1) switch(errno) {
        case EMFILE:  // Reached process fd limit
            exit_after_c_error(
                "The number of fds in current process reached the limit. "
                "Consider increasing the limit if possible."
            );
        case ENFILE:  // Reached system fd limit
            exit_after_c_error(
                "The number of fds in the system reached the limit. "
                "Consider increasing the limit if possible."
            );
        case EINVAL:  // clockid or flags is invalid.
        case ENODEV:  // Could not mount (internal) anonymous inode device.
        case ENOMEM:  // Not enough memory to create a new timerfd.
            exit_after_c_error("This error is not expected to happen");
        }
        ensure_ge(maybe_timerfd, 0);
        debugf("Created a timer object with fd %d.", maybe_timerfd);
        return maybe_timerfd;
    #elif defined(_WIN32)
        HANDLE handle = CreateWaitableTimerExW(
            NULL,
            NULL,
            // Needed to allow N threads to wait on one future
            CREATE_WAITABLE_TIMER_MANUAL_RESET
                // Needed because otherwise we get ~16ms resolution which is
                // miserable by the standards of high-performance UDP.
                | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            DELETE | SYNCHRONIZE | TIMER_MODIFY_STATE
        );
        win32_exit_on_null(handle,
                           "Failed to create an anonymous timer object!");
        debugf("Set up a timer object with handle %p.", handle);
        return handle;
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
}

/// Set up a timer object in one-shot mode
///
/// This will configure the timer object to become signaled at the specified
/// `deadline`, resetting it to an unsignaled state if it was previously
/// signaled.
///
/// This function must be called within the scope of with_logger().
///
/// \param timer must point to a timer object that was initialized with
///              timer_initialize() and hasn't been destroyed with
///              timer_finalize() yet.
/// \param deadline indicates at which time the timer object should be
///                 signaled using the same conventions as
///                 udipe_start_timer_once().
static inline
void timer_set_once(udipe_timer_t timer, struct timespec deadline) {
    assert(deadline.tv_nsec < 1000 * 1000 * 1000);
    #ifdef __linux__
        debugf("Setting a deadline for the timer object with fd %d...", timer);
        struct itimerspec spec = { 0 };
        spec.it_value = deadline;
        exit_on_negative(
            timerfd_settime(
                timer,
                // Don't want to expose TFD_TIMER_CANCEL_ON_SET as it has no
                // clean equivalent on Windows.
                TFD_TIMER_ABSTIME,
                &spec,
                NULL
            ),
            "Failed to configure a timerfd's deadline!"
        );
    #elif defined(_WIN32)
        debugf("Setting a deadline for timer object with handle %p...", timer);
        const LARGE_INTEGER due = win32_filetime_from_timespec(due, false);
        win32_exit_on_zero(
            SetWaitableTimer(
                timer,
                &due,
                0,
                NULL,
                NULL,
                false
            ),
            "Failed to configure a Windows timer's deadline!"
        );
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
}

#ifdef __linux__
    /// Set up a Linux timer in repeating mode
    ///
    /// This will configure the timer object to become signaled at the specified
    /// `initial` instant, then again every `interval` nanoseconds, resetting it
    /// to an unsignaled state if it was previously signaled.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param timer must point to a timer object that was initialized with
    ///              timer_initialize() and hasn't been destroyed with
    ///              timer_finalize() yet.
    /// \param initial indicates at which time the timer object should be first
    ///                signaled using the same conventions as
    ///                udipe_start_timer_once().
    /// \param interval indicates at which time interval the timer object should
    ///                 be signaled again after the initial signal.
    static inline
    void linux_timer_set_repeat(udipe_timer_t timer,
                                struct timespec initial,
                                udipe_duration_ns_t interval) {
        debugf("Setting a deadline and repetition interval "
               "for the timer object with fd %d...", timer);
        assert(initial.tv_nsec < 1000 * 1000 * 1000);
        const struct itimerspec spec = {
            .it_value = initial,
            .it_interval = (struct timespec){
                .tv_sec = interval / (1000 * 1000 * 1000),
                .tv_nsec = interval % (1000 * 1000 * 1000)
            }
        };
        exit_on_negative(
            timerfd_settime(
                timer,
                // Don't want to expose TFD_TIMER_CANCEL_ON_SET as it has no
                // clean equivalent on Windows.
                TFD_TIMER_ABSTIME,
                &spec,
                NULL
            ),
            "Failed to configure a timerfd's deadline and period!"
        );
    }
#endif  // __linux__

/// Destroy a timer object which is not needed anymore
///
/// This function must be called at a time where no client is waiting for the
/// timer object and no client could start waiting for it.
///
/// Unlike with event objects, you are not encouraged to recycle and reuse timer
/// objects because 1/all known use cases for one-shot timer object reuse are
/// better left to repeating timer objects and 2/there is no known valid use
/// case for creating and deleting lots of repeating timer objects per second.
///
/// This function must be called within the scope of with_logger().
///
/// \param timer must point to a timer object that was initialized with
///              timer_initialize() and hasn't been destroyed with
///              timer_finalize() yet. It cannot be used again after calling
///              this function.
UDIPE_NON_NULL_ARGS
static inline
void timer_finalize(udipe_timer_t* timer) {
    #ifdef __linux__
        debugf("Destroying the timer object with fd %d...", *timer);
        close_virtual_fd(timer);
    #elif defined(_WIN32)
        debugf("Destroying the timer object with handle %p...", *timer);
        win32_exit_on_zero(CloseHandle(*timer),
                           "Failed to destroy a timer object!");
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
    *timer = TIMER_INVALID;
}

/// \}


// TODO: Unit tests
