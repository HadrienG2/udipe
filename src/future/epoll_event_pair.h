#pragma once

//! \file
//! \brief Coupled epollfd+eventfd pairs
//!
//! As explained in the documentation of \ref epoll_latch_event_t, we often need
//! to work with coupled epollfd+eventfd pairs. This code module provides some
//! generic utilities for doing that.

#ifdef __linux__

    #include <udipe/nodiscard.h>

    #include "epoll_latch_event.h"

    #include "../event.h"
    #include "../fd.h"


    /// Coupled epollfd+eventfd pair
    ///
    /// See member documentation for more information.
    typedef struct epoll_event_pair_s {
        /// epollfd that is attached to `event` with an `epoll_data` of
        /// `UINT64_MAX`, at the exclusion of any other fd.
        fd_t epoll;

        /// eventfd in an unsignaled state
        ///
        epoll_latch_event_t event;
    } epoll_event_pair_t;

    /// Allocate a fresh epollfd+eventfd pair
    ///
    /// In some cases, using a \ref epoll_event_cache_t may lead better performance.
    ///
    /// \returns an epollfd+eventfd pair that should later be liberated into an
    ///          \ref epoll_event_cache_t.
    UDIPE_NODISCARD
    epoll_event_pair_t epoll_event_pair_initialize();

#else
    #error "This header should only be used on Linux."
#endif  // __linux__
