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
    //! Benchmark duration data must be analyzed with care because this data
    //! violates the design assumptions of many common statistical analysis
    //! procedures. For example...
    //!
    //! - Finite timer resolution causes quantization error, which behaves very
    //!   differently from the normally distributed random error model that many
    //!   statistical models assume.
    //! - Duration data frequently exhibits a multi-modal distribution, which in
    //!   part trivially emerges from the aforementioned timer quantization but
    //!   there may also be "higher order modes" originating from other
    //!   phenomena including CPU frequency scaling, caches that may be hit or
    //!   missed, etc. This is in contrast with how many common statistical
    //!   analysis procedures assume a unimodal (typically normal) distribution
    //!   in one way or another.
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
    //! thankfully a luxury that we can often afford in software performance
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
    ///
    /// 95% is chosen because it is the de facto standard in statistics.
    #define CONFIDENCE ((double)0.95)
    static_assert(CONFIDENCE > 0.0 && CONFIDENCE < 1.0);

    /// Fraction of data points that dispersion quantiles surround
    ///
    /// Although they are both set to 95% at the time of writing, confidence
    /// intervals and dispersion quantiles should not be confused as they
    /// measure different things:
    ///
    /// - Confidence intervals apply to a certain population parameter, which is
    ///   presumed to be fixed, and indicate how much our estimate of this
    ///   parameter would likely vary due to random error if we were to run the
    ///   benchmark again while measuring the same thing in an identical system
    ///   configuration.
    ///     * For example, the confidence interval on the mean of a duration
    ///       indicates how much the observed mean duration is likely to vary
    ///       from one benchmark run to another due to observed random error
    ///       alone. A larger change in mean duration is likely to originate
    ///       from a change in benchmark workload or system configuration.
    /// - Dispersion quantiles are selected population quantiles that are used
    ///   to assess the dispersion i.e. the width of probability distribution.
    ///   It indicates how much observed timings vary around the mean. A
    ///   dispersion that is large with respect to the mean value indicates that
    ///   either there is a lot of noise or the mean is not representative for
    ///   another reason like a multimodal distribution. It should be understood
    ///   as an invitation to visualize the full dataset and not be satisfied by
    ///   the summary provided by the mean alone.
    ///
    /// While the width of confidence intervals is linked to the population
    /// dispersion, in the sense that a larger dispersion will lead to wider
    /// confidence intervals, it can be shown that for any probability law,
    /// measuring a larger number of data points will shrink confidence
    /// intervals whereas the dispersion will converge to a nonzero limit.
    #define DISPERSION_WIDTH ((double)0.95)

    /// Number of resamples required for confidence intervals to converge
    ///
    /// The value 201 seems appropriate for two reasons:
    ///
    /// - Bootstrap resampling literature frequently states that around 100
    ///   samples should be enough for standard error estimates.
    /// - When computing a 95% symmetrical confidence interval, it is best if
    ///   percentiles P2.5 and P97.5 fall nearly exactly on a certain value of
    ///   the resampled statistic list, as opposed to being rounded by a large
    ///   margin. This is trivially ensured with 201 resamples, where the
    ///   spacing between resamples corresponds to a quantile spacing of 0.5%.
    ///
    /// Nonetheless, this number of resamples should be increased, and the
    /// rationale comment updated accordingly, if unstable or blatantly
    /// incorrect confidence intervals are observed in a manner that is not
    /// resolved by simply collecting more data points per benchmark.
    #define NUM_RESAMPLES ((size_t)201)

    /// \}


    /// \name Type definitions
    /// \{

    /// Estimate of some population statistic
    ///
    /// Using doubles even for integer quantities like deciles is fine because
    /// we do not expect to encounter run durations larger than 2^54 ns (about 7
    /// months!) and below that the int64-to-double conversion is lossless.
    typedef struct estimate_s {
        /// Central tendency for the statistic of interest
        ///
        /// This is the median value of the statistic over all bootstrap runs.
        double center;

        /// Lower centered confidence bound of the statistic of interest
        ///
        /// This is the `(1 - CONFIDENCE) / 2` quantile of the statistic over
        /// all bootstrap runs.
        double low;

        /// Higher centered confidence bound of the statistic of interest
        ///
        /// This is the `(1 + CONFIDENCE) / 2` quantile of the statistic over
        /// all bootstrap runs.
        double high;
    } estimate_t;

    /// Set of statistical estimates used to describe timing distributions
    ///
    /// These statistics are currently chosen based on the needs of the clock
    /// calibration procedure and can rather easily be extended to accomodate
    /// new needs.
    ///
    /// Population quantiles are estimated through bootstram resampling rather
    /// than deduced from the standard deviation because the latter procedure
    /// implicitly relies on assuming a certain underlying probability law
    /// (typically the normal law) which is not even approximately followed by
    /// many real-world benchmark datasets.
    typedef struct statistics_s {
        /// Estimated population mean
        ///
        /// Technically a truncated mean since it is computed over a dataset
        /// from which outliers have been removed by \ref outlier_filter_t.
        ///
        /// The reason why are using a truncated mean rather than the median,
        /// even though it requires outlier filtering to achieve good outlier
        /// resilience, is that in the presence of clock quantization, the
        /// median's "boundary effects" turn out to particularly problematic.
        /// They can lead to values that are mostly very stable, yet can vary
        /// dramatically from time to time, and this can way too easily be
        /// misinterpreted as changes of the underlying benchmark load by users.
        estimate_t mean;

        /// Estimated `(1 - DISPERSION_WIDTH) / 2` population quantile
        ///
        /// Assuming a \ref DISPERSION_WIDTH of 95% for clarity, the interval
        /// `[sym_dispersion_start; sym_dispersion_end]` surrounds 95% of data
        /// points by setting aside the lowest and highest 2.5% of the dataset.
        ///
        /// In other words, it measures the spread of the dataset around its
        /// median value in a manner that is less outlier-sensitive than a pure
        /// `[min; max]` interval would, at the expense of ignoring some data.
        ///
        /// You can use the `[sym_dispersion_start; sym_dispersion_end]` as a
        /// rough indicator of where most of your data points lie.
        estimate_t sym_dispersion_start;

        /// Estimated `1 - DISPERSION_WIDTH` population quantile
        ///
        /// Assuming a \ref DISPERSION_WIDTH of 95% for clarity, the interval
        /// `[low_dispersion_bound; +inf[` surrounds 95% of data points by
        /// setting aside the lowest 5% of the dataset.
        ///
        /// This dispersion interval is useful when you want to detect when e.g.
        /// measured timings have risen above a certain constant threshold, such
        /// as a user-specified minimal duration.
        ///
        /// If the threshold is not fixed but determined via another
        /// measurement, then this quantile cannot be used and you must instead
        /// study the distribution of `measurement - threshold` differences. A
        /// typical statistical test will for example ensure that 95% of these
        /// differences are above 0.0.
        estimate_t low_dispersion_bound;

        /// Estimated \ref DISPERSION_WIDTH population quantile
        ///
        /// This is the "high" counterpart of `low_dispersion_bound`. Assuming
        /// our usual 95% \ref DISPERSION_WIDTH, the interval `]-inf;
        /// high_dispersion_bound]` surrounds 95% of data points by setting
        /// aside the highest 5% of the dataset.
        estimate_t high_dispersion_bound;

        /// Estimated `(1 + DISPERSION_WIDTH) / 2` population quantile
        ///
        /// This is the "high" counterpart of `sym_dispersion_start`.  Assuming
        /// our usual 95% \ref DISPERSION_WIDTH, the interval
        /// `[sym_dispersion_start; sym_dispersion_end]` surrounds 95% of data
        /// points by setting aside the lowest and highest 2.5% of the dataset.
        estimate_t sym_dispersion_end;

        /// Estimated width of `[sym_dispersion_start; sym_dispersion_end]`
        ///
        /// If you want to know the spread of data points, as when computing
        /// signal-to-noise ratio, then this statistic is more reliable than
        /// computing the difference `sym_dispersion_end - sym_dispersion_start`
        /// and guesstimating the width of the confidence interval as it will
        /// correctly account for correlations between the two bounds caused by
        /// e.g. CPU frequency scaling.
        estimate_t sym_dispersion_width;

        // To add another statistic here, you need an associated entry to
        // statistic_id_e and appropriate code in analyzer_apply().
    } statistics_t;

    /// Identifier for statistics within \ref statistics_t::statistics
    ///
    /// This enum has one entry per \ref estimate_t in \ref statistics_t and is
    /// used to locate the appropriate data sub-array inside of \ref analyzer_t.
    typedef enum statistic_id_e {
        MEAN = 0,
        SYM_DISPERSION_START,
        LOW_DISPERSION_BOUND,
        HIGH_DISPERSION_BOUND,
        SYM_DISPERSION_END,
        SYM_DISPERSION_WIDTH,
        NUM_STATISTICS  // Must remain the last entry
    } statistic_id_t;

    /// Statistical analyzer
    ///
    /// This struct contains the long-lived state needed to compute the \ref
    /// statistics_t associated with a certain \ref distribution_t.
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
        /// During resampling, statistics values are collected into this array,
        /// then at the end they are sorted and quantiles are extracted to build
        /// the confidence intervals of the output \ref estimate_t values.
        double statistics[NUM_STATISTICS][NUM_RESAMPLES];
    } analyzer_t;

    /// \}


    /// \name Public \ref analyzer_t API
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
    ///                 with analyzer_initialize() and has not been destroyed
    ///                 with analyzer_finalize() yet.
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't yet been recycled via
    ///             distribution_reset() or destroyed via
    ///             distribution_finalize().
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
    ///                 with analyzer_initialize() and has not been destroyed
    ///                 with analyzer_finalize() yet.
    UDIPE_NON_NULL_ARGS
    void analyzer_finalize(analyzer_t* analyzer);

    /// \}


    /// Estimate a mean iteration duration from a mean iteration batch duration
    ///
    /// Because iteration durations are not observable, we need to make some
    /// assumptions about the benchmark's probabilistic behavior in order to be
    /// able to estimate them by statistical inference means. Our assumptions
    /// are that:
    ///
    /// - The provided iteration batch duration solely represents the duration
    ///   of benchmark iterations, excluding any affine setup and teardown
    ///   overhead.
    /// - Within the batch of interest, iterations are independent from each
    ///   other and identically distributed. This is typically only achieved for
    ///   "central" iterations of a sufficiently long-running benchmark, as the
    ///   first and last few iterations tend to be slower than other iterations
    ///   due to CPU pipelining effects.
    /// - Iteration confidence intervals can be estimated from run confidence
    ///   intervals through linear scaling by the ratio of standard deviations.
    ///
    /// The first two assumptions typically require that this analysis be
    /// performed on a difference of large run durations, rather than a raw run
    /// duration. Indeed, any benchmark run has some nontrivial setup and
    /// teardown overhead and some slower iterations at the start and the end.
    /// But for a sufficiently long-running benchmark, the difference of
    /// durations between a run with N + M iterations and a run with N
    /// iterations will average to M times the duration of a central, maximally
    /// reproducible loop iteration.
    ///
    /// \param sum_mean is an estimate of the mean duration of `num_iterations`
    ///                 benchmark loop iterations.
    /// \param num_iterations is the number of iterations that are timed by
    ///                       `sum_means`.
    ///
    /// \returns an estimate of the duration of one benchmark loop iteration.
    static inline
    estimate_t estimate_iteration_duration(estimate_t sum_mean,
                                           size_t num_iterations) {
        estimate_t iter_mean;
        // Per linearity hypothesis, run duration = sum(iter duration)
        // From this, i.i.d. hypothesis gives us linear mean & variance scaling
        iter_mean.center = sum_mean.center / num_iterations;
        // Given linear variance scaling, we trivially deduce that stddev scales
        // as the square root of the number of iterations...
        const double stddev_norm = 1.0 / sqrt(num_iterations);
        // ...which, per the assumed confidence interval scaling law, gives us
        // the iteration duration confidence interval.
        iter_mean.low = iter_mean.center
                      - (sum_mean.center - sum_mean.low) * stddev_norm;
        iter_mean.high = iter_mean.center
                       + (sum_mean.high - sum_mean.center) * stddev_norm;
        return iter_mean;
    }


    /// \name Implementation details
    /// \{

    /// Compute the mean of a distribution
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param analyzer must be an \ref analyzer_t that has been initialized
    ///                 with analyzer_initialize() and has not been destroyed
    ///                 with analyzer_finalize() yet.
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't yet been recycled via
    ///             distribution_reset() or destroyed via
    ///             distribution_finalize().
    ///
    /// \returns the mean value of `dist`
    UDIPE_NON_NULL_ARGS
    double analyze_mean(analyzer_t* analyzer_t,
                        const distribution_t* dist);

    /// Estimate the value of a statistic based on bootstrap data
    ///
    /// This function must be run after the `analyzer::statistics` array has
    /// been filled up with data from bootstrap resampling.
    ///
    /// \param analyzer must be an \ref analyzer_t that has been initialized
    ///                 with analyzer_initialize() and has not been destroyed
    ///                 with analyzer_finalize() yet.
    /// \param stat is a \ref statistic_id_t that indicates which population
    ///             statistic should be estimated.
    ///
    /// \returns an estimate of the population statistic with a confidence
    ///          interval.
    UDIPE_NON_NULL_ARGS
    estimate_t analyze_estimate(analyzer_t* analyzer,
                                statistic_id_t stat);

    /// \}


    // TODO: Add unit tests

#endif  // UDIPE_BUILD_BENCHMARKS