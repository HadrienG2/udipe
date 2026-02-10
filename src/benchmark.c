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


    // TODO: Move to other files

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


    /// Log a statistical estimate, possibly as part of a bullet list
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param level is the verbosity level at which this log will be emitted
    /// \param bullet is a string that will be prepended to the log. This is
    ///               used for bullet lists of estimates. If you are not
    ///               displaying a bullet list you can set this to NULL, or
    ///               better yet use the simple log_estimate() function.
    /// \param name identifies the estimate that is being displayed
    /// \param estimate is the \ref estimate_t to be displayed
    /// \param unit is a string that spells out the measurement unit of
    ///             `estimate`
    UDIPE_NON_NULL_SPECIFIC_ARGS(3, 5)
    static void log_estimate_impl(udipe_log_level_t level,
                                  const char bullet[],
                                  const char name[],
                                  estimate_t estimate,
                                  const char unit[]) {
        // Find the smallest fluctuation around the mean
        assert(estimate.low <= estimate.center);
        double min_spread = estimate.center - estimate.low;
        assert(estimate.high >= estimate.center);
        if (min_spread > estimate.high - estimate.center) {
            min_spread = estimate.high - estimate.center;
        }

        // Deduce how many significant digits should be displayed
        int precision = 1;
        if (fabs(estimate.center) != 0.0) {
            precision += floor(log10(fabs(estimate.center)));
        }
        assert(min_spread >= 0.0);
        if (min_spread > 0.0) {
            precision += 1 - floor(log10(min_spread));
        }

        // Display the estimate
        const char* space_after_bullet = " ";
        if (!bullet) {
            bullet = "";
            space_after_bullet = "";
        }
        udipe_logf(level,
                   "%s%s%s: %.*g %s with %g%% CI [%.*g; %.*g].",
                   bullet,
                   space_after_bullet,
                   name,
                   precision,
                   estimate.center,
                   unit,
                   CONFIDENCE * 100.0,
                   precision,
                   estimate.low,
                   precision,
                   estimate.high);
    }

    /// Log a statistical estimate outside of a bullet list context
    ///
    /// Parameters work as in log_estimate_impl()
    UDIPE_NON_NULL_ARGS
    static void log_estimate(udipe_log_level_t level,
                             const char name[],
                             estimate_t estimate,
                             const char unit[]) {
        log_estimate_impl(level, NULL, name, estimate, unit);
    }


    /// Log measurement statistics
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param level is the verbosity level at which this log will be emitted
    /// \param title serves as a headed to the overall display
    /// \param bullet will be prepended to each stat's display
    /// \param stats are the \ref statistics_t to be displayed
    /// \param unit is a string that spells out the measurement unit of
    ///             `stats`
    UDIPE_NON_NULL_ARGS
    static void log_statistics(udipe_log_level_t level,
                               const char title[],
                               const char bullet[],
                               statistics_t stats,
                               const char unit[]) {
        udipe_logf(level, "%s:", title);
        log_estimate_impl(level,
                          bullet,
                          "Sym. dispersion start",
                          stats.sym_dispersion_start,
                          unit);
        log_estimate_impl(level,
                          bullet,
                          "Low dispersion bound",
                          stats.low_dispersion_bound,
                          unit);
        log_estimate_impl(level,
                          bullet,
                          "Mean",
                          stats.mean,
                          unit);
        log_estimate_impl(level,
                          bullet,
                          "High dispersion bound",
                          stats.high_dispersion_bound,
                          unit);
        log_estimate_impl(level,
                          bullet,
                          "Sym. dispersion end",
                          stats.sym_dispersion_end,
                          unit);
        log_estimate_impl(level,
                          bullet,
                          "Sym. dispersion width",
                          stats.sym_dispersion_width,
                          unit);
    }

    /// Compute the relative dispersion from some \ref estimate_t
    ///
    /// \param estimate is an \ref estimate_t that directly or indirectly derive
    ///                 from some measurements.
    /// \returns the relative magnitude of its dispersion in percentage points
    ///          of the central tendency.
    static inline double relative_dispersion(estimate_t estimate) {
        return (double)(estimate.high - estimate.low) / estimate.center * 100.0;
    }

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

        return result;
    }


    UDIPE_NON_NULL_ARGS
    os_clock_t os_clock_initialize(outlier_filter_t* outlier_filter,
                                   analyzer_t* analyzer) {
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
        const statistics_t offset_stats =
            analyzer_apply(analyzer, &clock.offsets);
        log_statistics(UDIPE_DEBUG,
                       "- Clock offset",
                       "  *",
                       offset_stats,
                       "ns");

        info("Deducing clock baseline...");
        distribution_t tmp_zeros = distribution_sub(&clock.builder,
                                                    &clock.offsets,
                                                    &clock.offsets);
        const statistics_t zero_stats = analyzer_apply(analyzer, &tmp_zeros);
        log_statistics(UDIPE_DEBUG,
                       "- Baseline",
                       "  *",
                       zero_stats,
                       "ns");
        clock.builder = distribution_reset(&tmp_zeros);

        info("Finding minimal measurable loop...");
        distribution_t loop_durations;
        statistics_t loop_duration_stats;
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
            loop_duration_stats = analyzer_apply(analyzer, &loop_durations);
            log_statistics(UDIPE_DEBUG,
                           "  * Loop duration",
                           "    -",
                           loop_duration_stats,
                           "ns");
            if (loop_duration_stats.low_dispersion_bound.low <= 0.0) {
                debug("  * Measuring a zero duration is too likely...");
            } else if(loop_duration_stats.mean.low < 10*loop_duration_stats.sym_dispersion_width.high) {
                debug("  * Duration signal-noise ratio is too low...");
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
        const int64_t best_precision = loop_duration_stats.sym_dispersion_width.high;
        estimate_t best_iter =
            estimate_iteration_duration(loop_duration_stats.mean,
                                        clock.best_empty_iters);
        double best_dispersion = relative_dispersion(best_iter);
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
            loop_duration_stats = analyzer_apply(analyzer, &loop_durations);
            log_statistics(UDIPE_DEBUG,
                           "  * Loop duration",
                           "    -",
                           loop_duration_stats,
                           "ns");
            const estimate_t curr_iter =
                estimate_iteration_duration(loop_duration_stats.mean,
                                            num_iters);
            log_estimate(UDIPE_DEBUG,
                         "  * Iteration duration",
                         curr_iter,
                         "ns");
            const double dispersion = relative_dispersion(curr_iter);
            const int64_t precision = loop_duration_stats.sym_dispersion_width.high;
            // In a regime of stable run timing precision, doubling the
            // iteration count should improve iteration timing dispersion by
            // 2x. Ignore small improvements that don't justify a 2x longer run
            // duration, and thus fewer runs per unit of execution time...
            if (dispersion < best_dispersion/1.1) {
                debug("  * This is our new best loop. Can we do even better?");
                best_iter = curr_iter;
                best_dispersion = dispersion;
                clock.best_empty_iters = num_iters;
                clock.builder = distribution_reset(&clock.best_empty_durations);
                clock.best_empty_durations = loop_durations;
                distribution_poison(&loop_durations);
                clock.best_empty_stats = loop_duration_stats;
                continue;
            } else if (precision <= 3*best_precision) {
                // ...but keep trying until the dispersion degradation becomes
                // much worse than expected in a regime of stable iteration
                // timing dispersion, in which case loop duration fluctuates 2x
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
        log_statistics(UDIPE_INFO,
                       "- Best loop duration",
                       "  *",
                       clock.best_empty_stats,
                       "ns");
        log_estimate(UDIPE_DEBUG,
                     "- Best iteration duration",
                     best_iter,
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
        clock->best_empty_stats = (statistics_t){ 0 };
    }


    #ifdef X86_64

        UDIPE_NON_NULL_ARGS
        x86_clock_t
        x86_clock_initialize(outlier_filter_t* outlier_filter,
                             os_clock_t* os,
                             analyzer_t* analyzer) {
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
            log_statistics(UDIPE_INFO,
                           "- Offset-biased best loop",
                           "  *",
                           analyzer_apply(analyzer, &raw_empty_ticks),
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
            log_statistics(UDIPE_INFO,
                           "- Clock offset",
                           "  *",
                           analyzer_apply(analyzer, &clock.offsets),
                           "ticks");

            info("Deducing clock baseline...");
            distribution_t tmp_zeros = distribution_sub(&builder,
                                                        &clock.offsets,
                                                        &clock.offsets);
            const statistics_t zero_stats = analyzer_apply(analyzer,
                                                           &tmp_zeros);
            builder = distribution_reset(&tmp_zeros);
            log_statistics(UDIPE_DEBUG,
                           "- Baseline",
                           "  *",
                           zero_stats,
                           "ticks");

            debug("Applying offset correction to best loop duration...");
            distribution_t corrected_empty_ticks = distribution_sub(
                &builder,
                &raw_empty_ticks,
                &clock.offsets
            );
            builder = distribution_reset(&raw_empty_ticks);
            clock.best_empty_stats = analyzer_apply(analyzer,
                                                    &corrected_empty_ticks);
            log_statistics(UDIPE_DEBUG,
                           "- Offset-corrected best loop",
                           "  *",
                           clock.best_empty_stats,
                           "ticks");
            const estimate_t best_iter_ticks =
                estimate_iteration_duration(clock.best_empty_stats.mean,
                                            os->best_empty_iters);
            log_estimate(UDIPE_DEBUG,
                         "- Loop iteration",
                         best_iter_ticks,
                         "ticks");

            info("Deducing TSC tick frequency...");
            clock.frequencies = distribution_scaled_div(
                &builder,
                &corrected_empty_ticks,
                UDIPE_SECOND,
                &os->best_empty_durations
            );
            // `builder` cannot be used after this point
            log_statistics(UDIPE_INFO,
                           "- TSC frequency",
                           "  *",
                           analyzer_apply(analyzer, &clock.frequencies),
                           "ticks/sec");

            debug("Deducing best loop duration...");
            const statistics_t best_empty_duration = x86_duration(
                &clock,
                &os->builder,
                &corrected_empty_ticks,
                analyzer
            );
            log_statistics(UDIPE_DEBUG,
                           "- Best loop duration",
                           "  *",
                           best_empty_duration,
                           "ns");
            const estimate_t best_iter_ns =
                estimate_iteration_duration(best_empty_duration.mean,
                                            os->best_empty_iters);
            log_estimate(UDIPE_DEBUG,
                         "- Loop iteration",
                         best_iter_ns,
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
        statistics_t x86_duration(x86_clock_t* clock,
                                  distribution_builder_t* tmp_builder,
                                  const distribution_t* ticks,
                                  analyzer_t* analyzer) {
            distribution_t tmp_durations =
                distribution_scaled_div(tmp_builder,
                                        ticks,
                                        UDIPE_SECOND,
                                        &clock->frequencies);
            const statistics_t result = analyzer_apply(analyzer, &tmp_durations);
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
            clock->best_empty_stats = (statistics_t){ 0 };
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

        debug("Setting up the statistical analyzer...");
        clock.analyzer = analyzer_initialize();

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
        analyzer_finalize(&clock->analyzer);

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