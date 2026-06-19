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

#ifdef __linux__
    #include "fd.h"

    #include <sys/timerfd.h>
#elif defined(_WIN32)
    // Must be included first
    #include <windows.h>

    #include <handleapi.h>
    #include <synchapi.h>
#endif


#ifdef __linux__
    /// \name Linux `timerfd` manipulation
    /// \{

    /// `timerfd` file descriptor
    ///
    /// Both \ref timer_once_t and \ref linux_timer_repeat_t are aliases to this
    /// powerful special Linux file descriptor type which can handle both
    /// one-shot and repeating timers with ease.
    ///
    /// On Linux, the timerfd can be awaited by waiting for file descriptor
    /// readability via poll, epoll/inpoll, io_uring and friends. Importantly,
    /// the wait method should not involve reading from the file descriptor, as
    /// this would reset the timer to an unsignaled state and thus break the
    /// "broadcast signaling" property of timerfds as a synchronization
    /// primitive.
    ///
    /// For repeating timers, the timerfd must be read from to measure how many
    /// timer periods have elapsed, however, but as outlined above this will
    /// reset it to an unsignaled state. So if you want to broadcast a readiness
    /// signal to multiple threads, you will need a wrapper based on \ref
    /// inpoll_t and \ref inpoll_latch_event_t, as done in the implementation of
    /// \ref TYPE_TIMER_REPEAT futures.
    typedef fd_t timerfd_t;

    /// \}
#endif  // __linux__


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

// TODO: Write the timer_once API, taking inspiration from event.h and factoring
//       out timerfd commonalities in the "Linux-specific utilities" section.

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

    // TODO: Write rest of linux_timer_repeat API, taking inspiration from
    //       event.h and factoring out timerfd commonalities in the
    //       "Linux-specific utilities" section.

    /// \}
#endif


// TODO: Unit tests
