#pragma once

//! \file
//! \brief Simplified epollfd interface for read readiness multiplexing
//!
//! The Linux epollfd interface can do many things. But in the implementation of
//! udipe futures, we use it for a very specific (and somewhat unusual) purpose:
//!
//! - We are only interested in the read readiness signal (`EPOLLIN`).
//! - We only connect an epollfd to special fd types that do not represent true
//!   I/O devices (at the time of writing eventfds, timerfds and other
//!   epollfds). The read readiness of these fd types signals some kind of
//!   system/application event rather than actual data availability.
//! - We only use epollfds for the purposes of...
//!     * Getting a single fd that is ready when any of a set of "upstream" fds
//!       is ready, which is key to efficient chaining of collective futures
//!       like join and unordered as a dependency to other futures.
//!     * "Abstracting out" the true fd behind a future's output fd, so that
//!       said fd can later be rebound to a different future and reconfigured
//!       without changing the initial future's output fd status.
//!
//! In other words, we use epollfds as an "any-of" fd operator that is ready to
//! be read when any upstream fd is ready to be read, where the "ready to be
//! read" signal does not represent actual data availability but rather some
//! other system- or user-controlled event.
//!
//! And this peculiar use calls for a simplified interface that highlights the
//! true nature of what we are trying to do.

#ifdef __linux__

// TODO: Add simplified interface. Steal code from epoll_event_pair, steal docs
//       from the file-level description, and simplify wait.c using the
//       resulting primitives.

#else
    #error "This header should only be used on Linux."
#endif  // __linux__