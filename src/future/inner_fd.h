#pragma once

//! \file
//! \brief Inner file descriptor of \ref udipe_future_t

#ifdef __unix__

    #include "../fd.h"


    /// Inner file descriptor from a chained future
    ///
    /// Unordered and repeating timer operations emit a chain of futures. On
    /// Linux, these child futures are generated based on information from a
    /// long-lived internal file descriptor which is...
    ///
    /// - An epollfd attached to the upstream futures for unordered futures
    /// - A repeating timerfd for repeating timer futures.
    ///
    /// Unfortunately, said internal file descriptor cannot be directly exposed
    /// as the `status_sync` fd of these futures. If we did that, we could not
    /// simultaneously enforce all of the following important properties:
    ///
    /// - Older futures in the chain must keep a signaled fd until they are
    ///   liberated, letting clients know that they have reached their final
    ///   state.
    /// - The latest future in the chain must keep its fd in an unsignaled state
    ///   until the next event of interest occurs.
    /// - The fd of a future can not be allowed to change from the moment a
    ///   future is allocated to an operation to the moment where the future is
    ///   destroyed, if we want to keep epoll-based waiting easy and effective.
    ///
    /// Instead, the `status_sync` fd of these "chained" futures is actually an
    /// epollfd which is initially attached to two other file descriptors:
    ///
    /// - The "inner" file descriptor that signals the event of interest, tagged
    ///   with this typedef.
    /// - An \ref epoll_latch_event_t epollfd.
    ///
    /// Through this peculiar cascading file descriptor configuration, the event
    /// of interest can then be handled in the following manner:
    ///
    /// - Readiness from the inner fd is automatically signaled by
    ///   `status_sync.latched_epoll`, letting client knowns that this future is
    ///   ready to make progress (and possibly change status as a result).
    /// - Clients proceed to lazily update the future state as they wait for
    ///   this future to complete, following the usual logic of lazy udipe
    ///   futures.
    /// - Once the future reaches its final state, the inner fd is detached from
    ///   `status_sync.latched_epoll` and reconfigured as appropriate for the
    ///   successor future, to which it is subsequently attached.
    /// - After building the final result, the final \ref future_status_t is set
    ///   and the \ref epoll_latch_event_t is signaled to notify clients that
    ///   this future has reached its final state.
    ///
    /// Whenever this pattern is used, this `int` typedef is used to annotate
    /// the internal fd that signals the event of interest, so that relevant
    /// documentation does not need to be replicated in two different places.
    typedef fd_t inner_fd_t;

#else
    #error "This header should only be used on Unix operating systems."
#endif  // __unix__
