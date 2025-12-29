#ifdef UDIPE_BUILD_BENCHMARKS

    #include "benchmark.h"

    #include <udipe/log.h>
    #include <udipe/pointer.h>

    #include "memory.h"
    #include "visibility.h"

    #include <assert.h>
    #include <hwloc.h>
    #include <math.h>
    #include <stddef.h>
    #include <stdint.h>
    #include <stdlib.h>
    #include <string.h>


    /// Dataset size for median duration computations
    ///
    /// To reduce the impact of outliers, we don't directly handle raw
    /// durations, we handle medians of a small number of duration samples. The
    /// number of samples in use is controlled by this parameter.
    ///
    /// This parameter should be tuned as follows:
    ///
    /// - To avoid inventing output durations that weren't present in input
    ///   data, this parameter should be odd, not even.
    /// - If this parameter is too small, then outlier regression will become
    ///   ineffective. But since outliers are very rare, a typical median
    ///   duration input only contains 0 or 1 outlier, so it doesn't take a huge
    ///   median dataset size to minimize the effect of outliers.
    /// - If this parameter is too large, then output statistics will require a
    ///   larger amount of input data to converge, will be more expensive to
    ///   compute, and will be less sensitive to small changes of input data.
    #define NUM_MEDIAN_SAMPLES ((size_t)11)
    static_assert(NUM_MEDIAN_SAMPLES % 2 == 1,
                  "Medians should be computed over an odd number of samples");

    /// Confidence interval used for clock calibration
    ///
    /// Setting this much tighter than \ref RESULT_CONFIDENCE ensures that if
    /// the calibration deviates a bit from its optimal value, it will have a
    /// smaller impact on the end results.
    #define CALIBRATION_CONFIDENCE 99.0
    static_assert(CALIBRATION_CONFIDENCE >= MEASUREMENT_CONFIDENCE,
                  "Calibration should be at least as strict as user measurements");

    /// Desired number of measurements on either side of the confidence interval
    ///
    /// This parameter should be tuned up until multiple runs of the analysis
    /// process over the same input data consistently produce the same results.
    ///
    /// Tuning it too high will increase the overhead of the statistical
    /// analysis process for no good reason.
    #define NUM_EDGE_MEASUREMENTS ((size_t)10)

    /// Number of benchmark runs used for OS clock offset calibration
    ///
    /// This should be tuned high enough that the OS clock offset calibration
    /// produces reproducible results.
    //
    // TODO: Current value is the minimum required run count needed for
    //       reproducibility on the author's laptop. This minimum should be
    //       tuned up through testing on a more diverse set of systems, then
    //       once the pool of available testing systems is exhausted, some extra
    //       safety margin (maybe 2-4x?) should be applied on top of the final
    //       result to account for unknown systems.
    #define NUM_RUNS_OFFSET_OS ((size_t)32*1024)

    /// Number of benchmark runs used for shortest loop calibration
    ///
    /// This should be tuned high enough that the shortest loop calibration
    /// consistently ends at a number of loop iterations smaller than the
    /// optimal number of loop iterations.
    //
    // TODO: Generally speaking, doing statistics on less than 100 samples makes
    //       the author nervous, and on the author's laptop that is already
    //       enough. Test on more systems and tune up if needed.
    #define NUM_RUNS_SHORTEST_LOOP ((size_t)128)

    /// Number of benchmark run used for optimal loop calibration, when using
    /// the system clock to perform said calibration
    ///
    /// This should be tuned high enough that the optimal loop calibration
    /// consistently ends at the same number of loop iterations and yields
    /// similar final statistics.
    //
    // TODO: Current value is the minimum required run count needed for
    //       reproducibility on the author's laptop. This minimum should be
    //       tuned up through testing on a more diverse set of systems, then
    //       once the pool of available testing systems is exhausted, some extra
    //       safety margin (maybe 2-4x?) should be applied on top of the final
    //       result to account for unknown systems.
    #define NUM_RUNS_BEST_LOOP_OS ((size_t)8*1024)

    #ifdef X86_64

        /// Number of benchmark runs used when measuring the duration of the
        /// optimal loop using the x86 TimeStamp Counter
        ///
        /// This should be tuned high enough that the optimal loop measurement
        /// produces reproducible results.
        //
        // TODO: Current value is the minimum required run count needed for
        //       reproducibility on the author's laptop. This minimum should be
        //       tuned up through testing on a more diverse set of systems, then
        //       once the pool of available testing systems is exhausted, some
        //       extra safety margin (maybe 2-4x?) should be applied on top of
        //       the final result to account for unknown systems.
        #define NUM_RUNS_BEST_LOOP_X86 ((size_t)512)

        /// Number of benchmark runs used for TSC clock offset calibration
        ///
        /// This should be tuned high enough that the TSC clock offset
        /// calibration produces reproducible results.
        //
        // TODO: Generally speaking, doing statistics on less than 100 samples
        //       makes the author nervous, and on the author's laptop that is
        //       already enough. Test on more systems and tune up if needed.
        #define NUM_RUNS_OFFSET_X86 ((size_t)128)

    #endif  // X86_64


    /// Comparison function for applying qsort() to int64_t[]
    static inline int compare_i64(const void* v1, const void* v2) {
        const int64_t* const d1 = (const int64_t*)v1;
        const int64_t* const d2 = (const int64_t*)v2;
        if (*d1 < *d2) return -1;
        if (*d1 > *d2) return 1;
        return 0;
    }


    /// Logical size of a bin from a \ref distribution_t
    ///
    /// \ref distribution_t internally uses a structure-of-array layout, so it
    /// is not literally an array of `(int64_t, size_t)` pairs but rather an
    /// array of `int64_t` followed by an array of `size_t`.
    const size_t distribution_bin_size = sizeof(int64_t) + sizeof(size_t);

    /// Allocate a distribution of duration-based values
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param capacity is the number of bins that the distribution should be
    ///                 able to hold internally before reallocating.
    ///
    /// \returns a distribution that must later been liberated using
    ///          distribution_finalize().
    static distribution_t distribution_allocate(size_t capacity) {
        void* const allocation = malloc(capacity * distribution_bin_size);
        exit_on_null(allocation, "Failed to allocate distribution storage");
        debugf("Allocated storage for %zu bins at location %p.",
               capacity, allocation);
        return (distribution_t){
            .allocation = allocation,
            .num_bins = 0,
            .capacity = capacity
        };
    }

    distribution_builder_t distribution_initialize() {
        const size_t capacity = EXPECTED_MIN_PAGE_SIZE / distribution_bin_size;
        return (distribution_builder_t){
            .inner = distribution_allocate(capacity)
        };
    }

    UDIPE_NON_NULL_ARGS
    void distribution_create_bin(distribution_builder_t* builder,
                                 size_t pos,
                                 int64_t value) {
        distribution_t* dist = &builder->inner;
        distribution_layout_t layout = distribution_layout(dist);
        if (dist->num_bins < dist->capacity) {
            const size_t last_pos = dist->num_bins - 1;
            if (pos == last_pos) {
                trace("At end of histogram, can append value directly.");
                layout.sorted_values[pos] = value;
                layout.counts_or_cumsum[pos] = 1;
                ++(dist->num_bins);
                return;
            }

            tracef("Backing up current bin at position %zu...", pos);
            int64_t next_value = layout.sorted_values[pos];
            size_t next_count = layout.counts_or_cumsum[pos];

            trace("Inserting new value...");
            layout.sorted_values[pos] = value;
            layout.counts_or_cumsum[pos] = 1;

            trace("Shifting previous bins up...");
            for (size_t dst = pos + 1; dst < dist->num_bins; ++dst) {
                int64_t tmp_value = layout.sorted_values[dst];
                size_t tmp_count = layout.counts_or_cumsum[dst];
                layout.sorted_values[dst] = next_value;
                layout.counts_or_cumsum[dst] = next_count;
                next_value = tmp_value;
                next_count = tmp_count;
            }

            trace("Restoring last bin...");
            layout.sorted_values[dist->num_bins] = next_value;
            layout.counts_or_cumsum[dist->num_bins] = next_count;
            ++(dist->num_bins);
        } else {
            debug("No room for extra bins, must reallocate...");
            assert(dist->num_bins == dist->capacity);
            distribution_t new_dist = distribution_allocate(2 * dist->capacity);
            distribution_layout_t new_layout = distribution_layout(&new_dist);

            debug("Transferring old values smaller than the new one...");
            for (size_t bin = 0; bin < pos; ++bin) {
                new_layout.sorted_values[bin] = layout.sorted_values[bin];
                new_layout.counts_or_cumsum[bin] = layout.counts_or_cumsum[bin];
            }

            debug("Inserting new value...");
            new_layout.sorted_values[pos] = value;
            new_layout.counts_or_cumsum[pos] = 1;

            debug("Transferring old values larger than the new one...");
            for (size_t src = pos; src < dist->num_bins; ++src) {
                const size_t dst = src + 1;
                new_layout.sorted_values[dst] = layout.sorted_values[src];
                new_layout.counts_or_cumsum[dst] = layout.counts_or_cumsum[src];
            }

            debug("Replacing former distribution...");
            new_dist.num_bins = dist->num_bins + 1;
            distribution_finalize(dist);
            builder->inner = new_dist;
        }
    }

    UDIPE_NON_NULL_ARGS
    distribution_t distribution_finish(distribution_builder_t* builder) {
        debug("Extracting the distribution from the builder...");
        distribution_t dist = builder->inner;
        builder->inner = (distribution_t){
            .allocation = NULL,
            .num_bins = 0,
            .capacity = 0
        };

        debug("Ensuring the distribution can be sampled...");
        ensure_ge(dist.num_bins, (size_t)1);

        distribution_layout_t layout = distribution_layout(&dist);
        if (log_enabled(UDIPE_TRACE)) {
            trace("Final distribution is {");
            for (size_t bin = 0; bin < dist.num_bins; ++bin) {
                tracef("  %zd: %zu,",
                       layout.sorted_values[bin], layout.counts_or_cumsum[bin]);
            }
            trace("}");
        }

        debug("Turning value counts into a cumulative sum...");
        size_t cumsum = layout.counts_or_cumsum[0];
        for (size_t bin = 1; bin < dist.num_bins; ++bin) {
            cumsum += layout.counts_or_cumsum[bin];
            layout.counts_or_cumsum[bin] = cumsum;
        }
        return dist;
    }

    UDIPE_NON_NULL_ARGS
    void distribution_finalize(distribution_t* dist) {
        debugf("Liberating storage at location %p...", dist->allocation);
        free(dist->allocation);

        debug("Poisoning distribution state to detect future invalid usage...");
        *dist = (distribution_t){
            .allocation = NULL,
            .num_bins = 0,
            .capacity = 0
        };
    }


    duration_analyzer_t duration_analyzer_initialize(float confidence_f) {
        debug("Checking analysis parameters...");
        double confidence = (double)confidence_f;
        ensure_gt(confidence, 0.0);
        ensure_lt(confidence, 100.0);

        debug("Determining storage needs...");
        size_t num_medians = ceil((double)(2*NUM_EDGE_MEASUREMENTS)
                                  / (1.0-0.01*confidence));
        if ((num_medians % 2) == 0) num_medians += 1;

        debug("Allocating storage...");
        size_t medians_size = num_medians * sizeof(int64_t);
        int64_t* medians = realtime_allocate(medians_size);

        debug("Finishing setup...");
        double low_quantile = (1.0 - 0.01*confidence) / 2.0;
        double high_quantile = 1.0 - low_quantile;
        return (duration_analyzer_t){
            .medians = medians,
            .num_medians = num_medians,
            .low_idx = (size_t)(low_quantile * num_medians),
            .center_idx = num_medians / 2,
            .high_idx = (size_t)(high_quantile * num_medians)
        };
    }

    UDIPE_NON_NULL_ARGS
    stats_t analyze_duration(duration_analyzer_t* analyzer,
                             int64_t durations[],
                             size_t durations_len) {
        trace("Computing medians...");
        ensure_gt(durations_len, (size_t)0);
        int64_t median_samples[NUM_MEDIAN_SAMPLES];
        for (size_t median = 0; median < analyzer->num_medians; ++median) {
            tracef("- Computing medians[%zu]...", median);
            for (size_t sample = 0; sample < NUM_MEDIAN_SAMPLES; ++sample) {
                size_t duration_idx = rand() % durations_len;
                int64_t duration = durations[duration_idx];
                tracef("  * Picked %zu-th sample durations[%zu] = %zd, inserting...",
                       sample, duration_idx, duration);

                ptrdiff_t prev;
                for (prev = sample - 1; prev >= 0; --prev) {
                    int64_t pivot = median_samples[prev];
                    tracef("    - Checking median_samples[%zd] = %zd...",
                           prev, pivot);
                    if (pivot > duration) {
                        trace("    - Too high, shift that up to make room.");
                        median_samples[prev + 1] = median_samples[prev];
                        continue;
                    } else {
                        trace("    - Small enough, sample goes after that.");
                        break;
                    }
                }
                tracef("  * Sample inserted at median_samples[%zd].", prev + 1);
                median_samples[prev + 1] = duration;
            }
            analyzer->medians[median] = median_samples[NUM_MEDIAN_SAMPLES / 2];
            tracef("  * medians[%zu] is therefore %zd.",
                   median, analyzer->medians[median]);
        }

        trace("Computing result...");
        qsort(analyzer->medians,
              analyzer->num_medians,
              sizeof(int64_t),
              compare_i64);
        return (stats_t){
            .center = analyzer->medians[analyzer->center_idx],
            .low = analyzer->medians[analyzer->low_idx],
            .high = analyzer->medians[analyzer->high_idx]
        };
    }

    UDIPE_NON_NULL_ARGS
    void duration_analyzer_finalize(duration_analyzer_t* analyzer) {
        debug("Liberating storage...");
        realtime_liberate(analyzer->medians,
                          analyzer->num_medians * sizeof(int64_t));

        debug("Poisoining analyzer state...");
        analyzer->medians = NULL;
        analyzer->num_medians = 0;
        analyzer->center_idx = SIZE_MAX;
        analyzer->low_idx = SIZE_MAX;
        analyzer->high_idx = SIZE_MAX;
    }


    /// Empty benchmark workload
    ///
    /// Used to measure the offset of the operating system clock.
    static inline void nothing(void* /* context */) {}

    /// Empty-loop benchmark workload
    ///
    /// Used to measure the maximal precision of a clock and the maximal
    /// benchmark duration before OS interrupts start hurting clock precision.
    ///
    /// \param context must be a `const size_t*` indicating the desired amount
    ///                of loop iterations.
    UDIPE_NON_NULL_ARGS
    static inline void empty_loop(void* context) {
        size_t num_iters = *((const size_t*)context);
        // Ensures that all loop lengths get the same codegen
        UDIPE_ASSUME_ACCESSED(num_iters);
        for (size_t iter = 0; iter < num_iters; ++iter) {
            UDIPE_ASSUME_READ(iter);
        }
    }

    /// Log statistics from the calibration process
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param level is the verbosity level at which this log will be emitted
    /// \param header is a string that will be prepended to the log
    /// \param stats is \ref stats_t from the calibration process that will
    ///              be printed out
    /// \param unit is a string that spells out the measurement unit of
    ///             `stats`
    #define log_calibration_stats(level, header, stats, unit)  \
        do {  \
            stats_t udipe_duration = (stats);  \
            udipe_logf((level),  \
                       "%s: %zd %s with %g%% CI [%zd; %zd].",  \
                       (header),  \
                       udipe_duration.center,  \
                       (unit),  \
                       CALIBRATION_CONFIDENCE,  \
                       udipe_duration.low,  \
                       udipe_duration.high);  \
        } while(false)

    /// Compute the relative uncertainty from some \ref stats_t
    ///
    /// \param stats is \ref stats_t that directly or indirectly derive from
    ///              some measurements.
    /// \returns the associated statistical uncertainty in percentage points
    static inline double relative_uncertainty(stats_t stats) {
        return (double)(stats.high - stats.low) / stats.center * 100.0;
    }

    /// Log per-iteration statistics from the calibration process
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param level is the verbosity level at which this log will be emitted
    /// \param stats is \ref stats_t from the calibration process that will
    ///              be printed out
    /// \param unit is a string that spells out the measurement unit of
    ///             `stats`
    #define log_iteration_stats(level, bullet, stats, num_iters, unit)  \
        do {  \
            const stats_t udipe_stats = (stats);  \
            const size_t udipe_num_iters = (num_iters);  \
            const double udipe_center = (double)udipe_stats.center / udipe_num_iters;  \
            const double udipe_low = (double)udipe_stats.low / udipe_num_iters;  \
            const double udipe_high = (double)udipe_stats.high / udipe_num_iters;  \
            const double udipe_spread = udipe_high - udipe_low;  \
            const int udipe_stats_decimals = ceil(-log10(udipe_spread));  \
            const double udipe_uncertainty = relative_uncertainty(udipe_stats);  \
            int udipe_uncertainty_decimals = ceil(-log10(udipe_uncertainty));  \
            if (udipe_uncertainty_decimals < 0) udipe_uncertainty_decimals = 0;  \
            udipe_logf((level),  \
                       "%s That's %.*f %s/iter with %g%% CI [%.*f; %.*f] (%.*f%% uncertainty).",  \
                       (bullet),  \
                       udipe_stats_decimals,  \
                       udipe_center,  \
                       (unit),  \
                       CALIBRATION_CONFIDENCE,  \
                       udipe_stats_decimals,  \
                       udipe_low,  \
                       udipe_stats_decimals,  \
                       udipe_high,  \
                       udipe_uncertainty_decimals,  \
                       udipe_uncertainty);  \
        } while(false)

    UDIPE_NON_NULL_ARGS
    os_clock_t os_clock_initialize(duration_analyzer_t* calibration_analyzer) {
        // Zero out all clock fields initially
        //
        // This is a valid (if incorrect) value for some fields but not all of
        // them. We will take care of the missing fields later on.
        os_clock_t clock = { 0 };

        #ifdef _WIN32
            debug("Obtaining Windows performance counter frequency...");
            clock.win32_frequency = QueryPerformanceFrequency().QuadPart;
        #endif

        debug("Allocating timestamp and duration buffers...");
        size_t max_runs = NUM_RUNS_OFFSET_OS;
        if (max_runs < NUM_RUNS_SHORTEST_LOOP) max_runs = NUM_RUNS_SHORTEST_LOOP;
        if (max_runs < NUM_RUNS_BEST_LOOP_OS) max_runs = NUM_RUNS_BEST_LOOP_OS;
        const size_t timestamps_size = (max_runs+1) * sizeof(os_timestamp_t);
        const size_t durations_size = max_runs * sizeof(signed_duration_ns_t);
        clock.timestamps = realtime_allocate(timestamps_size);
        clock.durations = realtime_allocate(durations_size);
        clock.num_durations = max_runs;

        info("Calibrating clock offset...");
        clock.offset_stats = os_clock_measure(
            &clock,
            nothing,
            NULL,
            NUM_RUNS_OFFSET_OS,
            calibration_analyzer
        );
        log_calibration_stats(UDIPE_INFO,
                              "- Clock offset",
                              clock.offset_stats,
                              "ns");
        // TODO: This way of combining confidence intervals is not statistically
        //       correct and will lead confidence intervals to be
        //       over-estimated. A proper bootstrap resampling approach would be
        //       to randomly sample pairs of offsets and compute their
        //       difference.
        infof("- Offset correction will increase %g%% CI by [%+zd; %+zd] ns.",
              CALIBRATION_CONFIDENCE,
              clock.offset_stats.center-clock.offset_stats.high,
              clock.offset_stats.center-clock.offset_stats.low);

        info("Finding minimal measurable loop...");
        stats_t loop_duration;
        size_t num_iters = 1;
        do {
            debugf("- Trying loop with %zu iteration(s)...", num_iters);
            loop_duration = os_clock_measure(
                &clock,
                empty_loop,
                &num_iters,
                NUM_RUNS_SHORTEST_LOOP,
                calibration_analyzer
            );
            log_calibration_stats(UDIPE_DEBUG,
                                  "  * Loop duration",
                                  loop_duration,
                                  "ns");
            if (loop_duration.low > clock.offset_stats.high) {
                debug("  * Loop finally contributes more to measurements than clock offset!");
                break;
            } else {
                debug("  * That's not even the clock offset, try a longer loop...");
                num_iters *= 2;
                continue;
            }
        } while(true);
        infof("- Loops with >=%zu iterations have non-negligible duration.",
              num_iters);

        info("Finding optimal loop duration...");
        clock.best_empty_iters = num_iters;
        clock.best_empty_stats = loop_duration;
        double best_uncertainty = relative_uncertainty(loop_duration);
        // TODO: This way of combining confidence intervals is not statistically
        //       correct and will lead confidence intervals to be
        //       over-estimated. A proper bootstrap resampling approach would be
        //       to randomly sample pairs of offsets and compute statistics over
        //       their difference.
        int64_t best_precision =
            (clock.offset_stats.high - clock.offset_stats.low)
            - (clock.offset_stats.low - clock.offset_stats.high);
        do {
            num_iters *= 2;
            debugf("- Trying loop with %zu iterations...", num_iters);
            loop_duration = os_clock_measure(
                &clock,
                empty_loop,
                &num_iters,
                NUM_RUNS_BEST_LOOP_OS,
                calibration_analyzer
            );
            log_calibration_stats(UDIPE_DEBUG,
                                  "  * Loop duration",
                                  loop_duration,
                                  "ns");
            log_iteration_stats(UDIPE_DEBUG,
                                "  *",
                                loop_duration,
                                num_iters,
                                "ns");
            const double uncertainty = relative_uncertainty(loop_duration);
            if (loop_duration.high - loop_duration.low > 2*best_precision) {
                debug("  * Timing precision degraded by >2x. Time to stop!");
                break;
            } else if (uncertainty < best_uncertainty) {
                debug("  * This is our new best loop. Can we do even better?");
                clock.best_empty_iters = num_iters;
                best_uncertainty = uncertainty;
                clock.best_empty_stats = loop_duration;
                continue;
            } else {
                debug("  * That's not much worse. Keep trying...");
                continue;
            }
        } while(true);
        infof("- Achieved optimal precision at %zu loop iterations.",
              clock.best_empty_iters);
        log_calibration_stats(UDIPE_INFO,
                              "- Best loop duration",
                              clock.best_empty_stats,
                              "ns");
        log_iteration_stats(UDIPE_INFO,
                            "-",
                            clock.best_empty_stats,
                            clock.best_empty_iters,
                            "ns");
        return clock;
    }

    UDIPE_NON_NULL_ARGS
    void os_clock_finalize(os_clock_t* clock) {
        debug("Liberating and poisoning measurement storage...");
        realtime_liberate(clock->timestamps,
                          (clock->num_durations+1) * sizeof(os_timestamp_t));
        clock->timestamps = NULL;
        realtime_liberate(clock->durations,
                          clock->num_durations * sizeof(signed_duration_ns_t));
        clock->durations = NULL;
        clock->num_durations = 0;

        debug("Poisoning the now-invalid OS clock...");
        #ifdef _WIN32
            clock->win32_frequency = 0;
        #endif
        clock->offset_stats = (stats_t){
            .low = INT64_MIN,
            .center = INT64_MIN,
            .high = INT64_MIN
        };
        clock->best_empty_iters = SIZE_MAX;
        clock->best_empty_stats = (stats_t){
            .low = INT64_MIN,
            .center = INT64_MIN,
            .high = INT64_MIN
        };
    }


    #ifdef X86_64

        UDIPE_NON_NULL_ARGS
        x86_clock_t
        x86_clock_initialize(const os_clock_t* os,
                             duration_analyzer_t* calibration_analyzer) {
            // Zero out all clock fields initially
            //
            // This is a valid (if incorrect) value for some fields but not all
            // of them. We will take care of the missing fields later on.
            x86_clock_t clock = { 0 };

            debug("Allocating timestamp and duration buffers...");
            size_t max_runs = NUM_RUNS_BEST_LOOP_X86;
            if (max_runs < NUM_RUNS_OFFSET_X86) max_runs = NUM_RUNS_OFFSET_X86;
            const size_t instants_size = 2 * max_runs * sizeof(x86_instant);
            const size_t ticks_size = max_runs * sizeof(x86_duration_ticks);
            clock.instants = realtime_allocate(instants_size);
            clock.ticks = realtime_allocate(ticks_size);
            clock.num_durations = max_runs;

            // This should happen as soon as possible to reduce the risk of CPU
            // clock frequency changes, which would degrade the quality of our
            // TSC frequency calibration
            //
            // TODO: Investigate paired benchmarking techniques as a more robust
            //       alternative to reducing the delay between these two
            //       measurements. The general idea is to alternatively measure
            //       durations with the OS and TSC clocks, use pairs of raw
            //       duration data points from each clock to compute frequency
            //       samples, and compute statistics over these frequency
            //       samples. This way we are using data that was acquired in
            //       similar system configurations, so even if the system
            //       configuration changes over time, the results remain stable.
            info("Measuring optimal loop again with the TSC...");
            size_t best_empty_iters = os->best_empty_iters;
            const stats_t raw_empty_stats = x86_clock_measure(
                &clock,
                empty_loop,
                &best_empty_iters,
                NUM_RUNS_BEST_LOOP_X86,
                calibration_analyzer
            );
            log_calibration_stats(UDIPE_INFO,
                                  "- Offset-biased best loop stats",
                                  raw_empty_stats,
                                  "ticks");

            info("Calibrating TSC offset...");
            clock.offset_stats = x86_clock_measure(
                &clock,
                nothing,
                NULL,
                NUM_RUNS_OFFSET_X86,
                calibration_analyzer
            );
            log_calibration_stats(UDIPE_INFO,
                                  "- Clock offset",
                                  clock.offset_stats,
                                  "ticks");
            // TODO: This way of combining confidence intervals is not
            //       statistically correct and will lead confidence intervals to
            //       be over-estimated. A proper bootstrap resampling approach
            //       would be to keep around the offset dataset and subtract
            //       another random point from the offset dataset from each
            //       point, thus producing a difference-of-offsets dataset over
            //       which statistics can be computed.
            infof("- Offset correction will increase %g%% CI by [%+zd; %+zd] ticks.",
                  CALIBRATION_CONFIDENCE,
                  clock.offset_stats.center-clock.offset_stats.high,
                  clock.offset_stats.center-clock.offset_stats.low);

            // TODO: This way of combining confidence intervals is not
            //       statistically correct and will lead confidence intervals to
            //       be over-estimated. A proper bootstrap resampling approach
            //       would be to keep around the offset dataset and subtract a
            //       random offset from this dataset from each of the (end -
            //       start) raw deltas, then compute statistics over that.
            debug("Applying offset correction to best loop duration...");
            clock.best_empty_stats = (stats_t){
                .center = raw_empty_stats.center - clock.offset_stats.center,
                .low = raw_empty_stats.low - clock.offset_stats.high,
                .high = raw_empty_stats.high - clock.offset_stats.low
            };
            log_calibration_stats(UDIPE_DEBUG,
                                  "- Offset-corrected best loop stats",
                                  clock.best_empty_stats,
                                  "ticks");
            log_iteration_stats(UDIPE_DEBUG,
                                "-",
                                clock.best_empty_stats,
                                os->best_empty_iters,
                                "ticks");

            info("Deducing TSC tick frequency...");
            const int64_t nano = 1000*1000*1000;
            // TODO: This way of combining confidence intervals is not
            //       statistically correct and will lead confidence intervals to
            //       be over-estimated. A proper bootstrap resampling approach
            //       would be to keep around the best empty loop dataset from
            //       the OS clock and divide each point from the TSC dataset by
            //       a random point from the OS clock dataset.
            clock.frequency_stats = (stats_t){
                .center = clock.best_empty_stats.center * nano / os->best_empty_stats.center,
                .low = clock.best_empty_stats.low * nano / os->best_empty_stats.high,
                .high = clock.best_empty_stats.high * nano / os->best_empty_stats.low
            };
            log_calibration_stats(UDIPE_INFO,
                                  "- TSC frequency",
                                  clock.frequency_stats,
                                  "ticks/sec");

            debug("Deducing best loop duration...");
            const stats_t best_empty_duration = x86_duration(
                &clock,
                clock.best_empty_stats
            );
            log_calibration_stats(UDIPE_DEBUG,
                                  "- Best loop duration",
                                  best_empty_duration,
                                  "ns");
            log_iteration_stats(UDIPE_DEBUG,
                                "-",
                                best_empty_duration,
                                os->best_empty_iters,
                                "ns");
            return clock;
        }

        UDIPE_NON_NULL_ARGS
        void x86_clock_finalize(x86_clock_t* clock) {
            debug("Liberating and poisoning measurement storage...");
            realtime_liberate(clock->instants,
                              2 * clock->num_durations * sizeof(x86_instant));
            clock->instants = NULL;
            realtime_liberate(clock->ticks,
                              clock->num_durations * sizeof(x86_duration_ticks));
            clock->ticks = NULL;
            clock->num_durations = 0;

            debug("Poisoning the now-invalid TSC clock...");
            clock->best_empty_stats = (stats_t){
                .low = INT64_MIN,
                .center = INT64_MIN,
                .high = INT64_MIN
            };
            clock->offset_stats = (stats_t){
                .low = INT64_MIN,
                .center = INT64_MIN,
                .high = INT64_MIN
            };
            clock->frequency_stats = (stats_t){
                .low = INT64_MIN,
                .center = INT64_MIN,
                .high = INT64_MIN
            };
        }

    #endif  // X86_64


    benchmark_clock_t benchmark_clock_initialize() {
        // Zero out all clock fields initially
        //
        // This is a valid (if incorrect) value for some fields but not all of
        // them. We will take care of the missing fields later on.
        benchmark_clock_t clock = { 0 };

        debug("Setting up clock calibration analysis...");
        clock.calibration_analyzer =
            duration_analyzer_initialize(CALIBRATION_CONFIDENCE);

        info("Setting up the OS clock...");
        clock.os = os_clock_initialize(&clock.calibration_analyzer);

        #ifdef X86_64
            info("Setting up the TSC clock...");
            clock.x86 = x86_clock_initialize(&clock.os,
                                             &clock.calibration_analyzer);
        #endif

        debug("Setting up duration measurement analysis...");
        clock.measurement_analyzer =
            duration_analyzer_initialize(MEASUREMENT_CONFIDENCE);
        return clock;
    }

    UDIPE_NON_NULL_ARGS
    void benchmark_clock_recalibrate(benchmark_clock_t* clock) {
        // TODO: Check if clock calibration still seems correct, recalibrate if
        //       needed.
        // TODO: This should probably be implemented by implementing recalibrate
        //       for the x86 and OS clocks, then calling these here.
        error("Not implemented yet!");
        exit(EXIT_FAILURE);
    }

    UDIPE_NON_NULL_ARGS
    void benchmark_clock_finalize(benchmark_clock_t* clock) {
        debug("Liberating the measurement analyzer...");
        duration_analyzer_finalize(&(clock->measurement_analyzer));

        #ifdef X86_64
            debug("Liberating the TSC clock...");
            x86_clock_finalize(&clock->x86);
        #endif

        debug("Liberating the OS clock...");
        os_clock_finalize(&clock->os);

        debug("Liberating the calibration analyzer...");
        duration_analyzer_finalize(&(clock->calibration_analyzer));
    }


    DEFINE_PUBLIC
    UDIPE_NON_NULL_ARGS
    UDIPE_NON_NULL_RESULT
    udipe_benchmark_t* udipe_benchmark_initialize(int argc, char *argv[]) {
        // Set up logging
        logger_t logger = logger_initialize((udipe_log_config_t){ 0 });
        udipe_benchmark_t* benchmark;
        with_logger(&logger, {
            debug("Setting up benchmark harness...");
            benchmark =
                (udipe_benchmark_t*)realtime_allocate(sizeof(udipe_benchmark_t));
            memset(benchmark, 0, sizeof(udipe_benchmark_t));
            benchmark->logger = logger;

            // Warn about bad build/runtime configurations
            #ifndef NDEBUG
                warning("You are running micro-benchmarks on a Debug build. "
                        "This will bias measurements!");
            #else
                if (benchmark->logger.min_level <= UDIPE_DEBUG) {
                    warning("You are running micro-benchmarks with DEBUG/TRACE "
                            "logging enabled. This will bias measurements!");
                }
            #endif

            debug("Setting up benchmark name filter...");
            ensure_le(argc, 2);
            const char* filter_key = (argc == 2) ? argv[1] : "";
            benchmark->filter = name_filter_initialize(filter_key);

            debug("Setting up the hwloc topology...");
            exit_on_negative(hwloc_topology_init(&benchmark->topology),
                             "Failed to allocate the hwloc hopology!");
            exit_on_negative(hwloc_topology_load(benchmark->topology),
                             "Failed to build the hwloc hopology!");

            debug("Pinning the benchmark timing thread...");
            benchmark->timing_cpuset = hwloc_bitmap_alloc();
            exit_on_null(benchmark->timing_cpuset,
                         "Failed to allocate timing thread cpuset");
            exit_on_negative(
                hwloc_get_last_cpu_location(benchmark->topology,
                                            benchmark->timing_cpuset,
                                            HWLOC_CPUBIND_THREAD),
                "Failed to query timing thread cpuset"
            );
            exit_on_negative(hwloc_set_cpubind(benchmark->topology,
                                               benchmark->timing_cpuset,
                                               HWLOC_CPUBIND_THREAD
                                               | HWLOC_CPUBIND_STRICT),
                             "Failed to pin the timing thread");

            // Set up the benchmark clock
            benchmark->clock = benchmark_clock_initialize();
        });
        return benchmark;
    }

    DEFINE_PUBLIC
    UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 3)
    bool udipe_benchmark_run(udipe_benchmark_t* benchmark,
                             const char* name,
                             udipe_benchmark_runnable_t runnable,
                             void* context) {
        bool name_matches;
        with_logger(&benchmark->logger, {
            name_matches = name_filter_matches(benchmark->filter, name);
            if (name_matches) {
                trace("Pinning the benchmark timing thread...");
                exit_on_negative(hwloc_set_cpubind(benchmark->topology,
                                                   benchmark->timing_cpuset,
                                                   HWLOC_CPUBIND_THREAD
                                                   | HWLOC_CPUBIND_STRICT),
                                 "Failed to pin benchmark timing thread");

                tracef("Running benchmark \"%s\"...", name);
                // TODO: Make this a realtime thread with a priority given by
                //       environment variable UDIPE_PRIORITY_BENCHMARK if set,
                //       by default halfway from the bottom of the realtime
                //       priority range to UDIPE_PRIORITY_WORKER, which itself
                //       is by default halfway through the entire realtime
                //       priority range. Start writing an environment variable
                //       doc that covers this + UDIPE_LOG.
                runnable(context, benchmark);

                trace("Recalibrating benchmark clock...");
                benchmark_clock_recalibrate(&benchmark->clock);
            }
        });
        return name_matches;
    }

    DEFINE_PUBLIC
    UDIPE_NON_NULL_ARGS
    void udipe_benchmark_finalize(udipe_benchmark_t** benchmark) {
        logger_t logger = (*benchmark)->logger;
        with_logger(&logger, {
            info("All benchmarks executed successfully!");

            debug("Finalizing the benchmark clock...");
            benchmark_clock_finalize(&(*benchmark)->clock);

            debug("Freeing and poisoning the timing thread cpuset...");
            hwloc_bitmap_free((*benchmark)->timing_cpuset);
            (*benchmark)->timing_cpuset = NULL;

            debug("Destroying and poisoning the hwloc topology...");
            hwloc_topology_destroy((*benchmark)->topology);
            (*benchmark)->topology = NULL;

            debug("Finalizing the benchmark name filter...");
            name_filter_finalize(&(*benchmark)->filter);

            debug("Liberating and poisoning the benchmark...");
            realtime_liberate(*benchmark, sizeof(udipe_benchmark_t));
            *benchmark = NULL;

            debug("Finalizing the logger...");
        });
        logger_finalize(&logger);
    }


    DEFINE_PUBLIC void udipe_micro_benchmarks(udipe_benchmark_t* benchmark) {
        // Microbenchmarks are ordered such that a piece of code is
        // benchmarked before other pieces of code that may depend on it
        // TODO: UDIPE_BENCHMARK(benchmark, xyz_micro_benchmarks, NULL);
    }

#endif  // UDIPE_BUILD_BENCHMARKS