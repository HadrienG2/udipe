#pragma once

//! \file
//! \brief Coupled inpoll+eventfd pairs
//!
//! As explained in the documentation of \ref inpoll_latch_event_t, we often
//! need to work with coupled inpoll+eventfd pairs. This code module provides
//! some generic utilities for doing that.

#ifdef __linux__

    #include <udipe/nodiscard.h>

    #include "inpoll_latch_event.h"

    #include "../event.h"
    #include "../inpoll.h"


    /// Coupled inpoll+eventfd pair
    ///
    /// See member documentation for more information.
    typedef struct inpoll_event_pair_s {
        /// \ref inpoll_t that is attached to `event` with an `identifier` of
        /// `UINT64_MAX`, at the exclusion of any other fd.
        inpoll_t inpoll;

        /// eventfd in an unsignaled state
        ///
        inpoll_latch_event_t event;
    } inpoll_event_pair_t;

    /// Allocate a fresh inpoll+eventfd pair
    ///
    /// In some cases, using a \ref inpoll_event_cache_t may lead better
    /// performance.
    ///
    /// \returns an inpoll+eventfd pair that should later be liberated into an
    ///          \ref inpoll_event_cache_t.
    UDIPE_NODISCARD
    inpoll_event_pair_t inpoll_event_pair_initialize();

#else
    #error "This header should only be used on Linux."
#endif  // __linux__
