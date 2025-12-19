#ifdef UDIPE_BUILD_TESTS

    #pragma once

    //! \file
    //! \brief Unit tests
    //!
    //! This header contains the unit tests of `libudipe`. It is an
    //! implementation detail of the tests/unit_tests.c binary that you should
    //! not use directly. Instead, please run the unit_tests binary itself, or
    //! use ctest which will run it along with all testable examples.

    #include "visibility.h"


    /// Run all the libudipe unit tests
    ///
    /// This is an implementation detail of the tests/unit_tests.c binary.
    /// Please run this binary directly or via ctest instead of calling this
    /// internal function whose API may change without warnings.
    UDIPE_PUBLIC void udipe_unit_tests(int argc, char *argv[]);

#endif  // UDIPE_BUILD_TESTS
