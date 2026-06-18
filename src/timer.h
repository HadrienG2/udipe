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
#endif


#ifdef __linux__
    /// \name Linux-specific utilities
    /// \{

    /// timerfd file descriptor
    ///
    /// Both \ref timer_once_t and \ref timer_repeat_t are aliases to this
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
#else
    // TODO add windows version based on waitable timer objects
    #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
#endif

// TODO: Write rest of timer_once, taking inspiration from event.h and factoring
//       out timerfd commonalities in the "Linux-specific utilities" section.

/// \}


/// \name Repeating timers
/// \{

/// Repeating timer object
///
/// This object gets signaled at a user-specified time, then gets signaled again
/// and again at fixed time intervals. After at least one such signaling has
/// occured, the user can atomically reset the object to an unsignaled state and
/// determine how many times it's been signaled since it was last queried in
/// this manner. The implementation is a timerfd on Linux (see \ref timerfd_t)
/// and a nontrivial struct based on an NT semaphore and a thread pool timer on
/// Windows.
///
/// Windows needs this nontrivial implementation because it lacks a built-in way
/// to count missed timer deadlines, which means that this feature must be
/// emulated by setting up a thread pool timer whose callback releases an NT
/// semaphore once per elapsed timer period. By subsequently performing
/// nonblocking waits on this semaphore until the wait fails, the user can get
/// the number of ticks that elapsed on the other side. This approach gets
/// expensive on the reader side for pathologically small timer intervals where
/// missing many timer periods is common, but it is remarkably cheap when the
/// timer interval is large enough and an integer number of milliseconds, and it
/// is also much simpler than the alternative of combining a counter and an
/// event object that are always manipulated together under a critical section.
///
/// On Linux, the timerfd is exposed directly to let the user pick the fd wait
/// method that best fits their use case among the many ones available. On
/// Windows, our unconventional usage of semaphores to count timer periods makes
/// this approach intractable, so a more traditional abstraction approach is
/// used where you should use special methods for everything.
#ifdef __linux__
    typedef timerfd_t timer_repeat_t;
#else
    // TODO add windows version based on thread pool timers and semaphores
    #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
#endif

// TODO: - Write timer_repeat_t, taking inspiration from timer_once and event.h
//         and factoring out timerfd commonalities in the "Linux-specific
//         utilities" section.
//       - If the Windows impl is configured with a period that is a multiple of
//         UDIPE_MILLISECOND, use a repeating thread pool timer with this period.
//       - If the Windows impl is configured with a period that is not a
//         multiple of UDIPE_MILLISECOND, then use a one-shot thread pool timer
//         that gets reset to the next period's deadline on each callback
//         invocation.
//       - In both cases, use GetSystemTimePreciseAsFileTime to track the number
//         of periods that elapsed since the last call to the callback, rounding
//         to the appropriate integer period count (which can be the one above
//         if we're sufficiently close to the next deadline), then pass that to
//         SemaphoreRelease to signal the semaphore the appropriate number.
//       - On the client side, repeatedly use WaitForSingleObject in nonblocking
//         mode until it fails to destructively read the number of timer
//         deadlines that elapsed.
//       - Expose a way to get access to the semaphore handle, warning that it
//         must only be used for waiting and not for any other purpose.

/// \}


// TODO: Unit tests
