#ifdef UDIPE_BUILD_BENCHMARKS

    #pragma once

    //! \file
    //! \internal
    //! \brief Microbenchmarks
    //!
    //! This header contains the microbenchmarks of `libudipe`. It is an
    //! internal implementation detail that you should not use directly.

    #include "visibility.h"


    /// \internal
    /// \brief Run all the libudipe micro-benchmarks
    UDIPE_PUBLIC void udipe_micro_benchmarks(int argc, char *argv[]);

#endif  // UDIPE_BUILD_BENCHMARKS
