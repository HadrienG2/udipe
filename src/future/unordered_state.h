#pragma once

//! \file
//! \brief Future state that is specific to \ref TYPE_UNORDERED

#include <udipe/result.h>

#include "collective_upstream.h"

#ifdef __linux__
    #include "inpoll_latch_event.h"
    #include "inner_fd.h"
#endif


/// Unordered future state and result
///
/// This is the \ref udipe_future_t::specific variant used for \ref
/// TYPE_UNORDERED. It tracks the state needed to wait for at least one of the
/// specified upstream futures to reach its final outcome. And when this
/// happens, it makes it possible to report which future got ready and how to
/// await subsequent futures (if any).
typedef struct future_unordered_state_s {
    /// Set of upstream futures awaited by this collective future
    ///
    collective_upstream_t upstream;

    /// Result of the asynchronous operation
    ///
    /// This result is set before signaling \ref OUTCOME_SUCCESS. It indicates
    /// which of the upstream futures became ready and how to await the rest of
    /// the upstream futures.
    ///
    /// Must be written under `lazy_lock` protection. Inner future (if any) must
    /// not be recycled on udipe_finish(), as it will be fed to the caller which
    /// is responsible for liberating it.
    udipe_unordered_payload_t payload;

    #ifdef __linux__
        /// Inner inpoll that monitors the upstream `status_sync` fds
        ///
        /// This inner fd is attached to the \ref status_sync_t::latched_inpoll
        /// of the host future. See \ref inner_fd_t for more information about
        /// this cascading file descriptor pattern.
        ///
        /// It must be awaited under `lazy_lock` protection, then eventually
        /// detached from the `latched_inpoll` of the original future and
        /// attached to the `latched_inpoll` of the successor future (if any)
        /// once a result is ready.
        ///
        /// It must be destroyed when the last future in the unordered chain is
        /// liberated. There seems to be little point in trying to recycle the
        /// inpolls of unordered futures because setting up a collective future
        /// requires an arbitrarily large amount of inpoll_detach() operations,
        /// so it's not expected that inpoll allocation/liberation will often be
        /// the bottleneck.
        //
        // TODO: Prove the above assertion through benchmarking and profiling of
        //       real-world workloads.
        // TODO: Find an epoll replacement for Windows. Will likely be based on
        //       the Win32 thread pool driving an eager future.
        inner_fd_t upstream_inpoll;

        /// Event object used to keep `latched_inpoll` perma-ready after the
        /// future has reached its final state.
        ///
        /// See \ref inpoll_latch_event_t for more information.
        inpoll_latch_event_t inpoll_latch;
    #endif
} future_unordered_state_t;
