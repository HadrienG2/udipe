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
    #include "name_filter.h"

    #include <assert.h>
    #include <stdint.h>

    #ifdef __unix__
        #include <time.h>
        #include <unistd.h>
    #elif defined(_WIN32)
        #include <profileapi.h>
    #endif



    /// \name Benchmark timing measurements
    /// \{

    /// Result of the statistical analysis of a duration-based dataset
    ///
    /// This provides the most likely value and x% confidence interval of some
    /// quantity that originates from benchmark clock measurements, using
    /// bootstrap resampling techniques to avoid assuming that duration
    /// measurements are normally distributed (they aren't).
    ///
    /// To allow statistical analysis code sharing between the code paths
    /// associated with different clocks and different stages of the
    /// benchmarking process, the quantity that is being measured (nanoseconds,
    /// clock ticks, TSC frequency...) and the width of the confidence interval
    /// are purposely left unspecified.
    ///
    /// Such details must clarified at each point where this type is used. If
    /// you find that a specific configuration is used often, consider creating
    /// a typedef for this common configuration.
    typedef struct stats_s {
        /// Most likely value of the duration-based measurement
        ///
        /// This value is determined by repeatedly drawing a small amount of
        /// duration samples from the duration dataset, taking their median
        /// value (which eliminates outliers), then taking the median of a large
        /// number of these median values (which determines the most likely
        /// small-window median duration value).
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

    /// \ref stats_t from a duration measurement that was performed during
    /// calibration
    typedef stats_t calibration_duration_t;

    /// Harness for statistically analyzing duration data with a certain
    /// confidence interval
    ///
    /// We will typically end up analyzing many timing datasets with the same
    /// confidence interval, which means that it is beneficial to keep around
    /// the associated memory allocation and layout data.
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
    /// \param data is the raw duration data from the clock that you are using
    /// \param data_len is the number of data points within `data`
    UDIPE_NON_NULL_ARGS
    stats_t analyze_duration(duration_analyzer_t* analyzer,
                             int64_t data[],
                             size_t data_len);

    /// Destroy a \ref duration_analyzer_t
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param analyzer is a duration analyzer that has been previously set up
    ///                 via duration_analyzer_initialize() and hasn't been
    ///                 destroyed via duration_analyzer_finalize() yet
    UDIPE_NON_NULL_ARGS
    void duration_analyzer_finalize(duration_analyzer_t* analyzer);

    /// Benchmark clock configuration
    ///
    /// This is a cache of informations about the CPU and system's high
    /// resolution clocks that are needed for precision benchmarking, but can
    /// only be queried at runtime through a relatively time-consuming process
    /// whose results should be cached and reused across benchmarks.
    typedef struct benchmark_clock_s {
        #ifdef X86_64
            /// Frequency of the TSC clock in ticks/second
            ///
            /// This is calibrated against the OS clock, enabling us to turn
            /// RDTSC readings into nanoseconds as with `win32_frequency`.
            uint64_t tsc_frequency;

            /// Typical x86_timer_start()/x86_timer_end() overhead inside of the
            /// timed region, in TSC ticks
            ///
            /// This is the offset that must be subtracted from TSC differences
            /// in order to get an unbiased estimator of the duration of the
            /// benchmarked code, excluding the cost of
            /// x86_timer_start()/x86_timer_end() themselves.
            ///
            /// We measure the offset in ticks, not nanoseconds, because TSC
            /// ticks have sub-nanosecond resolution so ticks are more precise.
            x86_duration_ticks tsc_offset;
        #endif

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

        /// Typical OS clock offset in nanoseconds
        ///
        /// This is the offset that must be subtracted from OS clock durations
        /// in order to get an unbiased estimator of the duration of the code
        /// that is being benchmarked, excluding the cost of os_now() itself. If
        /// you use os_duration(), it will do this automatically for you.
        udipe_duration_ns_t os_offset;

        /// Width of the 99% duration confidence interval when measuring
        /// sufficiently small durations
        ///
        /// Real world clocks are not perfectly precise, and when timing a
        /// phenomenon of constant duration they will provided duration that
        /// vary by some small amount of nanoseconds. This happens due to a
        /// combination of factors:
        ///
        /// - Clocks measure timestamps with a finite granularity (the "tick"),
        ///   which means that individual clock timetamps deviate from the true
        ///   timestamp at the time where they were measured by +/- 0.5 clock
        ///   tick, and thus differences of clock timestamps deviate from the
        ///   true durations by +/- 1 clock tick.
        /// - The clock measurement process is not instantaneous, it takes a
        ///   certain amount of time. On clocks whose readout involves some form
        ///   of synchronization (some OS clocks, HPET...), this measurement
        ///   duration is not constant but may fluctuate from one clock readout
        ///   to another, and the precise point within this time window where a
        ///   timestamp is acquired may fluctuate as well.
        ///
        /// The resulting absolute clock precision acts as a lower limit on how
        /// small a duration you can reliably measure with a given relative
        /// precision. For example, if your clock has 10ns precision, then 1µs
        /// (`100*10ns`) is the lowest absolute duration that you can measure
        /// with 1% relative precision.
        udipe_duration_ns_t best_precision;

        /// Longest benchmark run duration where `best_precision` is achieved
        ///
        /// Duration measurement precision decreases above a certain benchmark
        /// run duration because a non-negligible fraction of benchmark runs
        /// start being interrupted by the OS scheduler and I/O peripheral
        /// notifications, which has many harmful side-effects:
        ///
        /// - On its own, the kernel/user mode round trip and interrupt
        ///   processing logic has nontrivial overhead, which gets accounted
        ///   into your benchmark duration even though that's not the code you
        ///   are interested in measuring. This introduces bias on the duration
        ///   output and reduces result reproducibility as the background
        ///   interrupt workload that causes this is not constant over time.
        /// - As a result of this interrupt, the OS scheduler may decide to
        ///   migrate your task to a different CPU core, which will trash all
        ///   long-lived CPU state that your application relies on for optimal
        ///   execution performance: data caches, branch predictor entries,
        ///   frequency scaling, enablement of wider SIMD instruction sets...
        ///   This will make the next few benchmark runs last longer, thus
        ///   increasing measured duration variability.
        ///     * If you are using the TSC clock, or an OS clock based on it,
        ///       then CPU migration will additionally increase duration
        ///       measurement error because the TSC clocks of different CPU
        ///       cores are not perfectly synchronized with each other.
        /// - Even if your task does not migrate to a different CU core, the
        ///   interrupt processing routine will still trash a subset of the
        ///   aforementioned CPU state, which depends on non-reproducible
        ///   details of the OS background workload.
        ///
        /// This clock figure of merit tells you the longest a benchmark run can
        /// last before these effects start measurably degrading the empirical
        /// duration measurement precision.
        ///
        /// It is measured on a trivial task (empty loop) which is minimally
        /// perturbed by interrupts and must therefore be understood as an upper
        /// bound on the optimal benchmark run duration. More complex workloads
        /// will be more severely perturbed by OS interrupts and should
        /// therefore be tuned to a shorter benchmark run duration.
        ///
        /// To find out the optimal benchmark run duration for your specific
        /// workload, you can use the following algorithm:
        ///
        /// - Quickly tune up benchmark loop iteration count until benchmark run
        ///   duration crosses this upper limit.
        /// - Tune down benchmark loop iteration count more slowly/carefully
        ///   until empirically optimal duration precision is achieved.
        /// - Finish measurement at this iteration count.
        udipe_duration_ns_t longest_optimal_duration;

        /// Duration analyzer for clock calibration data
        ///
        /// This will be used whenever the clock is recalibrated
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

    /// Check if the benchmark clock needs recalibration, if so recalibrate it
    ///
    /// This recalibration process mainly concerns \ref
    /// benchmark_clock_t::uninterrupted_window, which may evolve as the system
    /// background workload changes. But it is also a good occasion to
    /// sanity-check that other clock parameters still seem valid.
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

    /// Signed version of \ref udipe_duration_ns_t
    ///
    /// Most system clocks guarantee that if two timestamps t1 and t2 were taken
    /// in succession, t2 cannot be "lesser than" t1 and therefore t2 - t1 must
    /// be a positive or zero duration. But this monotonicity property is
    /// unfortunately partially lost we attempt to compute true user code
    /// durations, i.e. the time that elapsed between the end of the now() at
    /// the beginning of a benchmark workload and the start of now() at the end
    /// of a benchmark workload. There are two reasons for this:
    ///
    /// - Computing the user workload duration requires us to subtract the
    ///   system clock access delay, which is not perfectly known but estimated
    ///   by statistical means (and may indeed fluctuate one some uncommon
    ///   hardware configurations). If we over-estimate the clock access delay,
    ///   then negative duration measurements may happen.
    /// - System clocks do not guarantee that a timestamp will always be
    ///   acquired at the same time between the start and the end of the call to
    ///   now(), and this introduces an uncertainty window over the position of
    ///   time windows that can be as large as the clock access delay in the
    ///   worst case (though it will usually be smaller). If we take t the true
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

    /// Read the system clock
    ///
    /// The output of this function is OS-specific and unrelated to any time
    /// base you may be familiar with like UTC or local time. To minimize
    /// measurement condition drift, you should only buffer these timestamps
    /// during the measurement cycle, then post-process pairs of them into
    /// duration estimates using os_duration().
    ///
    /// \returns a timestamp representing the current time at some point between
    ///          the moment where now() was called and the moment where the call
    ///          to now() completed.
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
    /// `clock->precision` provides an estimate of the precision of the result,
    /// which should be shown in user-facing output as `duration ± precision`.
    ///
    /// \param clock is a set of clock parameters that were previously measured
    ///              via benchmark_clock_initialize().
    /// \param start is the timestamp that was measured using os_now() at the
    ///              start of the time span of interest.
    /// \param end is the timestamp that was measured using os_now() at the end
    ///            of the time span of interest (and therefore after `start`).
    ///
    /// \returns an unbiased estimate of the amount of time that elapsed between
    ///          `start` and `end`, in nanoseconds, with a precision given by
    ///          `clock->precison`.
    UDIPE_NON_NULL_ARGS
    static inline signed_duration_ns_t os_duration(benchmark_clock_t* clock,
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
        return uncorrected_ns - clock->os_offset;
    }

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

        /// Benchmark clock
        ///
        /// Used in the adjustment of benchmark parameters and interpretation of
        /// benchmark results.
        benchmark_clock_t clock;

        // TODO: Will need some kind of cache-friendly buffer setup for
        //       timestamps, may want to steal the one from buffer.h.
    };

#endif  // UDIPE_BUILD_BENCHMARKS