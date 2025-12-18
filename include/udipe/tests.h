#ifdef UDIPE_BUILD_TESTS

    #pragma once

    //! \file
    //! \internal
    //! \brief Unit tests
    //!
    //! This header contains the unit tests of `libudipe`. It is an internal
    //! implementation detail of the libudipe unit tests that you should not use.

    #include "visibility.h"


    /// \internal
    /// \brief Run all the libudipe unit tests
    UDIPE_PUBLIC void udipe_unit_tests(int argc, char *argv[]);

#endif  // UDIPE_BUILD_TESTS
