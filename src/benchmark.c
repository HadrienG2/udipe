#ifdef UDIPE_BUILD_BENCHMARKS

    #include "benchmark.h"

    #include <udipe/log.h>
    #include <udipe/pointer.h>

    #include "memory.h"
    #include "visibility.h"

    #include <assert.h>
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

    /// Confidence interval used for final measurements
    ///
    /// Picked because 95% is kinda the standard in statistics, so it is what
    /// the end user will most likely be used to.
    #define RESULT_CONFIDENCE 95.0

    /// Confidence interval used for clock calibration
    ///
    /// Setting this much tighter than \ref RESULT_CONFIDENCE ensures that if
    /// the calibration deviates a bit from its optimal value, it will have a
    /// smaller impact on the end results.
    #define CALIBRATION_CONFIDENCE 99.0

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
    #define NUM_RUNS_OFFSET ((size_t)32*1024)

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

    /// Number of benchmark run used for optimal loop calibration
    ///
    /// This should be tuned high enough that the optimal loop calibration
    /// consistently ends at the same number of loop iterations and reaches
    /// similar precision figures of merit.
    //
    // TODO: Current value is the minimum required run count needed for
    //       reproducibility on the author's laptop. This minimum should be
    //       tuned up through testing on a more diverse set of systems, then
    //       once the pool of available testing systems is exhausted, some extra
    //       safety margin (maybe 2-4x?) should be applied on top of the final
    //       result to account for unknown systems.
    #define NUM_RUNS_OPTIMAL_LOOP ((size_t)8*1024)

    /// Comparison function for applying qsort() to int64_t[]
    static inline int compare_i64(const void* v1, const void* v2) {
        const int64_t* d1 = (const int64_t*)v1;
        const int64_t* d2 = (const int64_t*)v2;
        if (*d1 < *d2) return -1;
        if (*d1 > *d2) return 1;
        return 0;
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
                             int64_t data[],
                             size_t data_len) {
        trace("Checking dataset...");
        ensure_gt(data_len, (size_t)0);

        trace("Computing medians...");
        int64_t median_samples[NUM_MEDIAN_SAMPLES];
        for (size_t median = 0; median < analyzer->num_medians; ++median) {
            tracef("- Computing medians[%zu]...", median);
            for (size_t sample = 0; sample < NUM_MEDIAN_SAMPLES; ++sample) {
                size_t data_idx = rand() % data_len;
                int64_t data_point = data[data_idx];
                tracef("  * Picked %zu-th sample data[%zu] = %zd, inserting...",
                       sample, data_idx, data_point);

                ptrdiff_t prev;
                for (prev = sample - 1; prev >= 0; --prev) {
                    int64_t pivot = median_samples[prev];
                    tracef("    - Checking median_samples[%zd] = %zd...",
                           prev, pivot);
                    if (pivot > data_point) {
                        trace("    - Too high, shift that up to make room.");
                        median_samples[prev + 1] = median_samples[prev];
                        continue;
                    } else {
                        trace("    - Small enough, sample goes after that.");
                        break;
                    }
                }
                tracef("  * Sample inserted at median_samples[%zd].", prev + 1);
                median_samples[prev + 1] = data_point;
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

    /// Measure the execution duration of `workload` using the OS clock
    ///
    /// This call `workload` repeatedly `num_runs` times with timing calls
    /// interleaved between each call. Usual micro-benchmarking precautions must
    /// be taken to avoid compiler over-optimization:
    ///
    /// - If `workload` always processes the same inputs, then
    ///   UDIPE_ASSUME_ACCESSED() should be used to make the compiler assume
    ///   that these inputs change from one execution to another.
    /// - If `workload` emits outputs, then UDIPE_ASSUME_READ() should be used
    ///   to make the compiler assume that these outputs are being used.
    /// - If `workload` is just an artificial empty loop (as used during
    ///   calibration), then UDIPE_ASSUME_READ() should be used on the loop
    ///   counter to preserve the number of loop iterations.
    ///
    /// `num_runs` controls how many timed calls to `workload` will occur, it
    /// should be tuned such that...
    ///
    /// - Output timestamps fit in `timestamps` and output durations fit in
    ///   `durations`.
    /// - Results are reproducible enough across benchmark executions (what
    ///   constitutes "reproducible enough" is context dependent, a parameter
    ///   autotuning loop can typically work with less steady timing data than
    ///   the final benchmark measurement).
    /// - Execution time, which grows roughly linearly with `num_runs`,
    ///   remains reasonable.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param clock is the benchmark clock that is going to be used. This
    ///              routine can be used before said clock is fully initialized,
    ///              but it must be at minimum initialized enough to allow for
    ///              offset-biased OS clock measurements (i.e. on Windows
    ///              `win32_frequency` must have been queried already, and on
    ///              all OSes `os_offset` must be zeroed out if not known yet).
    /// \param workload is the workload whose duration should be measured. For
    ///                 minimal overhead/bias, it should be inlinable by the
    ///                 compiler static inline function.
    /// \param context encodes the parameters that should be passed to
    ///                `workload`, if any.
    /// \param timestamps should point to an array of at least `num_runs+1`
    ///                   \ref os_timestamp_t, ideally allocated using
    ///                   realtime_allocate().
    /// \param durations should point to an array of at least `num_runs`
    ///                  \ref signed_durations_ns_t, ideally allocated using
    ///                  realtime_allocate().
    /// \param num_runs indicates how many timed calls to `workload` should
    ///                      be performed, see above for tuning advice.
    /// \param analyzer is a duration analyzer that has been previously set up
    ///                 via duration_analyzer_initialize() and hasn't been
    ///                 destroyed via duration_analyzer_finalize() yet.
    UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 4, 5, 7)
    static inline stats_t measure_os_duration(
        benchmark_clock_t* clock,
        void (*workload)(void*),
        void* context,
        os_timestamp_t timestamps[],
        signed_duration_ns_t durations[],
        size_t num_runs,
        duration_analyzer_t* analyzer
    ) {
        tracef("Performing %zu timed runs...", num_runs);
        timestamps[0] = os_now();
        for (size_t run = 0; run < num_runs; ++run) {
            UDIPE_ASSUME_READ(timestamps);
            workload(context);
            timestamps[run+1] = os_now();
        }
        trace("Computing run durations...");
        for (size_t run = 0; run < num_runs; ++run) {
            durations[run] = os_duration(clock,
                                         timestamps[run],
                                         timestamps[run+1]);
            tracef("- durations[%zu] = %zd ns", run, durations[run]);
        }
        trace("Analyzing durations...");
        return analyze_duration(analyzer, durations, num_runs);
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
            const int udipe_stats_decimals = ceil(1.0 - log10(udipe_spread));  \
            const double udipe_uncertainty = relative_uncertainty(udipe_stats);  \
            int udipe_uncertainty_decimals = ceil(1.0 - log10(udipe_uncertainty));  \
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

    benchmark_clock_t benchmark_clock_initialize() {
        // Zero out all clock fields initially
        //
        // This is a valid (if incorrect) value for some fields but not all of
        // them. We will take care of the missing fields later on.
        benchmark_clock_t clock = { 0 };

        // On Windows, we must check the performance counter frequency before we
        // can do any precision time measurement with QueryPerformanceCounter().
        #ifdef _WIN32
            debug("Obtaining Windows performance counter frequency...");
            clock.win32_frequency = QueryPerformanceFrequency().QuadPart;
        #endif

        debug("Setting up calibration analysis...");
        clock.calibration_analyzer =
            duration_analyzer_initialize(CALIBRATION_CONFIDENCE);

        debug("Allocating OS clock timestamp and duration buffers...");
        size_t max_runs = NUM_RUNS_OFFSET;
        if (max_runs < NUM_RUNS_SHORTEST_LOOP) max_runs = NUM_RUNS_SHORTEST_LOOP;
        if (max_runs < NUM_RUNS_OPTIMAL_LOOP) max_runs = NUM_RUNS_OPTIMAL_LOOP;
        const size_t timestamps_size = (max_runs+1) * sizeof(os_timestamp_t);
        const size_t durations_size = max_runs * sizeof(signed_duration_ns_t);
        os_timestamp_t* timestamps = realtime_allocate(timestamps_size);
        signed_duration_ns_t* durations = realtime_allocate(durations_size);

        info("Calibrating OS clock offset...");
        const stats_t os_offset = measure_os_duration(
            &clock,
            nothing,
            NULL,
            timestamps,
            durations,
            NUM_RUNS_OFFSET,
            &clock.calibration_analyzer
        );
        log_calibration_stats(UDIPE_INFO,
                              "- OS clock offset",
                              os_offset,
                              "ns");
        clock.os_offset = os_offset.center;
        clock.best_precision = os_offset.high - os_offset.low;
        infof("- Offset correction will have %g%% CI [%+zd; %+zd].",
              CALIBRATION_CONFIDENCE,
              os_offset.low-os_offset.center,
              os_offset.high-os_offset.center);

        info("Finding minimal measurable loop...");
        stats_t loop_duration;
        size_t num_iters = 1;
        do {
            debugf("- Trying loop with %zu iteration(s)...", num_iters);
            loop_duration = measure_os_duration(
                &clock,
                empty_loop,
                &num_iters,
                timestamps,
                durations,
                NUM_RUNS_SHORTEST_LOOP,
                &clock.calibration_analyzer
            );
            log_calibration_stats(UDIPE_DEBUG,
                                  "  * Loop duration",
                                  loop_duration,
                                  "ns");
            if (loop_duration.center > (int64_t)clock.os_offset) {
                debug("  * Loop finally contributes more to measurements than the clock offset!");
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
        stats_t best_duration = loop_duration;
        double best_uncertainty = relative_uncertainty(loop_duration);
        size_t best_iters = num_iters;
        do {
            num_iters *= 2;
            debugf("- Trying loop with %zu iterations...", num_iters);
            loop_duration = measure_os_duration(
                &clock,
                empty_loop,
                &num_iters,
                timestamps,
                durations,
                NUM_RUNS_OPTIMAL_LOOP,
                &clock.calibration_analyzer
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
            if (loop_duration.high - loop_duration.low > 2*(os_offset.high - os_offset.low)) {
                debug("  * Timing precision degraded by >2x: time to stop!");
                break;
            } else if (uncertainty < best_uncertainty) {
                debug("  * This is our new best loop!");
                best_iters = num_iters;
                best_uncertainty = uncertainty;
                best_duration = loop_duration;
                continue;
            } else {
                debug("  * That's not much worse, keep trying...");
                continue;
            }
        } while(true);
        infof("- Achieved optimal precision at %zu loop iterations.", best_iters);
        log_calibration_stats(UDIPE_INFO,
                              "- Best loop duration",
                              best_duration,
                              "ns");
        log_iteration_stats(UDIPE_INFO,
                            "-",
                            best_duration,
                            best_iters,
                            "ns");
        clock.longest_optimal_duration = best_duration.low;

        // TODO: WIP x86 TSC calibration, once ready extract it into its own
        //       function guarded with #ifdef X86_64
        // TODO: Move to toplevel and tune
        // TODO: Use different values for different stages of the calibration
        //       process
        #define NUM_RUNS_TSC ((size_t)128*1024)
        // TODO: Dynamically allocate with realtime_allocate() after max with
        //       other TSC allocation sizes
        x86_timestamp_start starts[NUM_RUNS_TSC+1];
        x86_timestamp_end ends[NUM_RUNS_TSC];
        x86_duration_ticks ticks[NUM_RUNS_TSC];
        info("Measuring optimal loop with the TSC...");
        debugf("- Performing %zu timed runs...", NUM_RUNS_TSC);
        starts[0] = x86_timer_start(false);
        for (size_t run = 0; run < NUM_RUNS_TSC; ++run) {
            UDIPE_ASSUME_READ(starts);
            empty_loop(&best_iters);
            ends[run] = x86_timer_end(false);
            UDIPE_ASSUME_READ(ends);
            starts[run+1] = x86_timer_start(false);
        }
        debug("- Analyzing CPU migrations & durations...");
        x86_cpu_id prev_start_cpu = starts[0].cpu_id;
        size_t num_internal_migrations = 0;
        size_t num_external_migrations = 0;
        size_t num_valid_ticks = 0;
        for (size_t run = 0; run < NUM_RUNS_TSC; ++run) {
            const bool migrated = (ends[run].cpu_id != prev_start_cpu);
            if (migrated) {
                tracef("  * Skipping run %zu due to CPU%u -> CPU%u migration",
                       run, prev_start_cpu, ends[run].cpu_id);
            } else {
                const size_t idx = ++num_valid_ticks;
                ticks[idx] = ends[run].ticks - starts[run].ticks;
                tracef("  * ticks[%zu] = %zd", idx, ticks[idx]);
            }
            num_internal_migrations += migrated;
            num_external_migrations += (starts[run+1].cpu_id != ends[run].cpu_id);
            prev_start_cpu = starts[run+1].cpu_id;
        }
        debugf("- Got %zu migrations during runs, %zu between runs",
               num_internal_migrations, num_external_migrations);
        // TODO: Handle case where there are too many migrations
        debug("- Analyzing durations...");
        const stats_t best_ticks =
            analyze_duration(&clock.calibration_analyzer,
                             ticks,
                             num_valid_ticks);
        log_calibration_stats(UDIPE_INFO,
                              "- Best loop duration",
                              best_ticks,
                              "ticks");

        // TODO: Deduplicate with above code
        // FIXME: Don't use a half-length loop for offset calibration when an
        //        empty loop will get us to the desired result quicker and also
        //        provide us with the precision in ticks.
        info("Calibrating TSC offset & frequency with half-length loop...");
        debugf("- Performing %zu timed runs...", NUM_RUNS_TSC);
        starts[0] = x86_timer_start(false);
        size_t half_iters = best_iters / 2;
        for (size_t run = 0; run < NUM_RUNS_TSC; ++run) {
            UDIPE_ASSUME_READ(starts);
            empty_loop(&half_iters);
            ends[run] = x86_timer_end(false);
            UDIPE_ASSUME_READ(ends);
            starts[run+1] = x86_timer_start(false);
        }
        debug("- Analyzing CPU migrations & durations...");
        prev_start_cpu = starts[0].cpu_id;
        num_internal_migrations = 0;
        num_external_migrations = 0;
        num_valid_ticks = 0;
        for (size_t run = 0; run < NUM_RUNS_TSC; ++run) {
            const bool migrated = (ends[run].cpu_id != prev_start_cpu);
            if (migrated) {
                tracef("  * Skipping run %zu due to CPU%u -> CPU%u migration",
                       run, prev_start_cpu, ends[run].cpu_id);
            } else {
                const size_t idx = ++num_valid_ticks;
                ticks[idx] = ends[run].ticks - starts[run].ticks;
                tracef("  * ticks[%zu] = %zd", idx, ticks[idx]);
            }
            num_internal_migrations += migrated;
            num_external_migrations += (starts[run+1].cpu_id != ends[run].cpu_id);
            prev_start_cpu = starts[run+1].cpu_id;
        }
        debugf("- Got %zu migrations during runs, %zu between runs",
               num_internal_migrations, num_external_migrations);
        // TODO: Handle case where there are too many migrations
        debug("- Analyzing durations...");
        const stats_t half_ticks =
            analyze_duration(&clock.calibration_analyzer,
                             ticks,
                             num_valid_ticks);
        log_calibration_stats(UDIPE_INFO,
                              "- Half-loop duration",
                              half_ticks,
                              "ticks");
        const stats_t tsc_offset = (stats_t){
            .center = 2*half_ticks.center - best_ticks.center,
            .low = 2*half_ticks.low - best_ticks.high,
            .high = 2*half_ticks.high - best_ticks.low
        };
        log_calibration_stats(UDIPE_INFO,
                              "- Deduced timing offset",
                              tsc_offset,
                              "ticks");
        clock.tsc_offset = tsc_offset.center;
        const stats_t corrected_best_ticks = (stats_t){
            .center = best_ticks.center - tsc_offset.center,
            .low = best_ticks.low - tsc_offset.high,
            .high = best_ticks.high - tsc_offset.low
        };
        log_calibration_stats(UDIPE_INFO,
                              "- Corrected best loop duration",
                              corrected_best_ticks,
                              "ticks");
        const stats_t tsc_freq = (stats_t){
            .center = corrected_best_ticks.center * 1000*1000*1000 / best_duration.center,
            .low = corrected_best_ticks.low * 1000*1000*1000 / best_duration.high,
            .high = corrected_best_ticks.high * 1000*1000*1000 / best_duration.low
        };
        log_calibration_stats(UDIPE_INFO,
                              "- TSC frequency",
                              tsc_freq,
                              "ticks/sec");

        // TODO: Deduplicate with above code
        // TODO: Only run when debug logging is enabled
        debug("Revisiting smaller loops with the TSC...");
        num_iters = best_iters;
        do {
            debugf("- Trying loop with %zu iterations...", num_iters);
            debugf("  * Performing %zu timed runs...", NUM_RUNS_TSC);
            starts[0] = x86_timer_start(false);
            for (size_t run = 0; run < NUM_RUNS_TSC; ++run) {
                UDIPE_ASSUME_READ(starts);
                empty_loop(&num_iters);
                ends[run] = x86_timer_end(false);
                UDIPE_ASSUME_READ(ends);
                starts[run+1] = x86_timer_start(false);
            }
            debug("  * Analyzing CPU migrations & durations...");
            prev_start_cpu = starts[0].cpu_id;
            num_internal_migrations = 0;
            num_external_migrations = 0;
            num_valid_ticks = 0;
            for (size_t run = 0; run < NUM_RUNS_TSC; ++run) {
                const bool migrated = (ends[run].cpu_id != prev_start_cpu);
                if (migrated) {
                    tracef("    - Skipping run %zu due to CPU%u -> CPU%u migration",
                           run, prev_start_cpu, ends[run].cpu_id);
                } else {
                    const size_t idx = ++num_valid_ticks;
                    ticks[idx] = ends[run].ticks - starts[run].ticks - clock.tsc_offset;
                    tracef("    - ticks[%zu] = %zd", idx, ticks[idx]);
                }
                num_internal_migrations += migrated;
                num_external_migrations += (starts[run+1].cpu_id != ends[run].cpu_id);
                prev_start_cpu = starts[run+1].cpu_id;
            }
            debugf("  * Got %zu migrations during runs, %zu between runs",
                   num_internal_migrations, num_external_migrations);
            // TODO: Handle case where there are too many migrations
            debug("  * Analyzing durations...");
            const stats_t loop_ticks =
                analyze_duration(&clock.calibration_analyzer,
                                 ticks,
                                 num_valid_ticks);
            loop_duration = (stats_t){
                .center = loop_ticks.center * 1000*1000*1000 / tsc_freq.center,
                .low = loop_ticks.low * 1000*1000*1000 / tsc_freq.high,
                .high = loop_ticks.high * 1000*1000*1000 / tsc_freq.low
            };
            log_calibration_stats(UDIPE_DEBUG,
                                  "  * Loop duration",
                                  loop_duration,
                                  "ns");
            log_iteration_stats(UDIPE_DEBUG,
                                "  *",
                                loop_duration,
                                num_iters,
                                "ns");
            if (loop_ticks.center <= clock.tsc_offset) {
                debug("  * This is getting below the offset, stop here.");
                break;
            } else {
                debug("  * That still seems within significant range, keep going...");
                if (num_iters == 0) break;
                num_iters /= 2;
            }
        } while(true);

        // TODO: Once done, extract OS clock calibration and TSC calibration
        //       into their own functions.
        // TODO: Consider keeping around the optimal loop iteration count to
        //       speed up future recalibrations, which may then become...
        //       - Measure previously optimal loop
        //       - Tune iteration count up and down by 2x, pick the
        //         configuration with optimal timing precision, and if it's not
        //         the previous one then tune further by 2x steps in adjustment
        //         direction until optimal precision is achieved.
        //       - Adjust clock precision figure of merit and longest optimal
        //         benchmark run duration if needed.
        //       - Take iteration count 2x smaller than optimal and use simple
        //         affine model to compute the clock offset (2*short - long =
        //         offset), apply a correction if it is not zero as it should.

        // TODO: Remove this exit once the above is done
        exit(EXIT_FAILURE);

        // TODO: Consider keeping those around along with any other data needed
        //       for future recalibration.
        debug("Liberating OS clock timestamp and duration buffers...");
        realtime_liberate(timestamps, timestamps_size);
        realtime_liberate(durations, durations_size);

        return clock;
    }

    UDIPE_NON_NULL_ARGS
    void benchmark_clock_recalibrate(benchmark_clock_t* clock) {
        // TODO: Check if clock calibration still seems correct, recalibrate if
        //       needed.
        error("Not implemented yet!");
        exit(EXIT_FAILURE);
    }

    UDIPE_NON_NULL_ARGS
    void benchmark_clock_finalize(benchmark_clock_t* clock) {
        debug("Poisoning the now-invalid benchmark clock...");
        #ifdef X86_64
            clock->tsc_frequency = 0;
            clock->tsc_offset = UDIPE_DURATION_MAX;
        #endif
        #ifdef _WIN32
            clock->win32_frequency = 0;
        #endif
        clock->os_offset = UDIPE_DURATION_MAX;
        clock->best_precision = UDIPE_DURATION_MAX;
        clock->longest_optimal_duration = 0;
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
            // Our goal is to fill up this struct
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

            // Set up name-based benchmark filtering
            debug("Setting up benchmark name filter...");
            ensure_le(argc, 2);
            const char* filter_key = (argc == 2) ? argv[1] : "";
            benchmark->filter = name_filter_initialize(filter_key);

            // TODO: Pin to one CPU hyperthread to avoid CPU migrations, which
            //       complicate TSC measurement. Then simplify the TSC code
            //       accordingly.

            // Set up the benchmark clock
            benchmark->clock = benchmark_clock_initialize();
        });
        return benchmark;
    }

    DEFINE_PUBLIC
    UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 3)
    bool udipe_benchmark_run(udipe_benchmark_t* benchmark,
                             const char* name,
                             udipe_callable_t callable,
                             void* context) {
        bool matches;
        with_logger(&benchmark->logger, {
            matches = name_filter_matches(benchmark->filter, name);
            if (matches) {
                tracef("Running benchmark \"%s\"...", name);
                // TODO: Pin to one CPU hyperthread to avoid CPU migrations,
                //       which complicate TSC measurement.
                // TODO: Make this a realtime thread with a priority given by
                //       environment variable UDIPE_PRIORITY_BENCHMARK if set,
                //       by default halfway from the bottom of the realtime
                //       priority range to UDIPE_PRIORITY_WORKER, which itself
                //       is by default halfway through the entire realtime
                //       priority range. Start writing an environment variable
                //       doc that covers this + UDIPE_LOG.
                callable(context, benchmark);

                trace("Recalibrating benchmark clock...");
                benchmark_clock_recalibrate(&benchmark->clock);
            }
        });
        return matches;
    }

    DEFINE_PUBLIC
    UDIPE_NON_NULL_ARGS
    void udipe_benchmark_finalize(udipe_benchmark_t** benchmark) {
        logger_t logger = (*benchmark)->logger;
        with_logger(&logger, {
            info("All benchmarks executed successfully!");

            debug("Finalizing the benchmark clock...");
            benchmark_clock_finalize(&(*benchmark)->clock);

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