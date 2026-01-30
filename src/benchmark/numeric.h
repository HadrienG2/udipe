#ifdef UDIPE_BUILD_BENCHMARKS

    #pragma once

    //! \file
    //! \brief Numerical analysis tools
    //!
    //! There are a few basic operations in floating-point math that require
    //! some care if you don't want to experience a massive precision
    //! degradation on larger datasets. This code module provides such
    //! operations.

    #include <udipe/pointer.h>

    #include <stddef.h>


    /// Compute the sum of numbers in `array` of length `len` in place
    ///
    /// The summation algorithm takes precautions to minimize accumulation and
    /// cancelation error, while trying to remain reasonably fast on large
    /// arrays. It assumes finite inputs and may therefore produce unexpected
    /// results if the dataset contains infinities or NaNs.
    ///
    /// To avoid overflow, the elements of `arrays` should preferably be
    /// normalized such that the maximum value is around 1.0. But since `double`
    /// has a huge exponent range (up to 2^1023), we can tolerate "reasonably"
    /// unnormalized values for short enough sums.
    ///
    /// \param array must point to an array of at least `length` numbers, which
    ///              will be modified during the summation process.
    /// \param length indicates how many elements of `array` must be summed.
    ///
    /// \returns the sum of the elements of `array`.
    UDIPE_NON_NULL_ARGS
    double sum_f64(double array[], size_t length);


    #ifdef UDIPE_BUILD_TESTS
        /// Unit tests
        ///
        /// This function runs all the unit tests for this module. It must be called
        /// within the scope of with_logger().
        void numeric_unit_tests();
    #endif

#endif  // UDIPE_BUILD_BENCHMARKS