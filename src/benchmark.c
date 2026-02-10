#ifdef UDIPE_BUILD_BENCHMARKS

    #include "benchmark.h"

    #include <udipe/log.h>
    #include <udipe/pointer.h>

    #include "benchmark/outlier_filter.h"
    #include "benchmark/distribution_log.h"
    #include "benchmark/statistics.h"
    #include "error.h"
    #include "log.h"
    #include "memory.h"
    #include "visibility.h"

    #include <assert.h>
    #include <hwloc.h>
    #include <math.h>
    #include <stddef.h>
    #include <stdint.h>
    #include <stdlib.h>
    #include <string.h>


    /// Number of samples used for median duration computations
    ///
    /// To reduce the impact of outliers, we don't directly handle raw
    /// durations, we handle medians of a small number of duration samples. This
    /// parameter controls the number of samples that are used.
    ///
    /// Tuning this parameter has many consequences:
    ///
    /// - It can only take odd values. No pseudo-median allowed.
    /// - Tuning it higher allows you to tolerate more OS interrupts, and thus
    ///   work with benchmark run durations that are closer to the
    ///   inter-interrupt spacing. Given a fixed run timing precision, these
    ///   longer benchmark runs let you achieve lower uncertainty on the
    ///   benchmark iteration duration.
    /// - Tuning it higher makes statistics more sensitive to the difference
    ///   between the empirical duration distribution and the true duration
    ///   distribution, therefore you need to collect more benchmark run
    ///   duration data points for the statistics to converge. When combined
    ///   with the use of longer benchmark runs, this means that benchmarks will
    ///   take longer to execute before stable results are achieved.
    #define NUM_MEDIAN_SAMPLES ((size_t)5)
    static_assert(NUM_MEDIAN_SAMPLES % 2 == 1,
                  "Medians are computed over an odd number of samples");

    /// Desired number of measurements on either side of the confidence interval
    ///
    /// Tune this up if you observe unstable duration statistics even though the
    /// underlying duration distributions are stable.
    ///
    /// Tuning it too high will increase the overhead of the statistical
    /// analysis process for no good reason.
    //
    // TODO: Tune on more system
    #define NUM_EDGE_MEASUREMENTS ((size_t)512)

    /// Warmup duration used for OS clock offset calibration
    //
    // TODO: Tune on more systems
    #define WARMUP_OFFSET_OS (1000*UDIPE_MILLISECOND)

    /// Number of benchmark runs used for OS clock offset calibration
    ///
    /// Tune this up if clock offset calibration is unstable, as evidenced by
    /// the fact that short loops get a nonzero median duration.
    //
    // TODO: Tune on more systems
    #define NUM_RUNS_OFFSET_OS ((size_t)64*1024)

    /// Warmup duration used for shortest loop calibration
    //
    // TODO: Tune on more systems
    #define WARMUP_SHORTEST_LOOP (2000*UDIPE_MILLISECOND)

    /// Number of benchmark runs used for shortest loop calibration
    ///
    /// Tune this up if the shortest loop calibration is unstable and does not
    /// converge to a constant loop size.
    //
    // TODO: Tune on more systems
    #define NUM_RUNS_SHORTEST_LOOP ((size_t)64*1024)

    /// Warmup duration used for best loop calibration
    //
    // TODO: Tune on more systems
    #define WARMUP_BEST_LOOP (2000*UDIPE_MILLISECOND)

    /// Number of benchmark run used for optimal loop calibration, when using
    /// the system clock to perform said calibration
    ///
    /// Tune this up if the optimal loop calibration is unstable and does not
    /// converge to sufficiently reproducible statistics.
    ///
    /// Tune this down if you observe multimodal timing laws, which indicates
    /// that the CPU switches performance states during the measurement, and
    /// this state instability is not fixed by using a longer warmup or
    /// adjusting the system's power management configuration.
    //
    // TODO: Tune on more systems
    #define NUM_RUNS_BEST_LOOP_OS ((size_t)64*1024)

    #ifdef X86_64

        /// Number of benchmark runs used when measuring the duration of the
        /// optimal loop using the x86 TimeStamp Counter
        ///
        /// Tune this up if the optimal loop calibration does not yield
        /// reproducible results.
        //
        // TODO: Tune on more systems
        #define NUM_RUNS_BEST_LOOP_X86 ((size_t)8*1024)

        /// Warmup duration used for TSC clock offset calibration
        //
        // TODO: Tune on more systems
        #define WARMUP_OFFSET_X86 (1*UDIPE_MILLISECOND)

        /// Number of benchmark runs used for TSC clock offset calibration
        ///
        /// Tune this up if the TSC offset calibration does not yield
        /// reproducible results.
        //
        // TODO: Tune on more systems
        #define NUM_RUNS_OFFSET_X86 ((size_t)16*1024)

    #endif  // X86_64


    /// Comparison function for applying qsort() to int64_t[]
    static inline int compare_i64(const void* v1, const void* v2) {
        const int64_t* const d1 = (const int64_t*)v1;
        const int64_t* const d2 = (const int64_t*)v2;
        if (*d1 < *d2) return -1;
        if (*d1 > *d2) return 1;
        return 0;
    }


    // TODO: Move to other files

    stats_analyzer_t stats_analyzer_initialize(float confidence_f) {
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
        return (stats_analyzer_t){
            .medians = medians,
            .num_medians = num_medians,
            .low_idx = (size_t)(low_quantile * num_medians),
            .center_idx = num_medians / 2,
            .high_idx = (size_t)(high_quantile * num_medians)
        };
    }

    UDIPE_NON_NULL_ARGS
    stats_t stats_analyze(stats_analyzer_t* analyzer,
                          const distribution_t* dist) {
        trace("Computing medians...");
        int64_t median_samples[NUM_MEDIAN_SAMPLES];
        for (size_t median = 0; median < analyzer->num_medians; ++median) {
            tracef("- Computing medians[%zu]...", median);
            for (size_t sample = 0; sample < NUM_MEDIAN_SAMPLES; ++sample) {
                const int64_t value = distribution_choose(dist);
                tracef("  * Inserting sample %zd...", value);
                ptrdiff_t prev;
                for (prev = sample - 1; prev >= 0; --prev) {
                    int64_t pivot = median_samples[prev];
                    tracef("    - Checking median_samples[%zd] = %zd...",
                           prev, pivot);
                    if (pivot > value) {
                        trace("    - Too high, shift that up to make room.");
                        median_samples[prev + 1] = median_samples[prev];
                        continue;
                    } else {
                        trace("    - Small enough, value goes after that.");
                        break;
                    }
                }
                tracef("  * Sample inserted at median_samples[%zd].", prev + 1);
                median_samples[prev + 1] = value;
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
    void stats_analyzer_finalize(stats_analyzer_t* analyzer) {
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


    UDIPE_NON_NULL_ARGS
    void empty_loop(void* context) {
        size_t num_iters = *((const size_t*)context);
        // Ensures that all loop lengths get the same codegen
        UDIPE_ASSUME_ACCESSED(num_iters);
        for (size_t iter = 0; iter < num_iters; ++iter) {
            // This is ASSUME_ACCESSED and not ASSUME_READ because with
            // ASSUME_READ the compiler can unroll the loop and this will reduce
            // timing reproducibility with respect to the pure dependency chain
            // of a non-unrolled loop.
            UDIPE_ASSUME_ACCESSED(iter);
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
                       CONFIDENCE * 100.0,  \
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
            int udipe_uncertainty_decimals = ceil(-log10(udipe_uncertainty)) + 1;  \
            if (udipe_uncertainty_decimals < 0) udipe_uncertainty_decimals = 0;  \
            udipe_logf((level),  \
                       "%s That's %.*f %s/iter with %g%% CI [%.*f; %.*f] (%.*f%% uncertainty).",  \
                       (bullet),  \
                       udipe_stats_decimals,  \
                       udipe_center,  \
                       (unit),  \
                       CONFIDENCE * 100.0,  \
                       udipe_stats_decimals,  \
                       udipe_low,  \
                       udipe_stats_decimals,  \
                       udipe_high,  \
                       udipe_uncertainty_decimals,  \
                       udipe_uncertainty);  \
        } while(false)

    UDIPE_NON_NULL_ARGS
    distribution_t compute_duration_distribution(
        int64_t (*compute_duration)(void* /* context */,
                                    size_t /* run */),
        void* context,
        size_t num_runs,
        outlier_filter_t* outlier_filter,
        distribution_builder_t* empty_builder
    ) {
        ensure(distribution_empty(empty_builder));
        distribution_builder_t* builder = empty_builder;
        empty_builder = NULL;

        trace("Computing durations...");
        for (size_t run = 0; run < num_runs; ++run) {
            const int64_t duration = compute_duration(context, run);
            distribution_insert(builder, duration);
        }

        trace("Applying outlier filter...");
        outlier_filter_apply(outlier_filter, builder);
        if (log_enabled(UDIPE_DEBUG)) {
            distribution_log(outlier_filter_last_scores(outlier_filter),
                             UDIPE_DEBUG,
                             "Outlier filter scores");
            const distribution_t* rejections =
                outlier_filter_last_rejections(outlier_filter);
            if (rejections) {
                distribution_log(rejections,
                                 UDIPE_DEBUG,
                                 "Rejected durations");
            } else {
                debug("No duration was rejected!");
            }
        }

        trace("Finalizing accepted duration distribution");
        distribution_t result = distribution_build(builder);
        distribution_log(&result,
                         UDIPE_DEBUG,
                         "Accepted durations");

        // TODO: clean up e.g. reuse storage and deduplicate code
        analyzer_t analyzer = analyzer_initialize();
        const statistics_t stats = analyzer_apply(&analyzer, &result);
        debugf("Symmetric dispersion start is %g with %g%% CI [%g; %g]",
               stats.sym_dispersion_start.center,
               CONFIDENCE * 100.0,
               stats.sym_dispersion_start.low,
               stats.sym_dispersion_start.high);
        debugf("Low dispersion bound is %g with %g%% CI [%g; %g]",
               stats.low_dispersion_bound.center,
               CONFIDENCE * 100.0,
               stats.low_dispersion_bound.low,
               stats.low_dispersion_bound.high);
        debugf("Mean is %g with %g%% CI [%g; %g]",
               stats.mean.center,
               CONFIDENCE * 100.0,
               stats.mean.low,
               stats.mean.high);
        debugf("High dispersion bound is %g with %g%% CI [%g; %g]",
               stats.high_dispersion_bound.center,
               CONFIDENCE * 100.0,
               stats.high_dispersion_bound.low,
               stats.high_dispersion_bound.high);
        debugf("Symmetric dispersion end is %g with %g%% CI [%g; %g]",
               stats.sym_dispersion_end.center,
               CONFIDENCE * 100.0,
               stats.sym_dispersion_end.low,
               stats.sym_dispersion_end.high);
        debugf("Symmetric dispersion width is %g with %g%% CI [%g; %g]",
               stats.sym_dispersion_width.center,
               CONFIDENCE * 100.0,
               stats.sym_dispersion_width.low,
               stats.sym_dispersion_width.high);
        analyzer_finalize(&analyzer);

        return result;
    }


    UDIPE_NON_NULL_ARGS
    os_clock_t os_clock_initialize(outlier_filter_t* outlier_filter,
                                   stats_analyzer_t* analyzer) {
        // Zero out all clock fields initially
        //
        // This is a valid (if incorrect) value for some fields but not all of
        // them. We will take care of the missing fields later on.
        os_clock_t clock = { 0 };

        #ifdef _WIN32
            debug("Obtaining Windows performance counter frequency...");
            clock.win32_frequency = QueryPerformanceFrequency().QuadPart;
        #endif

        debug("Allocating timestamp buffer and duration distribution...");
        size_t max_runs = NUM_RUNS_OFFSET_OS;
        if (max_runs < NUM_RUNS_SHORTEST_LOOP) max_runs = NUM_RUNS_SHORTEST_LOOP;
        if (max_runs < NUM_RUNS_BEST_LOOP_OS) max_runs = NUM_RUNS_BEST_LOOP_OS;
        const size_t timestamps_size = (max_runs+1) * sizeof(os_timestamp_t);
        clock.timestamps = realtime_allocate(timestamps_size);
        clock.num_durations = max_runs;
        clock.builder = distribution_initialize();

        info("Bootstrapping clock offset to 0 ns...");
        distribution_insert(&clock.builder, 0);
        clock.offsets = distribution_build(&clock.builder);
        clock.builder = distribution_initialize();

        info("Measuring actual clock offset...");
        size_t num_iters = 0;
        distribution_t tmp_offsets = os_clock_measure(
            &clock,
            empty_loop,
            &num_iters,
            WARMUP_OFFSET_OS,
            NUM_RUNS_OFFSET_OS,
            outlier_filter,
            &clock.builder
        );
        clock.builder = distribution_reset(&clock.offsets);
        clock.offsets = tmp_offsets;
        distribution_poison(&tmp_offsets);
        const stats_t offset_stats = stats_analyze(analyzer, &clock.offsets);
        log_calibration_stats(UDIPE_INFO, "- Clock offset", offset_stats, "ns");

        info("Deducing clock baseline...");
        distribution_t tmp_zeros = distribution_sub(&clock.builder,
                                                    &clock.offsets,
                                                    &clock.offsets);
        const stats_t zero_stats = stats_analyze(analyzer, &tmp_zeros);
        clock.builder = distribution_reset(&tmp_zeros);
        log_calibration_stats(UDIPE_INFO,
                              "- Baseline",
                              zero_stats,
                              "ns");

        info("Finding minimal measurable loop...");
        distribution_t loop_durations;
        stats_t loop_duration_stats;
        num_iters = 1;
        do {
            debugf("- Trying loop with %zu iteration(s)...", num_iters);
            loop_durations = os_clock_measure(
                &clock,
                empty_loop,
                &num_iters,
                WARMUP_SHORTEST_LOOP,
                NUM_RUNS_SHORTEST_LOOP,
                outlier_filter,
                &clock.builder
            );
            loop_duration_stats = stats_analyze(analyzer, &loop_durations);
            log_calibration_stats(UDIPE_DEBUG,
                                  "  * Loop duration",
                                  loop_duration_stats,
                                  "ns");
            const signed_duration_ns_t loop_duration_spread =
                loop_duration_stats.high - loop_duration_stats.low;
            if (loop_duration_stats.low < 9*offset_stats.high) {
                debug("  * Clock contribution may still be >10%...");
            } else if(loop_duration_stats.low < 10*loop_duration_spread) {
                debug("  * Duration may still fluctuates by >10%...");
            } else {
                debug("  * That's long enough and stable enough.");
                clock.builder = distribution_initialize();
                break;
            }
            // If control reaches here, must still increase loop size
            num_iters *= 2;
            clock.builder = distribution_reset(&loop_durations);
        } while(true);
        infof("- Loops with >=%zu iterations have non-negligible duration.",
              num_iters);

        info("Finding optimal loop duration...");
        clock.best_empty_iters = num_iters;
        clock.best_empty_durations = loop_durations;
        distribution_poison(&loop_durations);
        clock.best_empty_stats = loop_duration_stats;
        const int64_t best_precision = loop_duration_stats.high - loop_duration_stats.low;
        double best_uncertainty = relative_uncertainty(loop_duration_stats);
        do {
            num_iters *= 2;
            debugf("- Trying loop with %zu iterations...", num_iters);
            loop_durations = os_clock_measure(
                &clock,
                empty_loop,
                &num_iters,
                WARMUP_BEST_LOOP,
                NUM_RUNS_BEST_LOOP_OS,
                outlier_filter,
                &clock.builder
            );
            loop_duration_stats = stats_analyze(analyzer, &loop_durations);
            log_calibration_stats(UDIPE_DEBUG,
                                  "  * Loop duration",
                                  loop_duration_stats,
                                  "ns");
            log_iteration_stats(UDIPE_DEBUG,
                                "  *",
                                loop_duration_stats,
                                num_iters,
                                "ns");
            const double uncertainty = relative_uncertainty(loop_duration_stats);
            const int64_t precision = loop_duration_stats.high - loop_duration_stats.low;
            // In a regime of stable run timing precision, doubling the
            // iteration count should improve iteration timing uncertainty by
            // 2x. Ignore small improvements that don't justify a 2x longer run
            // duration, and thus fewer runs per unit of execution time...
            if (uncertainty < best_uncertainty/1.1) {
                debug("  * This is our new best loop. Can we do even better?");
                best_uncertainty = uncertainty;
                clock.best_empty_iters = num_iters;
                clock.builder = distribution_reset(&clock.best_empty_durations);
                clock.best_empty_durations = loop_durations;
                distribution_poison(&loop_durations);
                clock.best_empty_stats = loop_duration_stats;
                continue;
            } else if (precision <= 3*best_precision) {
                // ...but keep trying until the uncertainty degradation becomes
                // much worse than expected in a regime of stable iteration
                // timing uncertainty, in which case loop duration fluctuates 2x
                // more when loop iteration gets 2x higher.
                debug("  * That's not much better/worse, keep trying...");
                clock.builder = distribution_reset(&loop_durations);
                continue;
            } else {
                debug("  * Absolute precision degraded by >3x: time to stop!");
                clock.builder = distribution_reset(&loop_durations);
                break;
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

    /// compute_duration_distribution() callback used by os_clock_measure()
    ///
    /// `context` must be a pointer to the associated \ref os_clock_t.
    static inline int64_t compute_os_duration(void* context,
                                              size_t run) {
        os_clock_t* clock = (os_clock_t*)context;
        assert(run < clock->num_durations);
        return os_duration(clock,
                           clock->timestamps[run],
                           clock->timestamps[run+1]);
    }

    UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 6)
    distribution_t os_clock_measure(
        os_clock_t* clock,
        void (*workload)(void*),
        void* context,
        udipe_duration_ns_t warmup,
        size_t num_runs,
        outlier_filter_t* outlier_filter,
        distribution_builder_t* empty_builder
    ) {
        if (num_runs > clock->num_durations) {
            trace("Reallocating storage from %zu to %zu durations...");
            realtime_liberate(
                clock->timestamps,
                (clock->num_durations+1) * sizeof(os_timestamp_t)
            );
            clock->num_durations = num_runs;
            clock->timestamps =
                realtime_allocate((num_runs+1) * sizeof(os_timestamp_t));
        }

        trace("Warming up...");
        os_timestamp_t* timestamps = clock->timestamps;
        udipe_duration_ns_t elapsed = 0;
        os_timestamp_t start = os_now();
        do {
            workload(context);
            os_timestamp_t now = os_now();
            elapsed = os_duration(clock, start, now);
        } while(elapsed < warmup);

        tracef("Performing %zu timed runs...", num_runs);
        timestamps[0] = os_now();
        for (size_t run = 0; run < num_runs; ++run) {
            UDIPE_ASSUME_READ(timestamps);
            workload(context);
            timestamps[run+1] = os_now();
        }
        UDIPE_ASSUME_READ(timestamps);

        trace("Computing duration distribution...");
        return compute_duration_distribution(compute_os_duration,
                                             (void*)clock,
                                             num_runs,
                                             outlier_filter,
                                             empty_builder);
    }

    UDIPE_NON_NULL_ARGS
    void os_clock_finalize(os_clock_t* clock) {
        debug("Liberating and poisoning timestamp storage...");
        realtime_liberate(clock->timestamps,
                          (clock->num_durations+1) * sizeof(os_timestamp_t));
        clock->timestamps = NULL;
        clock->num_durations = 0;

        debug("Destroying duration distributions...");
        distribution_finalize(&clock->offsets);
        distribution_finalize(&clock->best_empty_durations);
        distribution_finalize(&clock->builder.inner);

        debug("Poisoning the rest of the OS clock...");
        #ifdef _WIN32
            clock->win32_frequency = 0;
        #endif
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
        x86_clock_initialize(outlier_filter_t* outlier_filter,
                             os_clock_t* os,
                             stats_analyzer_t* analyzer) {
            // Zero out all clock fields initially
            //
            // This is a valid (if incorrect) value for some fields but not all
            // of them. We will take care of the missing fields later on.
            x86_clock_t clock = { 0 };

            debug("Allocating timestamp and duration distribution...");
            size_t max_runs = NUM_RUNS_BEST_LOOP_X86;
            if (max_runs < NUM_RUNS_OFFSET_X86) max_runs = NUM_RUNS_OFFSET_X86;
            const size_t instants_size = 2 * max_runs * sizeof(x86_instant);
            clock.instants = realtime_allocate(instants_size);
            clock.num_durations = max_runs;
            distribution_builder_t builder = distribution_initialize();

            info("Bootstrapping clock offset to 0 ticks...");
            distribution_insert(&builder, 0);
            clock.offsets = distribution_build(&builder);
            builder = distribution_initialize();

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
            distribution_t raw_empty_ticks = x86_clock_measure(
                &clock,
                empty_loop,
                &best_empty_iters,
                WARMUP_BEST_LOOP,
                NUM_RUNS_BEST_LOOP_X86,
                outlier_filter,
                &builder
            );
            log_calibration_stats(UDIPE_INFO,
                                  "- Offset-biased best loop",
                                  stats_analyze(analyzer, &raw_empty_ticks),
                                  "ticks");

            info("Measuring clock offset...");
            builder = distribution_initialize();
            size_t empty_loop_iters = 0;
            distribution_t tmp_offsets = x86_clock_measure(
                &clock,
                empty_loop,
                &empty_loop_iters,
                WARMUP_OFFSET_X86,
                NUM_RUNS_OFFSET_X86,
                outlier_filter,
                &builder
            );
            builder = distribution_reset(&clock.offsets);
            clock.offsets = tmp_offsets;
            distribution_poison(&tmp_offsets);
            log_calibration_stats(UDIPE_INFO,
                                  "- Clock offset",
                                  stats_analyze(analyzer, &clock.offsets),
                                  "ticks");

            info("Deducing clock baseline...");
            distribution_t tmp_zeros = distribution_sub(&builder,
                                                        &clock.offsets,
                                                        &clock.offsets);
            const stats_t zero_stats = stats_analyze(analyzer, &tmp_zeros);
            builder = distribution_reset(&tmp_zeros);
            log_calibration_stats(UDIPE_INFO,
                                  "- Baseline",
                                  zero_stats,
                                  "ticks");

            debug("Applying offset correction to best loop duration...");
            distribution_t corrected_empty_ticks = distribution_sub(
                &builder,
                &raw_empty_ticks,
                &clock.offsets
            );
            builder = distribution_reset(&raw_empty_ticks);
            clock.best_empty_stats = stats_analyze(analyzer,
                                                   &corrected_empty_ticks);
            log_calibration_stats(UDIPE_DEBUG,
                                  "- Offset-corrected best loop",
                                  clock.best_empty_stats,
                                  "ticks");
            log_iteration_stats(UDIPE_DEBUG,
                                "-",
                                clock.best_empty_stats,
                                os->best_empty_iters,
                                "ticks");

            info("Deducing TSC tick frequency...");
            clock.frequencies = distribution_scaled_div(
                &builder,
                &corrected_empty_ticks,
                UDIPE_SECOND,
                &os->best_empty_durations
            );
            // `builder` cannot be used after this point
            log_calibration_stats(UDIPE_INFO,
                                  "- TSC frequency",
                                  stats_analyze(analyzer, &clock.frequencies),
                                  "ticks/sec");

            debug("Deducing best loop duration...");
            const stats_t best_empty_duration = x86_duration(
                &clock,
                &os->builder,
                &corrected_empty_ticks,
                analyzer
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

        /// compute_duration_distribution() context used by x86_clock_measure()
        ///
        typedef struct x86_measure_context_s {
            x86_clock_t* clock;  ///< x86 clock used for the measurement
            size_t num_runs;  ///< Number of benchmark runs
        } x86_measure_context_t;

        /// compute_duration_distribution() callback used by x86_clock_measure()
        ///
        /// `context` must be a pointer to the associated \ref
        /// x86_measure_context_t.
        static inline int64_t compute_x86_duration(void* context,
                                                   size_t run) {
            x86_measure_context_t* measure = (x86_measure_context_t*)context;
            x86_clock_t* clock = measure->clock;
            assert(run < measure->num_runs);
            assert(measure->num_runs < clock->num_durations);
            x86_instant* starts = clock->instants;
            x86_instant* ends = starts + measure->num_runs;
            const int64_t raw_ticks = ends[run] - starts[run];
            return raw_ticks - distribution_choose(&clock->offsets);
        }

        UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 6)
        distribution_t x86_clock_measure(
            x86_clock_t* xclock,
            void (*workload)(void*),
            void* context,
            udipe_duration_ns_t warmup,
            size_t num_runs,
            outlier_filter_t* outlier_filter,
            distribution_builder_t* empty_builder
        ) {
            if (num_runs > xclock->num_durations) {
                trace("Reallocating storage from %zu to %zu durations...");
                realtime_liberate(
                    xclock->instants,
                    2 * xclock->num_durations * sizeof(x86_instant)
                );
                xclock->num_durations = num_runs;
                xclock->instants =
                    realtime_allocate(2 * num_runs * sizeof(x86_instant));
            }

            trace("Setting up measurement...");
            x86_instant* starts = xclock->instants;
            x86_instant* ends = xclock->instants + num_runs;
            const bool strict = false;
            x86_timestamp_t timestamp = x86_timer_start(strict);
            const x86_cpu_id initial_cpu_id = timestamp.cpu_id;

            trace("Warming up...");
            udipe_duration_ns_t elapsed = 0;
            clock_t start = clock();
            do {
                timestamp = x86_timer_start(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                UDIPE_ASSUME_READ(timestamp.ticks);

                workload(context);

                timestamp = x86_timer_end(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                UDIPE_ASSUME_READ(timestamp.ticks);

                clock_t now = clock();
                elapsed = (udipe_duration_ns_t)(now - start)
                          * UDIPE_SECOND / CLOCKS_PER_SEC;
            } while(elapsed < warmup);

            tracef("Performing %zu timed runs...", num_runs);
            for (size_t run = 0; run < num_runs; ++run) {
                timestamp = x86_timer_start(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                starts[run] = timestamp.ticks;
                UDIPE_ASSUME_READ(starts);

                workload(context);

                timestamp = x86_timer_end(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                ends[run] = timestamp.ticks;
                UDIPE_ASSUME_READ(ends);
            }

            trace("Computing duration distribution...");
            x86_measure_context_t measure_context = {
                .clock = xclock,
                .num_runs = num_runs
            };
            return compute_duration_distribution(compute_x86_duration,
                                                 (void*)&measure_context,
                                                 num_runs,
                                                 outlier_filter,
                                                 empty_builder);
        }

        UDIPE_NON_NULL_ARGS
        stats_t x86_duration(x86_clock_t* clock,
                             distribution_builder_t* tmp_builder,
                             const distribution_t* ticks,
                             stats_analyzer_t* analyzer) {
            distribution_t tmp_durations =
                distribution_scaled_div(tmp_builder,
                                        ticks,
                                        UDIPE_SECOND,
                                        &clock->frequencies);
            const stats_t result = stats_analyze(analyzer, &tmp_durations);
            *tmp_builder = distribution_reset(&tmp_durations);
            return result;
        }

        UDIPE_NON_NULL_ARGS
        void x86_clock_finalize(x86_clock_t* clock) {
            debug("Liberating and poisoning timestamp storage...");
            realtime_liberate(clock->instants,
                              2 * clock->num_durations * sizeof(x86_instant));
            clock->instants = NULL;
            clock->num_durations = 0;

            debug("Destroying offset and frequency distributions...");
            distribution_finalize(&clock->offsets);
            distribution_finalize(&clock->frequencies);

            debug("Poisoning the now-invalid TSC clock...");
            clock->best_empty_stats = (stats_t){
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

        debug("Setting up outlier filtering...");
        clock.outlier_filter = outlier_filter_initialize();

        debug("Setting up statistical analysis...");
        clock.analyzer = stats_analyzer_initialize(CONFIDENCE * 100.0);

        info("Setting up the OS clock...");
        clock.os = os_clock_initialize(&clock.outlier_filter,
                                       &clock.analyzer);

        #ifdef X86_64
            info("Setting up the TSC clock...");
            clock.x86 = x86_clock_initialize(&clock.outlier_filter,
                                             &clock.os,
                                             &clock.analyzer);
        #endif
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

        #ifdef X86_64
            debug("Destroying the TSC clock...");
            x86_clock_finalize(&clock->x86);
        #endif

        debug("Destroying the OS clock...");
        os_clock_finalize(&clock->os);

        debug("Destroying the statistical analyzer...");
        stats_analyzer_finalize(&clock->analyzer);

        debug("Destroying the outlier filter...");
        outlier_filter_finalize(&clock->outlier_filter);
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
                warn("You are running micro-benchmarks on a Debug build. "
                     "This will bias measurements!");
            #else
                if (benchmark->logger.min_level <= UDIPE_DEBUG) {
                    warn("You are running micro-benchmarks with DEBUG/TRACE "
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
                             const char name[],
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