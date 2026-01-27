#ifdef UDIPE_BUILD_BENCHMARKS

    #pragma once

    //! \file
    //! \brief Statistical analysis of \ref distribution_t
    //!
    //! This module provides tools to perform various statistical computations
    //! over \ref distribution_t datasets that are composed of raw benchmark
    //! execution durations or quantities derived from such durations (e.g.
    //! differences of durations).
    //!
    //! Benchmark duration data must be analyzed with care because it violates
    //! the design assumptions of many common statistical analysis procedures.
    //! For example...
    //!
    //! - Finite timer resolution causes quantization error, which behaves very
    //!   differently from the normally distributed random error model that many
    //!   statistical models assume.
    //! - Duration data frequently exhibits a multi-modal distribution, emerging
    //!   from timer quantization but also many other phenomena including CPU
    //!   frequency scaling, caches that may be hit or missed, etc. This is in
    //!   contrast with how many common statistical analysis procedures assume a
    //!   unimodal (typically normal) distribution in one way or another.
    //! - Duration data tends to be right-skewed by occasional outliers, such as
    //!   OS scheduler interrupts, which can have a very different magnitude
    //!   from normal data point and have a behavior that depends on system
    //!   characteristics and non-reproducible background load. If aggregated
    //!   into outlier-sensitive metrics like the arithmetic mean and standard
    //!   deviation, these outliers will be a source of system-dependent
    //!   variance and bias.
    //!
    //! We handle these data peculiarities using a combination of nonparametric
    //! statistical analysis techniques which avoid assumptions of normally
    //! distributed data, the most prominent of which is bootstrap resampling.
    //!
    //! Bootstrap resampling works under the assumption that we have collected
    //! enough data points for the shape of the empirical data distribution to
    //! closely match that of the underlying probability distribution, which is
    //! thankfully a luxury that we can afford in software performance
    //! benchmarking. Under this assumption, if we denote N the number of data
    //! points inside of the sample distribution, it can be proven that the
    //! outcome of randomly sampling N points from the sample distribution with
    //! replacement is going to be close to yield a dataset close to that which
    //! we would have measured by performing N more benchmark runs.
    //!
    //! Once we are in this regime, we can leverage the aforementioned property
    //! to compute confidence intervals for any statistic of interest without
    //! making incorrect assumptions of data being normally distributed, by
    //! simply resampling the sample distribution a sufficiently high number of
    //! times, computing the statistic of interest over the each resampled
    //! distribution, and estimating the confidence interval as the
    //! corresponding quantiles of the distribution of resulting statistics.

    #include <udipe/pointer.h>

    #include "distribution.h"

    #include <assert.h>
    #include <stddef.h>
    #include <stdint.h>


    /// \name Tunable parameters
    /// \{

    /// Width of confidence intervals
    ///
    /// This should be set between 0.0 and 1.0 exclusive. Wider confidence
    /// intervals will likely require a larger \ref NUM_RESAMPLES to converge.
    #define CONFIDENCE ((double)0.95)
    static_assert(CONFIDENCE > 0.0 && CONFIDENCE < 1.0);

    /// Number of resamples required for confidence intervals to converge
    ///
    /// The value 200 is chosen based on common advice from bootstrap resampling
    /// literature regarding the use of bootstrap resampling to estimate
    /// confidence intervals, where people are most concerned with confidence
    /// intervals of standard error. It should be tuned up as needed if one
    /// observes unstable or incorrect confidence intervals in a manner that is
    /// not resolved by simply collecting more data points per benchmark.
    #define NUM_RESAMPLES ((size_t)200)

    /// \}


    /// \name Type definitions
    /// \{

    /// Estimate of some population statistic
    ///
    /// Using doubles even for integer quantities like deciles is fine because
    /// we do not expect to encounter run durations larger than 2^54 ns (about 7
    /// months!) and below that the int64-to-double conversion is lossless.
    //
    // TODO docs
    typedef struct estimate_s {
        /// Central tendency for the statistic of interest
        ///
        /// This is the median value of the statistic over all bootstrap runs.
        double center;

        /// Lower confidence bound of the statistic of interest
        ///
        /// This is the `(1 - CONFIDENCE)/2` quantile of the statistic over all
        /// bootstrap runs.
        double low;

        /// Higher confidence bound of the statistic of interest
        ///
        /// This is the `(1 + CONFIDENCE)/2` quantile of the statistic over all
        /// bootstrap runs.
        double high;
    } estimate_t;

    /// Set of statistical estimates used to describe timing distributions
    //
    // TODO docs
    typedef struct statistics_s {
        /// Estimated population mean
        ///
        /// Technically a truncated mean since it is computed over a dataset
        /// from which outliers have been removed by \ref outlier_filter_t.
        estimate_t mean;

        /// Estimated 5% population quantile
        ///
        /// Not computed via standard error as that relies on a normality
        /// assumption that may not be accurate.
        estimate_t p5;

        /// Estimated 95% population quantile
        ///
        /// Not computed via standard error as that relies on a normality
        /// assumption that may not be accurate.
        estimate_t p95;

        /// p95 - p5 difference
        ///
        /// Not redundant with p5 and p95 because the values of p5 and p95 may
        /// be correlated in such a way that the confidence interval of the
        /// difference is not equal to the difference of confidence intervals.
        estimate_t p5_to_p95;
    } statistics_t;

    /// Identifier for statistics within \ref statistics_t::statistics
    ///
    /// This enum has one entry per \ref estimate_t in \ref statistics_t and is
    /// used to locate the appropriate data sub-array inside of \ref analyzer_t.
    typedef enum statistic_id_e {
        MEAN = 0,
        P5,
        P95,
        P5_TO_P95,
        NUM_STATISTICS  // Must be the last entry
    } statistic_id_t;

    /// Statistical analyzer
    //
    // TODO docs
    typedef struct analyzer_s {
        /// Distribution builder used for resampling
        ///
        /// Reset for reuse at the end of each resampling cycle.
        distribution_builder_t resample_builder;

        /// Accumulators used when computing the mean value of a distribution
        ///
        /// During the mean computation, this buffer is first used to store the
        /// contribution of each \ref distribution_t to the mean, then to
        /// accumulate those contributions into a mean in a fashion that reduces
        /// floating-point rounding error.
        ///
        /// The underlying buffer's capacity is given by `mean_capacity`.
        double* mean_accumulators;

        /// Storage capacity of `mean_accumulators`
        ///
        /// We need one accumulator per bin of the \ref distribution_t that is
        /// being analyzed, reallocating `mean_accumulators` as necessary.
        size_t mean_capacity;

        /// Bootstrapped values of a statistic
        ///
        /// During resampling, statistical values are collected into this array,
        /// then at the end they are sorted and quantiles are extracted to build
        /// the output \ref estimate_t.
        double statistics[NUM_STATISTICS][NUM_RESAMPLES];
    } analyzer_t;

    /// \}


    /// \name Public API
    /// \{

    /// Set up a statistical analyzer
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \returns an \ref analyzer_t that can be used to analyze measurements
    ///          with analyzer_apply().
    analyzer_t analyzer_initialize();

    /// Perform statistical analysis of `dist`
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param analyzer must be an \ref analyzer_t that has been initialized
    ///        with analyzer_initialize() and has not been destroyed with
    ///        analyzer_finalize() yet.
    UDIPE_NON_NULL_ARGS
    statistics_t analyzer_apply(analyzer_t* analyzer,
                                const distribution_t* dist);

    /// Destroy a statistical analyzer
    ///
    /// `analyzer` must not be used again after calling this function.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param analyzer must be an \ref analyzer_t that has been initialized
    ///        with analyzer_initialize() and has not been destroyed with
    ///        analyzer_finalize() yet.
    UDIPE_NON_NULL_ARGS
    void analyzer_finalize(analyzer_t* analyzer);

    /// \}


    /// \name Implementation details
    /// \{

    /// Compute the mean of a distribution
    // TODO docs
    UDIPE_NON_NULL_ARGS
    double analyze_mean(analyzer_t* analyzer_t,
                        const distribution_t* dist);

    /// Estimate the value of a statistic based on bootstrap data
    ///
    /// This function must be run after the `analyzer::statistics` array has
    /// been filled up with bootstrap data.
    // TODO docs
    UDIPE_NON_NULL_ARGS
    estimate_t analyze_estimate(analyzer_t* analyzer,
                                statistic_id_t stat);

    /// \}


    // TODO: Add unit tests

#endif  // UDIPE_BUILD_BENCHMARKS