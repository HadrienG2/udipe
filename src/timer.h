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

    #include <errno.h>
    #include <sys/eventfd.h>
    #include <unistd.h>
#endif


/// Single-shot timer object
///
/// This object gets signaled at a specific time, and will stay signaled after
/// that until the user resets it to a different time. It is a timerfd on Linux
/// and a waitable timer object on Windows.
///
/// On Linux, a timerfd can be awaited by waiting for file descriptor
/// readability via poll, epoll/inpoll, io_uring and friends. Importantly, the
/// wait method should not involve reading from the file descriptor, as this
/// would reset the timer to an unsignaled state.
///
// TODO: Explain how it is on Windows and conclude, see event.h
// TODO: Write rest as in event.h, add a repeating timer object