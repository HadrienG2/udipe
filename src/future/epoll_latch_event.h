#pragma once

//! \file
//! \brief Mechanism to keep an epollfd signaled indefinitely

#ifdef __linux__

    #include "../event.h"


    /// eventfd used to keep an epollfd signaled indefinitely
    ///
    /// At the time of writing, the Linux implementation of futures uses
    /// epollfds in two different ways:
    ///
    /// - Collective joined and unordered futures use epoll as a way to
    ///   repeatedly await upstream futures. In this role, compared to select(),
    ///   poll() or futex_waitv(), epoll is more CPU-efficient due to its
    ///   stateful nature, which lets the set of monitored objects to be
    ///   remembered across epoll_wait() calls. And compared to io_uring, epoll
    ///   is a lot easier to understand, more compatible with older kernels, and
    ///   also more memory-efficient which matters for this unusally
    ///   fine-grained use case.
    /// - Unordered and repeated timer futures, which emit a chain of successor
    ///   futures, use epoll as an indirection layer. epollfd indirection makes
    ///   it possible to forward readiness notifications from a hidden file
    ///   descriptor for a while, then eventually "transfer" a hidden file
    ///   descriptor to the next future in the chain, without changing the
    ///   `status_sync` file descriptor number of the active future in the
    ///   process.
    ///
    /// The most straightforward and efficient way to use epoll like this while
    /// allowing futures to emit fd-based notifications, is to directly
    /// designate the resulting epollfd as the `status_sync` fd of the host
    /// future. But this requires a way to set said fd to a permanently ready
    /// state after the future has reached its final state, thus allowing all
    /// fd-based clients to reliably notice that this future has reached this
    /// stage.
    ///
    /// We do this by attaching an extra eventfd to the `status_sync` epollfd in
    /// addition to all other file descriptors that are being monitored. This
    /// eventfd is signaled once the future reaches its final state and never
    /// reset until the future is liberated.
    ///
    /// Whenever this pattern is used, as hinted by usage of this \ref event_t
    /// typedef, the associated eventfds should be set to under `lazy_lock`
    /// protection, and they must be reset and recycled along with the
    /// associated `status_sync.latched_epoll` epollfd at the time where the
    /// associated future is liberated.
    //
    // TODO: Find the Windows equivalent of this pattern. Since windows does not
    //       have epoll, the simplest option might be to make all futures eager
    //       and use the Win32 thread pool to await dependencies + use an output
    //       event object or WakeByAddress to signal dependents.
    typedef event_t epoll_latch_event_t;

#else
    #error "This header should only be used on Linux."
#endif  // __linux__