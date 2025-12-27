#ifdef UDIPE_BUILD_BENCHMARKS

    #pragma once

    //! \file
    //! \brief Benchmarking utilities
    //!
    //! This supplements the "public" interface defined inside of
    //! `udipe/benchmarks.h` with private utilities that can only be used within
    //! the libudipe codebase and microbenchmarks thereof.

    #include <udipe/benchmark.h>

    #include <udipe/pointer.h>
    #include <udipe/time.h>

    #include "arch.h"
    #include "error.h"
    #include "log.h"
    #include "memory.h"
    #include "name_filter.h"

    #include <assert.h>
    #include <hwloc.h>
    #include <stdint.h>

    #ifdef __unix__
        #include <time.h>
        #include <unistd.h>
    #elif defined(_WIN32)
        #include <profileapi.h>
    #endif


    /// \name Statistical analysis of timing data
    /// \{

    /// Result of the statistical analysis of a duration-based dataset
    ///
    /// This provides the most likely value and x% confidence interval of some
    /// quantity that originates from benchmark clock measurements. We use
    /// bootstrap resampling techniques to avoid assuming that duration
    /// measurements are normally distributed (which is totally false).
    ///
    /// To maximize statistical analysis code sharing between the code paths
    /// associated with different clocks and different stages of the benchmark
    /// harness setup process, the quantity that is being measured (nanoseconds,
    /// clock ticks, TSC frequency...) and the width of the confidence interval
    /// are purposely left unspecified.
    typedef struct stats_s {
        /// Most likely value of the duration-based measurement
        ///
        /// This value is determined by repeatedly drawing a small amount of
        /// duration samples from the duration dataset, taking their median
        /// value (which eliminates outliers), and then taking the median of a
        /// large number of these median values (which determines the most
        /// likely small-window median duration value).
        int64_t center;

        /// Lower bound of the confidence interval
        ///
        /// This value is determined by taking the specified lower quantile of
        /// the bootstrap median timing distribution discussed above:
        ///
        /// - For 95% confidence intervals, this is the 2.5% quantile of the
        ///   median timing distribution.
        /// - For 99% confidence intervals, this is the 0.5% quantile of the
        ///   median timing distribution.
        ///
        /// Bear in mind that larger confidence intervals require more
        /// measurements and median value computations to converge reasonably
        /// close to their true statistical asymptote.
        int64_t low;

        /// Higher bound of the confidence interval
        ///
        /// This provides the higher bound of the confidence interval using the
        /// same conventions as `low`:
        ///
        /// - For 95% confidence intervals, this is the 97.5% quantile of the
        ///   median timing distribution.
        /// - For 99% confidence intervals, this is the 99.5% quantile of the
        ///   median timing distribution.
        int64_t high;
    } stats_t;

    /// Harness for statistically analyzing duration data with a certain
    /// confidence interval
    ///
    /// We will typically end up analyzing many timing datasets with the same
    /// confidence interval, which means that it is beneficial to keep around
    /// the associated memory allocation and layout information.
    typedef struct duration_analyzer_s {
        int64_t* medians;  ///< Storage for median duration samples
        size_t num_medians;  ///< Number of samples within `medians`
        size_t low_idx;  ///< Confidence interval start location
        size_t center_idx;  ///< Median location
        size_t high_idx;  ///< Confidence interval end location
    } duration_analyzer_t;

    /// Set up a \ref duration_analyzer_t
    ///
    /// Given a confidence interval, get ready to analyze duration data with
    /// this confidence interval.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param confidence is the desired width of confidence intervals in
    ///                   percentage points (i.e. between 0.0 and 100.0,
    ///                   excluding both bounds)
    duration_analyzer_t duration_analyzer_initialize(float confidence);

    /// Statistically analyze timing data
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param analyzer is a duration analyzer that has been previously set up
    ///                 via duration_analyzer_initialize() and hasn't been
    ///                 destroyed via duration_analyzer_finalize() yet
    /// \param durations is the raw duration data from your clock
    /// \param durations_len is the number of data points within `durations`
    ///
    /// \returns the timing statistics associated with the input durations
    UDIPE_NON_NULL_ARGS
    stats_t analyze_duration(duration_analyzer_t* analyzer,
                             int64_t durations[],
                             size_t durations_len);

    /// Destroy a \ref duration_analyzer_t
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param analyzer is a duration analyzer that has been previously set up
    ///                 via duration_analyzer_initialize() and hasn't been
    ///                 destroyed via duration_analyzer_finalize() yet
    UDIPE_NON_NULL_ARGS
    void duration_analyzer_finalize(duration_analyzer_t* analyzer);

    /// \}


    /// \name Common clock properties
    /// \{

    /// Signed version of \ref udipe_duration_ns_t
    ///
    /// Most clocks guarantee that if two timestamps t1 and t2 were taken in
    /// succession, t2 cannot be "lesser than" t1 and therefore t2 - t1 must be
    /// a positive or zero duration. But this monotonicity property is
    /// unfortunately partially lost we attempt to compute true user code
    /// durations, i.e. the time that elapsed between the end of the now() at
    /// the beginning of a benchmark workload and the start of now() at the end
    /// of a benchmark workload. There are two reasons for this:
    ///
    /// - Computing the user workload duration requires us to subtract the clock
    ///   access delay, which is not perfectly known but estimated by
    ///   statistical means (and may indeed fluctuate one some uncommon hardware
    ///   configurations). If we over-estimate the clock access delay, then
    ///   negative duration measurements may happen.
    /// - Clocks do not guarantee that a timestamp will always be acquired at
    ///   the same time between the start and the end of the call to now(), and
    ///   this introduces an uncertainty window over the position of time
    ///   windows that can be as large as the clock access delay in the worst
    ///   case (though it will usually be smaller). If we take t the true
    ///   duration and dt the clock access time, the corrected duration `t2 - t1
    ///   - dt` may therefore be anywhere within the `[t - dt; t + dt]` range.
    ///   This means that in the edge case where `t < dt`, the computed duration
    ///   may also be negative.
    ///
    /// As a consequence of this, negative durations may pop up in intermediate
    /// computations of performance benchmarks, though they should never
    /// remain around in the final output of the computation if the benchmark
    /// was carried out correctly with workload durations that far exceed the
    /// clock access delay.
    typedef int64_t signed_duration_ns_t;

    /// \}


    /// \name Operating system clock
    /// \{

    /// Raw system clock timestamp
    ///
    /// This type is OS-specific and its values should not be used directly.
    /// Instead they are meant to be read with os_now() during a benchmark,
    /// buffered for a while, then post-processed using the os_duration()
    /// function which computes duration estimates from pairs of timestamps.
    #ifdef _POSIX_TIMERS
        typedef struct timespec os_timestamp_t;
    #elif defined(_WIN32)
        typedef LONG_INTEGER os_timestamp_t;
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif

    /// Check if two OS timestamps are equal
    ///
    /// If two timestamps that have been measured at different times turn out to
    /// be equal, it means that the system clock access time is smaller than the
    /// clock resolution (smallest nonzero difference between two clock
    /// readouts).
    ///
    /// When this happens, clock resolution is likely to be the factor that will
    /// limit OS clock timing precision. This is not as common as it was back in
    /// the days where clocks had a milisecond or microsecond time resolution,
    /// but it may still happen if e.g. one uses the clock() C library function
    /// as the timing backend in a microbenchmarking library.
    static inline bool os_timestamp_eq(os_timestamp_t t1, os_timestamp_t t2) {
        #if defined(_POSIX_TIMERS)
            return t1.tv_sec == t2.tv_sec && t1.tv_nsec == t2.tv_nsec;
        #elif defined(_WIN32)
            return t1.QuadPart == t2.QuadPart;
        #else
            #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
        #endif
    }

    /// Check if OS timestamp `t1` is lesser than or equal to timestamp `t2`
    ///
    /// This is a common sanity check in timing code, used to ensure that the
    /// clocks used for benchmarking are monotonic i.e. their timestamps never
    /// go back in time and can only go up (though they may remain constant).
    static inline bool os_timestamp_le(os_timestamp_t t1, os_timestamp_t t2) {
        #if defined(_POSIX_TIMERS)
            return t1.tv_sec < t2.tv_sec
                   || (t1.tv_nsec == t2.tv_sec && t1.tv_nsec <= t2.tv_nsec);
        #elif defined(_WIN32)
            return t1.QuadPart <= t2.QuadPart;
        #else
            #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
        #endif
    }

    /// Operating system clock
    ///
    /// This contains a cache of anything needed to (re)calibrate the operating
    /// system clock and use it for duration measurements.
    typedef struct os_clock_s {
        #ifdef _WIN32
            /// Frequency of the Win32 performance counter in ticks/second
            ///
            /// This is just the cached output of
            /// QueryPerformanceFrequency() in 64-bit form.
            ///
            /// To convert performance counter ticks to nanoseconds, multiply
            /// the number of ticks by one billion (`1000*1000*1000`) then
            /// divide it by this number.
            uint64_t win32_frequency;
        #endif

        /// Clock offset statistics in nanoseconds
        ///
        /// This is the offset that must be subtracted from OS clock durations
        /// in order to get an unbiased estimator of the duration of the code
        /// that is being benchmarked, excluding the cost of os_now() itself.
        ///
        /// You do not need to perform this offset subtraction yourself,
        /// os_duration() will take care of it for you.
        stats_t offset_stats;

        /// Empty loop iteration count at which the best relative precision on
        /// the loop iteration duration is achieved
        ///
        /// This is a useful starting point when recalibrating the system clock,
        /// or when calibrating a different clock based on the system clock.
        size_t best_empty_iters;

        /// Duration statistics for the best empty loop, in nanoseconds
        ///
        /// This data can be used in several different ways:
        ///
        /// - The central value indicates the timed function duration for which
        ///   the OS clock has optimal relative precision on the benchmark loop
        ///   iteration duration. It can be used as a target when calibrating
        ///   benchmark run iteration counts.
        /// - The (low, high) spread indicates the absolute clock precision in
        ///   the optimal regime where OS interrupts do not play a role yet.
        stats_t best_empty_stats;

        /// Timestamp buffer
        ///
        /// This is used for timestamp storage during OS clock measurements. It
        /// contains enough storage for `num_durations + 1` timestamps.
        os_timestamp_t* timestamps;

        /// Duration buffer
        ///
        /// This is used for duration storage during OS clock measurements. It
        /// contains enough storage for `num_durations` durations.
        signed_duration_ns_t* durations;

        /// Duration buffer capacity
        ///
        /// See individual buffer descriptions for more information about how
        /// buffer capacities derive from this quantity.
        size_t num_durations;
    } os_clock_t;

    /// Set up the system clock
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param calibration_analyzer should have been initialized with
    ///                             duration_analyzer_initialize() based on the
    ///                             width of the calibration confidence interval
    ///                             and not have been finalized yet
    ///
    /// \returns a system clock context that must later be finalized using
    ///          os_clock_finalize()
    UDIPE_NON_NULL_ARGS
    os_clock_t os_clock_initialize(duration_analyzer_t* calibration_analyzer);

    /// Read the system clock
    ///
    /// The output of this function is OS-specific and unrelated to any time
    /// base you may be familiar with like UTC or local time. To minimize
    /// measurement condition drift, you should only buffer these timestamps
    /// during the measurement cycle, then post-process pairs of them into
    /// duration estimates using os_duration().
    ///
    /// \returns a timestamp representing the current time at some point between
    ///          the moment where os_now() was called and the moment where the
    ///          call to os_now() returned.
    static inline os_timestamp_t os_now() {
        os_timestamp_t timestamp;
        #if defined(_POSIX_TIMERS)
            clockid_t clock;
            #ifdef __linux__
                clock = CLOCK_MONOTONIC_RAW;
            #elif defined(_POSIX_MONOTONIC_CLOCK)
                clock = CLOCK_MONOTONIC;
            #else
                clock = CLOCK_REALTIME;
            #endif
            int result = clock_gettime(clock, &timestamp);
            assert(result == 0);
        #elif defined(_WIN32)
            bool result = QueryPerformanceCounter(&timestamp);
            assert(result);
        #else
            #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
        #endif
        return timestamp;
    }

    /// Estimate the elapsed time between two system clock readouts
    ///
    /// Given the `start` and `end` timestamps returned by two calls to now(),
    /// where `start` was measured before `end`, this estimates the amount of
    /// time that elapsed between the end of the call to os_now() that returned
    /// `start` and the beginning of the call to os_now() that returned `end`.
    ///
    /// \param clock is a set of clock parameters that were previously measured
    ///              via os_clock_initialize() and haven't been finalized yet.
    /// \param start is the timestamp that was measured using os_now() at the
    ///              start of the time span of interest.
    /// \param end is the timestamp that was measured using os_now() at the end
    ///            of the time span of interest (and therefore after `start`).
    ///
    /// \returns an offset-corrected estimate of the amount of time that elapsed
    ///          between `start` and `end`, in nanoseconds.
    UDIPE_NON_NULL_ARGS
    static inline stats_t os_duration(os_clock_t* clock,
                                      os_timestamp_t start,
                                      os_timestamp_t end) {
        assert(os_timestamp_le(start, end));
        const uint64_t nano = 1000 * 1000 * 1000;
        signed_duration_ns_t uncorrected_ns;
        #if defined(_POSIX_TIMERS)
            uncorrected_ns = (end.tv_sec - start.tv_sec) * nano;
            if (start.tv_nsec > end.tv_nsec) uncorrected_ns -= nano;
            int nanosecs = abs((int)end.tv_nsec - (int)start.tv_nsec);
            uncorrected_ns += nanosecs;
        #elif defined(_WIN32)
            assert(clock->win32_frequency > 0);
            uncorrected_ns = (end.QuadPart - start.QuadPart) * nano / clock->win32_frequency;
        #else
            #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
        #endif
        return (stats_t){
            .center = uncorrected_ns - clock->offset_stats.center,
            .low = uncorrected_ns - clock->offset_stats.high,
            .high = uncorrected_ns - clock->offset_stats.low
        };
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
    ///              all OSes `offset` must be zeroed out if not known yet).
    /// \param workload is the workload whose duration should be measured. For
    ///                 minimal overhead/bias, its definition should be visible
    ///                 to the compiler at the point where this function is
    ///                 called, so that it can be inlined into it.
    /// \param context encodes the parameters that should be passed to
    ///                `workload`, if any.
    /// \param num_runs indicates how many timed calls to `workload` should
    ///                 be performed, see above for tuning advice.
    /// \param analyzer is a duration analyzer that has been previously set up
    ///                 via duration_analyzer_initialize() and hasn't been
    ///                 destroyed via duration_analyzer_finalize() yet.
    ///
    /// \returns `workload` execution time statistics in nanoseconds
    ///
    /// \internal
    ///
    /// This function is marked as `static inline` to encourage the compiler to
    /// make one copy of it per `workload` and inline `workload` into it,
    /// assuming the caller did their homework on their side by exposing the
    /// definition of `workload` at the point where os_clock_measure() is called.
    UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 5)
    static inline stats_t os_clock_measure(
        os_clock_t* clock,
        void (*workload)(void*),
        void* context,
        size_t num_runs,
        duration_analyzer_t* analyzer
    ) {
        if (num_runs > clock->num_durations) {
            trace("Reallocating storage from %zu to %zu durations...");
            realtime_liberate(
                clock->timestamps,
                (clock->num_durations+1) * sizeof(os_timestamp_t)
            );
            realtime_liberate(
                clock->durations,
                clock->num_durations * sizeof(signed_duration_ns_t)
            );
            clock->num_durations = num_runs;
            clock->timestamps =
                realtime_allocate((num_runs+1) * sizeof(os_timestamp_t));
            clock->durations =
                realtime_allocate(num_runs * sizeof(signed_duration_ns_t));
        }

        trace("Performing minimal CPU warmup...");
        os_timestamp_t* timestamps = clock->timestamps;
        signed_duration_ns_t* durations = clock->durations;
        timestamps[0] = os_now();
        UDIPE_ASSUME_READ(timestamps);
        workload(context);

        tracef("Performing %zu timed runs...", num_runs);
        timestamps[0] = os_now();
        for (size_t run = 0; run < num_runs; ++run) {
            UDIPE_ASSUME_READ(timestamps);
            workload(context);
            timestamps[run+1] = os_now();
        }

        trace("Analyzing durations...");
        stats_t result;
        trace("- Computing central run durations...");
        for (size_t run = 0; run < num_runs; ++run) {
            durations[run] = os_duration(clock,
                                         timestamps[run],
                                         timestamps[run+1]).center;
            tracef("- center[%zu] = %zd ns", run, durations[run]);
        }
        trace("- Analyzing central run durations...");
        result.center = analyze_duration(analyzer, durations, num_runs).center;
        trace("- Computing lower run durations...");
        for (size_t run = 0; run < num_runs; ++run) {
            durations[run] = os_duration(clock,
                                         timestamps[run],
                                         timestamps[run+1]).low;
            tracef("- low[%zu] = %zd ns", run, durations[run]);
        }
        trace("- Analyzing lower run durations...");
        result.low = analyze_duration(analyzer, durations, num_runs).low;
        trace("- Computing higher run durations...");
        for (size_t run = 0; run < num_runs; ++run) {
            durations[run] = os_duration(clock,
                                         timestamps[run],
                                         timestamps[run+1]).high;
            tracef("- high[%zu] = %zd ns", run, durations[run]);
        }
        trace("- Analyzing higher run durations...");
        result.high = analyze_duration(analyzer, durations, num_runs).high;
        return result;
    }

    /// Destroy the system clock
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param clock is a system clock that has been previously set up
    ///              via os_clock_initialize() and hasn't been destroyed via
    ///              os_clock_finalize() yet
    UDIPE_NON_NULL_ARGS
    void os_clock_finalize(os_clock_t* clock);

    /// \}


    #ifdef X86_64
        /// \name TSC clock (x86-specific for now)
        /// \{

        /// x86 TSC clock context
        ///
        /// This contains a cache of anything needed to (re)calibrate the x86
        /// TimeStamp Counter and use it for duration measurements.
        typedef struct x86_clock_s {
            /// Duration statistics for the best empty loop, in TSC ticks
            ///
            /// This data can be used in several different ways:
            ///
            /// - The central value indicates the timed function duration for
            ///   which the TSC has optimal relative precision on the benchmark
            ///   loop iteration duration. It can be used as a target when
            ///   calibrating benchmark run iteration counts.
            /// - The (low, high) spread indicates the absolute clock precision
            ///   in the optimal regime where OS interrupts do not play a
            ///   significant role yet.
            ///
            /// The associated empty loop iteration count can be found in the
            /// \ref os_clock_t against which the TSC was calibrated.
            stats_t best_empty_stats;

            /// Statistics of x86_timer_start()/x86_timer_end() overhead inside
            /// of the timed region, in TSC ticks
            ///
            /// This is the offset that must be subtracted from TSC differences
            /// in order to get an unbiased estimator of the duration of the
            /// benchmarked code, excluding the cost of
            /// x86_timer_start()/x86_timer_end() themselves.
            stats_t offset_stats;

            /// Frequency of the TSC clock in ticks/second
            ///
            /// This is calibrated against the OS clock, enabling us to turn
            /// RDTSC readings into nanoseconds as with `win32_frequency`.
            ///
            /// Because this frequency is derived from an OS clock measurement,
            /// it is not perfectly known, as highlighted by the fact that this
            /// is statistics not an absolute number. This means that
            /// precision-sensitive computations should ideally be performed in
            /// terms of TSC ticks, not nanoseconds.
            stats_t frequency_stats;

            /// Timestamp buffer
            ///
            /// This is used for timestamp storage during TSC measurements. It
            /// contains enough storage for `2*num_durations` timestamps.
            ///
            /// In terms of layout, it begins with all the `num_durations` start
            /// timestamps, followed by all the `num_durations` end timestamps,
            /// which ensures optimal SIMD processing.
            ///
            /// Because the timing thread is pinned to a single CPU core, we do
            /// not need to keep the CPU IDs around, only to check that the
            /// pinning is effective at keeping these constant in debug builds.
            /// Therefore we extract the instant values from the timestamps and
            /// only keep that around.
            x86_instant* instants;

            /// Duration buffer
            ///
            /// This is used for duration storage during TSC measurements. It
            /// contains enough storage for `num_durations` durations.
            x86_duration_ticks* ticks;

            /// Duration buffer capacity
            ///
            /// See individual buffer descriptions for more information about
            /// how buffer capacities derive from this quantity.
            size_t num_durations;
        } x86_clock_t;

        /// Set up the TSC clock
        ///
        /// The TSC is calibrated against the OS clock, which must therefore be
        /// calibrated first before the TSC can be calibrated.
        ///
        /// TSC calibration should ideally happen immediately after system clock
        /// setup, so that \ref os_clock_t::best_empty_stats is maximally up to
        /// date (e.g. CPU clock frequency did not have any time to drift to a
        /// different value).
        ///
        /// This function must be called within the scope of with_logger().
        ///
        /// \param os is a system clock context that was freshly initialized
        ///           with os_clock_initialize(), ideally right before calling
        ///           this function, and hasn't been finalized with
        ///           os_clock_finalize() yet.
        /// \param calibration_analyzer should have been initialized with
        ///                             duration_analyzer_initialize() based on
        ///                             the width of the calibration confidence
        ///                             interval and not have been finalized yet
        ///
        /// \returns a TSC clock context that must later be finalized using
        ///          x86_clock_finalize().
        UDIPE_NON_NULL_ARGS
        x86_clock_t
        x86_clock_initialize(const os_clock_t* os,
                             duration_analyzer_t* calibration_analyzer);

        /// Measure the execution duration of `workload` using the TSC clock
        ///
        /// This works a lot like os_clock_measure(), but it uses the TSC clock
        /// instead of the system clock, which changes a few things:
        ///
        /// - The timing thread that calls this function must have been pinned
        ///   to a specific CPU core to avoid CPU migrations. This is implicitly
        ///   taken care of by udipe_benchmark_initialize() before calling
        ///   benchmark_clock_initialize() and also by udipe_benchmark_run()
        ///   before calling the user-provided benchmarking routine.
        /// - Output measurements are provided in clock ticks not nanoseconds.
        ///   To convert them into nanoseconds, you must use
        ///   clock->frequency_stats, taking care to widen the output confidence
        ///   interval based on the associated TSC frequency uncertainty. The
        ///   x86_duration() function can be used to perform this conversion.
        ///
        /// This function must be called within the scope of with_logger().
        ///
        /// \param clock mostly works as in os_clock_measure(), except it wants
        ///              a TSC clock context not an OS clock context
        /// \param workload works as in os_clock_measure()
        /// \param context works as in os_clock_measure()
        /// \param num_runs works as in os_clock_measure()
        /// \param analyzer works as in os_clock_measure()
        ///
        /// \returns `workload` execution time statistics in TSC ticks
        ///
        /// \internal
        ///
        /// This function is `static inline` for the same reason that
        /// os_clock_measure() is `static inline`.
        UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 5)
        static inline stats_t x86_clock_measure(
            x86_clock_t* clock,
            void (*workload)(void*),
            void* context,
            size_t num_runs,
            duration_analyzer_t* analyzer
        ) {
            if (num_runs > clock->num_durations) {
                trace("Reallocating storage from %zu to %zu durations...");
                realtime_liberate(
                    clock->instants,
                    2 * clock->num_durations * sizeof(x86_instant)
                );
                realtime_liberate(
                    clock->ticks,
                    clock->num_durations * sizeof(x86_duration_ticks)
                );
                clock->num_durations = num_runs;
                clock->instants =
                    realtime_allocate(2 * num_runs * sizeof(x86_instant));
                clock->ticks =
                    realtime_allocate(num_runs * sizeof(x86_duration_ticks));
            }

            trace("Setting up measurement...");
            x86_instant* starts = clock->instants;
            x86_instant* ends = clock->instants + num_runs;
            x86_duration_ticks* ticks = clock->ticks;
            const bool strict = false;
            x86_timestamp_t timestamp = x86_timer_start(strict);
            const x86_cpu_id initial_cpu_id = timestamp.cpu_id;

            trace("Performing minimal CPU warmup...");
            starts[0] = timestamp.ticks;
            UDIPE_ASSUME_READ(starts);
            workload(context);
            timestamp = x86_timer_end(strict);
            assert(timestamp.cpu_id == initial_cpu_id);
            ends[0] = timestamp.ticks;
            UDIPE_ASSUME_READ(ends);

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

            trace("Analyzing durations...");
            stats_t result;
            trace("- Computing central run durations...");
            for (size_t run = 0; run < num_runs; ++run) {
                ticks[run] = ends[run] - starts[run] - clock->offset_stats.center;
                tracef("  * center[%zu] = %zd", run, ticks[run]);
            }
            trace("- Analyzing central run durations...");
            result.center = analyze_duration(analyzer, ticks, num_runs).center;
            trace("- Computing lower run durations...");
            for (size_t run = 0; run < num_runs; ++run) {
                ticks[run] = ends[run] - starts[run] - clock->offset_stats.high;
                tracef("  * low[%zu] = %zd", run, ticks[run]);
            }
            trace("- Analyzing lower run durations...");
            result.low = analyze_duration(analyzer, ticks, num_runs).low;
            trace("- Computing higher run durations...");
            for (size_t run = 0; run < num_runs; ++run) {
                ticks[run] = ends[run] - starts[run] - clock->offset_stats.low;
                tracef("  * high[%zu] = %zd", run, ticks[run]);
            }
            trace("- Analyzing higher run durations...");
            result.high = analyze_duration(analyzer, ticks, num_runs).high;
            return result;
        }

        /// Convert x86 clock ticks statistics into duration statistics
        ///
        /// \param clock is a TSC clock context that was freshly initialized
        ///              with x86_clock_initialize() and hasn't been finalized
        ///              with x86_clock_finalize() yet
        /// \param ticks are TSC tick statistics, typically from
        ///              x86_clock_measure()
        ///
        /// \returns duration statistics in nanoseconds
        UDIPE_NON_NULL_ARGS
        static inline stats_t x86_duration(x86_clock_t* clock,
                                           stats_t ticks) {
            const int64_t nano = 1000*1000*1000;
            return (stats_t){
                .center = ticks.center * nano / clock->frequency_stats.center,
                .low = ticks.low * nano / clock->frequency_stats.high,
                .high = ticks.high * nano / clock->frequency_stats.low
            };
        }

        /// Destroy the TSC clock
        ///
        /// This function must be called within the scope of with_logger().
        ///
        /// \param clock is a TSC clock context that has been previously set up
        ///              via x86_clock_initialize() and hasn't been destroyed
        ///              via x86_clock_finalize() yet
        UDIPE_NON_NULL_ARGS
        void x86_clock_finalize(x86_clock_t* clock);

        /// \}
    #endif  // X86_64


    /// \name Benchmark clock
    /// \{

    /// Benchmark clock
    ///
    /// This is a unified interface to the operating system and CPU clocks,
    /// which attempts to pick the best clock available on the target operating
    /// system and CPU architecture.
    typedef struct benchmark_clock_s {
        #ifdef X86_64
            /// TSC clock context
            ///
            /// This contains everything needed to recalibrate and use the x86
            /// TimeStamp Counter clock.
            x86_clock_t x86;
        #endif

        /// Duration analyzer for everyday benchmark measurements
        ///
        /// This represents a confidence interval of MEASUREMENT_CONFIDENCE and
        /// is used whenever regular benchmark measurements are taken.
        duration_analyzer_t measurement_analyzer;

        /// System clock context
        ///
        /// This contains everything needed to recalibrate and use the operating
        /// system clock.
        os_clock_t os;

        /// Duration analyzer for clock calibration data
        ///
        /// This represents a confidence interval of CALIBRATION_CONFIDENCE and
        /// is used whenever a clock is (re)calibrated.
        duration_analyzer_t calibration_analyzer;
    } benchmark_clock_t;

    /// Set up the benchmark clock
    ///
    /// Since operating systems do not expose many useful properties of their
    /// high-resolution clocks, these properties must unfortunately be manually
    /// calibrated by applications through a set of measurements, which will
    /// take some time.
    ///
    /// Furthermore, some aspects of this initial calibration may not remain
    /// correct forever, as system operation conditions can change during
    /// long-running benchmarks. It is therefore strongly recommended to call
    /// benchmark_clock_recalibrate() between two sets of measurements, so that
    /// the benchmark clock gets automatically recalibrated whenever necessary.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \returns a benchmark clock configuration that is meant to be integrated
    ///          into \ref udipe_benchmark_t, and eventually destroyed with
    ///          benchmark_clock_finalize().
    benchmark_clock_t benchmark_clock_initialize();

    // TODO: Benchmark clock measurement in raw clock units
    // TODO: Conversion of measurements from raw clock units to nanoseconds

    /// Check if the benchmark clock needs recalibration, if so recalibrate it
    ///
    /// This recalibration process mainly concerns the `best_empty_stats` of
    /// each clock, which may evolve as the system background workload changes.
    /// But it is also a good occasion to sanity-check that other clock
    /// parameters still seem valid.
    ///
    /// It should be run at the time where execution shifts from one benchmark
    /// workload to another, as performing statistics over measurements which
    /// were using different clock calibrations is fraught with peril.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param clock must be a benchmark clock configuration that was
    ///              initialized with benchmark_clock_initialize() and hasn't
    ///             been destroyed with benchmark_clock_finalize() yet.
    UDIPE_NON_NULL_ARGS
    void benchmark_clock_recalibrate(benchmark_clock_t* clock);

    /// Destroy the benchmark clock
    ///
    /// After this is done, the benchmark clock must not be used again.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param clock must be a benchmark clock configuration that was
    ///              initialized with benchmark_clock_initialize() and hasn't
    ///             been destroyed with benchmark_clock_finalize() yet.
    UDIPE_NON_NULL_ARGS
    void benchmark_clock_finalize(benchmark_clock_t* clock);

    /// \}


    /// \copydoc udipe_benchmark_t
    struct udipe_benchmark_s {
        /// Harness logger
        ///
        /// The benchmark harness implementation will use this logger to explain
        /// what it's doing. However, measurements are a benchmark binary's
        /// primary output. They should therefore be emitted over stdout or as
        /// structured data for programmatic manipulation, not as logs.
        logger_t logger;

        /// Benchmark name filter
        ///
        /// Used by udipe_benchmark_run() to decide which benchmarks should run.
        name_filter_t filter;

        /// hwloc topology
        ///
        /// Used to pin timing measurement routines on a single CPU core so that
        /// TSC timing works reliably.
        hwloc_topology_t topology;

        /// Timing thread cpuset
        ///
        /// Probed at benchmark harness initialization time and used to ensure
        /// that timing measurement routines remain pinned to the same CPU core
        /// from then on.
        hwloc_cpuset_t timing_cpuset;

        /// Benchmark clock
        ///
        /// Used in the adjustment of benchmark parameters and interpretation of
        /// benchmark results.
        benchmark_clock_t clock;
    };

#endif  // UDIPE_BUILD_BENCHMARKS