#pragma once

//! \file
//! \brief Mechanism to keep an inpoll signaled indefinitely

#ifdef __linux__

    #include "../event.h"


    /// eventfd used to keep an \ref inpoll_t signaled indefinitely
    ///
    /// At the time of writing, the Linux implementation of futures uses \ref
    /// inpoll_t in two different ways:
    ///
    /// - Collective joined and unordered futures use \ref inpoll_t as a way to
    ///   repeatedly await upstream futures. In this role, compared to select(),
    ///   poll() or futex_waitv(), \ref inpoll_t is more CPU-efficient due to
    ///   its stateful nature, which lets the set of monitored objects to be
    ///   remembered across inpoll_wait() calls. And compared to io_uring, \ref
    ///   inpoll_t is a lot easier to understand, more compatible with older
    ///   kernels, and also more memory-efficient which matters for this
    ///   unusally fine-grained use case.
    /// - Unordered and repeated timer futures, which emit a chain of successor
    ///   futures, use \ref inpoll_t as an indirection layer. \ref inpoll_t
    ///   indirection makes it possible to forward readiness notifications from
    ///   a hidden file descriptor for a while, then eventually "transfer" a
    ///   hidden file descriptor to the next future in the chain, without
    ///   changing the `status_sync` file descriptor number of the active future
    ///   in the process or the readiness of said file descriptor.
    ///
    /// The most straightforward and efficient way to use \ref inpoll_t like
    /// this while allowing futures to emit fd-based notifications, is to
    /// directly designate the output \ref inpoll_t as the `status_sync` fd of
    /// the host future. But this requires a way to set said fd to a permanently
    /// ready state after the future has reached its final state, thus allowing
    /// all fd-based clients to reliably notice that this future has reached
    /// this stage.
    ///
    /// We do this by attaching an extra eventfd to the `status_sync` \ref
    /// inpoll_t in addition to all other file descriptors that are being
    /// monitored. This eventfd is signaled once the future reaches its final
    /// state and never reset until the future is liberated.
    ///
    /// Whenever this pattern is used, as hinted by usage of this \ref event_t
    /// typedef, the associated eventfds should be set to under `lazy_lock`
    /// protection, and they must be reset and recycled along with the
    /// associated `status_sync.latched_inpoll` \ref inpoll_t at the time where
    /// the associated future is liberated.
    //
    // TODO: Find the Windows equivalent of this pattern. Since windows does not
    //       have inpoll, the simplest option might be to make all futures eager
    //       and use the Win32 thread pool to await dependencies + use an output
    //       event object and/or WakeByAddress to signal dependents.
    typedef event_t inpoll_latch_event_t;

#else
    #error "This header should only be used on Linux."
#endif  // __linux__