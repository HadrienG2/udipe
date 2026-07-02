#pragma once

//! \file
//! \brief \ref inpoll_t with an associated \ref inpoll_latch_event_t
//!
//! As explained in the documentation of \ref inpoll_latch_event_t, we often
//! need to work with coupled inpoll+eventfd pairs. This code module provides
//! a couple of utilities for doing that.

#ifdef __linux__

    #include <udipe/nodiscard.h>

    #include "inpoll_latch_event.h"

    #include "../event.h"
    #include "../inpoll.h"

    #include <stdint.h>


    /// Identifer of the `latch` of an \ref inpoll_with_latch_t
    ///
    /// This will always be `UINT64_MAX` to keep all realistic upstream future
    /// indices available, but the dedicated constant clarifies things a bit.
    #define INPOLL_LATCH_ID UINT64_MAX

    /// Coupled inpoll+eventfd pair
    ///
    /// This is the output of latched_inpoll_initialize(), see the member
    /// documentation for more information about its initial state.
    typedef struct inpoll_with_latch_s {
        /// \ref inpoll_t that comes pre-attached to `latch`
        ///
        /// `inpoll` is initially attached to `latch` with identifier \ref
        /// INPOLL_LATCH_ID. It is not initially attached to any other fd.
        inpoll_t inpoll;

        /// eventfd that is initially unsignaled and attached to `inpoll`
        ///
        /// `latch` comes pre-attached to `inpoll`, see associated
        /// documentation. It is provided in an unsignaled state.
        inpoll_latch_event_t latch;
    } inpoll_with_latch_t;

    /// Allocate a coupled inpoll+eventfd pair
    ///
    /// The initial state of the output \ref inpoll_with_latch_t is described in
    /// the documentation of this type.
    ///
    /// As this process requires multiple syscalls, using an \ref
    /// latched_inpoll_cache_t is recommended for performance.
    ///
    /// \returns a coupled inpoll+eventfd pair that should later be liberated
    ///          into an \ref latched_inpoll_cache_t.
    UDIPE_NODISCARD
    inpoll_with_latch_t latched_inpoll_initialize();

#else
    #error "This header should only be used on Linux."
#endif  // __linux__
