#ifdef UDIPE_BUILD_TESTS

    #pragma once

    //! \file
    //! \brief Unit testing utilities
    //!
    //! This supplements the "public" interface defined inside of
    //! `udipe/unit_tests.h` with private utilities that can only be used within
    //! the libudipe codebase and unit tests thereof.

    #include <udipe/unit_tests.h>


    /// Configure the libc random number generator so that stochastic seeds are
    /// used by default and deterministic seeds are enforced on demand.
    ///
    /// Every unit tests that uses random numbers starts by calling this.
    void configure_rand();

#endif  // UDIPE_BUILD_TESTS