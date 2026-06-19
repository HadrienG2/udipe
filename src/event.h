#pragma once

//! \file
//! \brief Event objects
//!
//! This module provides utilities for manipulating event objects, which get
//! signaled and reset per user request in such a way that other threads can
//! wait for one of multiple event objects to be signaled.

#include <udipe/nodiscard.h>

#include "error.h"
#include "log.h"

#ifdef __linux__
    #include "fd.h"

    #include <errno.h>
    #include <sys/eventfd.h>
    #include <unistd.h>
#elif defined(_WIN32)
    #include <synchapi.h>
#endif


/// Event object
///
/// This object can be signaled by a thread and awaited by any number of other
/// threads. It is an eventfd on Linux and an event object on Windows.
///
/// On Linux, the eventfd can be awaited by waiting for file descriptor
/// readability via poll, epoll/inpoll, io_uring and friends. Importantly, the
/// wait method should not involve reading from the file descriptor, as this
/// would reset the event to an unsignaled state.
///
/// On Windows, the event object can be awaited using methods for awaiting
/// synchronization object readiness including WaitForSingleObject,
/// WaitForMultipleObjects, or SetThreadPoolWait.
///
/// The reason why the underlying OS object is exposed instead of abstracting
/// the waiting method away is that operating systems provide multiple ways to
/// wait for synchronization objects and the optimal choice of waiting method is
/// context-dependent.
#ifdef __linux__
    typedef fd_t event_t;
#elif defined(_WIN32)
    typedef HANDLE event_t;
#else
    #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
#endif

/// Invalid event object identifier
///
/// This identifier is recognized as invalid by the operating system and can be
/// used as a safe placeholder value when you have storage for an event object
/// that does not currently hold an actual event object.
#ifdef __linux__
    #define EVENT_INVALID  FD_INVALID
#elif defined(_WIN32)
    #define EVENT_INVALID  NULL
#else
    #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
#endif

/// Set up an unsignaled \ref event_t
///
/// The event object can be signaled with event_signal() and reset to an
/// unsignaled state with event_reset(). Other threads can wait for it to be
/// signaled with fd wait methods on Linux and synchronization object wait
/// methods on Windows. It is destroyed via event_finalize().
///
/// This function must be called within the scope of with_logger().
///
/// \param signaled indicates whether the event should initially be in the
///                 signaled state.
///
/// \returns an initialized event_t
UDIPE_NODISCARD
static inline
event_t event_initialize(bool signaled) {
    #ifdef __linux__
        int maybe_eventfd = eventfd((unsigned)signaled,
                                    EFD_CLOEXEC | EFD_NONBLOCK);
        if (maybe_eventfd == -1) switch(errno) {
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
        case EINVAL:  // An unsupported value was specified in flags.
        case ENODEV:  // Could not mount (internal) anonymous inode device.
        case ENOMEM:  // Not enough memory to create a new eventfd.
            exit_after_c_error("This error is not expected to happen");
        }
        ensure_ge(maybe_eventfd, 0);
        debugf("Set up an event object with fd %d.", maybe_eventfd);
        return maybe_eventfd;
    #elif defined(_WIN32)
        HANDLE handle = CreateEventA(NULL,
                                     true,
                                     signaled,
                                     NULL);
        win32_exit_on_null(handle,
                           "Failed to create an anonymous event object");
        debugf("Set up an event object with handle %p.", handle);
        return handle;
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
}

#ifdef __linux__
    /// Mechanism for casting beteen a typed and untyped event payload
    ///
    typedef union event_payload_u {
        uint64_t payload;  ///< Typed payload for high-level interpretation
        char chars[8];  ///< Untyped buffers for read/write syscalls
    } event_payload_t;
#endif

/// Switch an \ref event_t to the signaled state
///
/// This will unblock clients waiting for the event to be signaled. Any client
/// which subsequently attempts to await the event's readiness will also see the
/// wait return immediately.
///
/// This function must be called within the scope of with_logger().
///
/// \param event must be an event_t that was initialized with event_initialize()
///              and hasn't been destroyed with event_finalize() yet.
static inline
void event_signal(event_t event) {
    #ifdef __linux__
        debugf("Signaling the event object with fd %d...", event);
        event_payload_t addend = (event_payload_t){ .payload = 1 };
        exit_on_negative(write(event, addend.chars, sizeof(addend.chars)),
                         "Failed to signal eventfd");
    #elif defined(_WIN32)
        debugf("Signaling the event object with handle %p...", event);
        win32_exit_on_zero(SetEvent(event));
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
}

/// Switch an \ref event_t to the unsignaled state
///
/// Starting from the call to this function, clients which attempt to wait for
/// this event object's readiness will block until event_signal() is called.
///
/// This function must be called within the scope of with_logger().
///
/// \param event must be an event_t that was initialized with event_initialize()
///              and hasn't been destroyed with event_finalize() yet.
static inline
void event_reset(event_t event) {
    #ifdef __linux__
        debugf("Resetting the event object with fd %d...", event);
        event_payload_t total;
        const int result = read(event, total.chars, sizeof(total.chars));
        if (result == -1) switch(errno) {
        case EAGAIN:  // eventfd not signaled but is in nonblocking mode
            warn("Reset event which was not in the signaled state.");
            return;
        case EINVAL:  // size of the supplied buffer is less than 8 bytes.
        default:
            exit_after_c_error("These error cases should not be encountered.");
        }
        ensure_eq(result, 8);
        tracef("Reset event which was previously signaled %zu times.",
               total.payload);
    #elif defined(_WIN32)
        debugf("Resetting the event object with handle %p...", event);
        win32_exit_on_zero(ResetEvent(event));
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
}

/// Destroy an \ref event_t which is not needed anymore
///
/// This function should be called at a time where no client is waiting for the
/// event object and no client could start waiting for it.
///
/// Note that building event objects is relatively expensive, and therefore
/// resetting and recycling should usually be preferred over destroying and
/// recreating them.
///
/// This function must be called within the scope of with_logger().
///
/// \param event must point to an event_t that was initialized with
///              event_initialize() and hasn't been destroyed with
///              event_finalize() yet.
UDIPE_NON_NULL_ARGS
static inline
void event_finalize(event_t* event) {
    #ifdef __linux__
        debugf("Destroying the event object with fd %d...", *event);
        close_virtual_fd(event);
    #elif defined(_WIN32)
        debugf("Destroying the event object with handle %p...", *event);
        win32_exit_on_zero(CloseHandle(*event));
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
    *event = EVENT_INVALID;
}


// TODO: Add unit tests
