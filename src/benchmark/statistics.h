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
    #include <math.h>
    #include <stddef.h>
    #include <stdint.h>
    #include <stdio.h>


    /// \name Tunable parameters
    /// \{

    /// Width of confidence intervals
    ///
    /// This should be set between 0.0 and 1.0 exclusive.
    ///
    /// 95% is used at the time of writing because it is the de facto standard
    /// in statistics. Higher values will lead to a lower probability of
    /// estimates randomly falling outside of the confidence interval by chance,
    /// at the expense of worse convergence that will likely require more data
    /// points and/or larger values of \ref NUM_RESAMPLES and thus
    /// longer-running benchmarks.
    #define CONFIDENCE ((double)0.95)
    static_assert(CONFIDENCE > 0.0 && CONFIDENCE < 1.0);

    /// Fraction of data points that are excluded by the quantiles used in
    /// dispersion analysis
    ///
    /// Although they both select 95% of a certain kind of value at the time of
    /// writing, confidence intervals and dispersion quantiles should not be
    /// confused as they measure two very different things:
    ///
    /// - Confidence intervals apply to estimates of a certain parameter of the
    ///   population probability distribution, which is presumed to remain fixed
    ///   across benchmark runs. They indicate how much our estimate of this
    ///   parameter would likely vary due to random error if we were to execute
    ///   the benchmark again while measuring the same workload in an identical
    ///   system configuration.
    ///     * For example, the confidence interval on the mean duration of a
    ///       benchmark indicates how much the computed mean duration is likely
    ///       to vary from one benchmark execution to another due to observed
    ///       random error alone. If another measurement yields a mean outside
    ///       of the previous confidence interval, it is most likely to
    ///       originate from a change in the true population distribution mean,
    ///       caused by a significant change of benchmark workload or system
    ///       configuration.
    ///     * Confidence intervals shrink as the amount of available data points
    ///       increase, with their width theoretically scaling as the inverse
    ///       square root of the amount of data points though practical
    ///       considerations like timer quantization and slow variations in the
    ///       system configuration will lead to deviations from this ideal law.
    ///       Therefore, if you encounter overly wide confidence intervals, a
    ///       reliable if not always satisfactory solution is to collect more
    ///       data points per benchmark execution.
    /// - Dispersion quantiles are selected population quantiles that are used
    ///   to assess the dispersion i.e. the width of the probability
    ///   distribution associated with the measured quantity of interest. It
    ///   serves as an indication of how much observed timings vary around the
    ///   mean or another quantity of interest. If the dispersion is large with
    ///   respect to the mean, it suggests that benchmark timings are
    ///   _intrinsically_ variable/non-reproducible in a manner that no extra
    ///   data points will fix.
    ///     * High dispersion warrants further investigation as such a finding
    ///       may either be normal (if measuring fundamentally non-reproducible
    ///       phenomena like download performance from a random internet server)
    ///       or pathological (if a timing that should be highly reproducible
    ///       has abnormal variability due to CPU frequency scaling, background
    ///       system workload, etc). Generally speaking, high-dispersion
    ///       distributions should be studied manually through visualization and
    ///       careful investigation, not by looking at statistical summaries
    ///       alone, which can only highlight dispersion but not explain it.
    ///
    /// There's nothing sacred about 5%, we can in principle use any
    /// distribution quantile to quantify dispersion. However there's a tradeoff
    /// that must be kept in mind when tuning this parameter:
    ///
    /// - Excluding fewer data points, where the limit is to study the
    ///   distribution's min/max value with an excluded fraction of 0.0, makes
    ///   the dispersion measurement more sensitive to outliers and slower to
    ///   converge as the number of data points increases because we become
    ///   sensitive to increasingly small/improbable tails of the probability
    ///   distribution.
    /// - Excluding more data points, as in the standard quartile-based
    ///   5-numbers statistical summary, will lead to more misleading numbers
    ///   that is less representative of the "true" distribution width when the
    ///   probability distribution has a complex shape like e.g. multiple modes.
    #define DISPERSION_EXCLUDED_FRACTION ((double)0.05)

    /// Number of resamples required for confidence intervals to converge
    ///
    /// The value 201 seems appropriate for two reasons:
    ///
    /// - Bootstrap resampling literature frequently states that around 100
    ///   samples should be enough when computing standard error estimates.
    /// - When computing a 95% symmetrical confidence interval, it is best if
    ///   percentiles P2.5 and P97.5 fall nearly exactly on a certain value of
    ///   the resampled statistic list, as opposed to being rounded by a large
    ///   margin. This is trivially ensured with 201 resamples, where the
    ///   spacing between resamples corresponds to a quantile spacing of 0.5%.
    ///
    /// Nonetheless, this number of resamples should be increased, and the above
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
        /// Value of the statistic of interest computed on the raw data sample
        ///
        /// This value is not computed on the bootstrap distribution but
        /// directly on the raw data distribution.
        double sample;

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
    /// Population quantiles are estimated through bootstrap resampling rather
    /// than deduced from the standard deviation because the latter procedure
    /// implicitly relies on assuming a certain underlying probability law
    /// (typically the normal law), which is not even approximately followed by
    /// many real-world benchmark datasets.
    typedef struct statistics_s {
        /// Estimated \ref DISPERSION_CENTER_START population quantile
        ///
        /// Assuming a \ref DISPERSION_EXCLUDED_FRACTION of 5% for clarity, the
        /// interval `[center_start; center_end]` surrounds 95% of data points
        /// by setting aside the lowest and highest 2.5% of the dataset.
        ///
        /// In other words, it measures the spread of the dataset around its
        /// median value in a manner that is less outlier-sensitive than a pure
        /// `[min; max]` interval would, at the expense of ignoring some data.
        ///
        /// You can use the `[center_start; center_end]` interval as an
        /// indicator of where most of your data points lie.
        estimate_t center_start;

        /// Estimated \ref DISPERSION_LOW_TAIL_BOUND population quantile
        ///
        /// Assuming a \ref DISPERSION_EXCLUDED_FRACTION of 5% for clarity, the
        /// interval `[low_tail_bound; +inf[` surrounds 95% of data points by
        /// setting aside the lowest 5% of the dataset.
        ///
        /// This dispersion interval is useful when you want to detect when most
        /// measured values have risen above a certain constant threshold, such
        /// as a user-specified minimal duration for timing measurements.
        ///
        /// If the threshold is not fixed but determined via another
        /// measurement, then this quantile cannot be used directly and you must
        /// instead study the distribution of `measurement - threshold`
        /// differences. A typical statistical test will for example ensure that
        /// 95% of these differences are above 0.0.
        estimate_t low_tail_bound;

        /// Estimated population mean
        ///
        /// Technically a truncated mean since it is computed over a dataset
        /// from which outliers have been removed by \ref outlier_filter_t.
        ///
        /// The reason we are using a truncated mean rather than the median,
        /// even though it requires outlier filtering to achieve satisfying
        /// outlier resilience, is that in the presence of timing measurements
        /// subjected to clock quantization, the median's "boundary effects" can
        /// lead to problematically large jumps in output values when the
        /// dataset is perturbed in a relatively small fashion.
        ///
        /// They can lead to values that are mostly very stable, yet can vary
        /// dramatically from time to time, and this can way too easily be
        /// misinterpreted as changes of the underlying benchmark load by users.
        estimate_t mean;

        /// Estimated \ref DISPERSION_HIGH_TAIL_BOUND population quantile
        ///
        /// This is the "high" counterpart of `low_tail_bound`. Assuming our
        /// usual 5% \ref DISPERSION_EXCLUDED_FRACTION, the interval `]-inf;
        /// high_tail_bound]` surrounds 95% of data points by setting aside the
        /// highest 5% of the dataset.
        estimate_t high_tail_bound;

        /// Estimated \ref DISPERSION_CENTER_END population quantile
        ///
        /// This is the "high" counterpart of `center_start`.  Assuming our
        /// usual 5% \ref DISPERSION_EXCLUDED_FRACTION, the interval
        /// `[center_start; center_end]` surrounds 95% of data points by setting
        /// aside the lowest and highest 2.5% of the dataset.
        estimate_t center_end;

        /// Estimated width of `[center_start; center_end]`
        ///
        /// If you want to know the spread of data points, as when computing
        /// signal-to-noise ratio metrics, then this statistic is more reliable
        /// than computing the difference `center_end.center -
        /// center_start.center` and guesstimating the width of the confidence
        /// interval, as it will correctly account for correlations between the
        /// two bounds caused by e.g. CPU frequency scaling.
        estimate_t center_width;

        // To add another statistic here, you need an associated entry to
        // statistic_id_e and appropriate code in analyzer_apply().
    } statistics_t;

    /// Identifier for statistics within \ref statistics_t::statistics
    ///
    /// This enum has one entry per \ref estimate_t in \ref statistics_t and is
    /// used to locate the appropriate data sub-array inside of \ref analyzer_t.
    typedef enum statistic_id_e {
        CENTER_START = 0,
        LOW_TAIL_BOUND,
        MEAN,
        HIGH_TAIL_BOUND,
        CENTER_END,
        CENTER_WIDTH,
        NUM_STATISTICS  // Must remain the last entry of this enum
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


    /// \name Public \ref estimate_t API
    /// \{

    /// Compute the relative dispersion of some \ref estimate_t
    ///
    /// \param estimate is an \ref estimate_t that directly or indirectly derive
    ///                 from some measurements.
    /// \returns the relative magnitude of its dispersion in percentage points
    ///          of the central tendency.
    static inline double relative_dispersion(estimate_t estimate) {
        return (estimate.high - estimate.low) / estimate.sample * 100.0;
    }

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
    /// \param batch_mean is an estimate of the mean duration of
    ///                   `batch_size` benchmark loop iterations.
    /// \param batch_size is the number of iterations that are timed by
    ///                   `batch_mean`.
    ///
    /// \returns an estimate of the duration of one benchmark loop iteration.
    static inline
    estimate_t estimate_iteration_duration(estimate_t batch_mean,
                                           size_t batch_size) {
        estimate_t iter_mean;
        // Per linearity hypothesis, run duration = sum(iter duration)
        // From this, i.i.d. hypothesis gives us linear mean & variance scaling
        iter_mean.sample = batch_mean.sample / batch_size;
        // Given linear variance scaling, we trivially deduce that stddev scales
        // as the square root of the number of iterations...
        const double stddev_norm = 1.0 / sqrt(batch_size);
        // ...which, per the assumed confidence interval scaling law, gives us
        // the iteration duration confidence interval.
        iter_mean.low = iter_mean.sample
                      - (batch_mean.sample - batch_mean.low) * stddev_norm;
        iter_mean.high = iter_mean.sample
                       + (batch_mean.high - batch_mean.sample) * stddev_norm;
        return iter_mean;
    }

    /// Log a statistical estimate
    ///
    /// This will log the string specified by "header", followed by a colon,
    /// followed by a description of `estimate`.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param level is the verbosity level at which this log will be emitted
    /// \param header is a string that will be prepended to the log. This is
    ///               typically used for list bullets and estimate names.
    /// \param estimate is the \ref estimate_t to be displayed
    /// \param mean_difference is used to indicate how much the measured
    ///                        quantity differs from the distribution mean,
    ///                        you can leave this as "" if not needed.
    /// \param unit is a string that spells out the measurement unit of
    ///             `estimate`
    UDIPE_NON_NULL_ARGS
    void log_estimate(udipe_log_level_t level,
                      const char header[],
                      estimate_t estimate,
                      const char mean_difference[],
                      const char unit[]);

    /// \}


    /// \name Public \ref statistics_t API
    /// \{

    /// Log measurement statistics
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param level is the verbosity level at which this log will be emitted
    /// \param title serves as a header to the overall statistics display
    /// \param bullet will be prepended to each estimate's display
    /// \param stats are the \ref statistics_t to be displayed
    /// \param unit is a string that spells out the measurement unit of
    ///             `stats`
    UDIPE_NON_NULL_ARGS
    void log_statistics(udipe_log_level_t level,
                        const char title[],
                        const char bullet[],
                        statistics_t stats,
                        const char unit[]);

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


    /// \name Implementation details
    /// \{

    /// Kind of comparison between a quantity quantity and a mean
    ///
    typedef enum mean_comparison_e {
        DELTA,  ///< "mean+/-1.2%" relative delta, fallback to ratio for large deltas
        FRACTION,  ///< "1.2% of mean" relative fraction, fallback to ratio for large deltas
        RATIO   ///< "1.2x mean" relative ratio
    } mean_comparison_t;

    /// Describe a percentile of a distribution
    ///
    /// \param output is the location where the description will be written
    /// \param output_size is the capacity of `output` in bytes
    /// \param prefix is a string to be prepended at the beginning (this
    ///               is mainly used for bullet lists)
    /// \param quantile is the quantile to be displayed, in range ]0.0; 1.0[
    ///
    /// \returns the number of bytes that were written to `output`
    UDIPE_NON_NULL_ARGS
    static inline size_t write_percentile_header(char output[],
                                                 size_t output_size,
                                                 const char prefix[],
                                                 double quantile) {
        ensure_gt(quantile, 0.0);
        ensure_lt(quantile, 1.0);
        const int len = snprintf(output, output_size,
                                 "%sP%.1f",
                                 prefix, quantile * 100.0);
        ensure_gt(len, 0);
        ensure_lt((size_t)len, output_size);
        return (size_t)len;
    }

    /// Describe how much a value differs from the sample mean of a distribution
    ///
    /// \param output is the location where the description will be written
    /// \param output_size is the capacity of `output` in bytes
    /// \param value is the value to be analyzed
    /// \param comparison is the kind of comparison that should be performed
    /// \param sample_mean is the sample mean to which it should be compared
    ///
    /// \returns the number of bytes that were written to `output`
    UDIPE_NON_NULL_ARGS
    size_t write_mean_difference(char output[],
                                 size_t output_size,
                                 estimate_t value,
                                 mean_comparison_t comparison,
                                 double sample_mean);

    /// Log the estimate of a particular distribution quantile
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param level is the verbosity level at which this log will be emitted
    /// \param prefix will be prepended to each estimate's display
    /// \param quantile is the quantile of interest in range ]0.0; 1.0[
    /// \param estimate is the estimate of the quantile of interest
    /// \param sample_mean is the mean of the underlying sample
    /// \param unit is a string that spells out the measurement unit of
    ///             `estimate`
    UDIPE_NON_NULL_ARGS
    static inline void log_quantile_estimate(udipe_log_level_t level,
                                             const char prefix[],
                                             double quantile,
                                             estimate_t estimate,
                                             double sample_mean,
                                             const char unit[]) {
        char header[48];
        write_percentile_header(header,
                                sizeof(header),
                                prefix,
                                quantile);
        char mean_difference[32];
        write_mean_difference(mean_difference,
                              sizeof(mean_difference),
                              estimate,
                              DELTA,
                              sample_mean);
        log_estimate(level,
                     header,
                     estimate,
                     mean_difference,
                     unit);
    }

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

    /// Estimate the confidence interval of a statistic based on bootstrap data
    ///
    /// This function must be run after the `analyzer::statistics` array has
    /// been filled up with data from bootstrap resampling.
    ///
    /// \param analyzer must be an \ref analyzer_t that has been initialized
    ///                 with analyzer_initialize() and has not been destroyed
    ///                 with analyzer_finalize() yet.
    /// \param stat is a \ref statistic_id_t that indicates which population
    ///             statistic should be estimated.
    /// \param estimate is the estimate whose confidence interval should be set
    UDIPE_NON_NULL_ARGS
    void set_result_confidence(analyzer_t* analyzer,
                               statistic_id_t stat,
                               estimate_t* estimate);

    /// \}


    // TODO: Add unit tests

#endif  // UDIPE_BUILD_BENCHMARKS