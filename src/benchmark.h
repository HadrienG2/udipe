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

    /// Benchmark clock
    ///
    /// This is a cache of informations about the system's high resolution clock
    /// that are needed when using it for precision benchmarking, but can only
    /// be queried at runtime through a relatively time-consuming process whose
    /// results should be cached and reused across benchmarks.
    typedef struct benchmark_clock_s {
        /// Median system clock access delay in nanoseconds, rounded to nearest
        ///
        /// This is the time that typically elapses between the moment where a
        /// clock readout is requested from the operating system and the moment
        /// where the associated timestamp is returned to the application.
        ///
        /// In other words, if a single CPU core reads the system clock in a
        /// loop and the overhead of looping is considered negligible (which is
        /// normally a reasonable assumption considering superscalar execution
        /// and the relative cost of clock readouts with respect to basic
        /// looping), then the typical number of clock readouts achieved every
        /// second should be the inverse of this number, multiplied by one
        /// billion to account for nanosecond/second conversions.
        ///
        /// When measuring benchmark durations by subtracting system clock
        /// timestamps, this offset should be subtracted from the raw difference
        /// of clock timestamps in order to get an unbiased estimator of the
        /// actual duration of interest, which is that between the **end** of
        /// the first clock measurement and the **start** of the second clock
        /// measurement. The provided duration() utility handles this
        /// automatically for you.
        udipe_duration_ns_t access_delay;

        /// Median system clock precision in nanoseconds, rounded up
        ///
        /// This is the typical uncertainty of clock measurements, i.e. the
        /// amount by which individual clock readouts tend to deviate from an
        /// ideal model where they are resolved to the nearest nanosecond and
        /// taken at a consistent point of the access delay window.
        ///
        /// The reason we care about this model is that it is the model under
        /// which we implicitly operate when we estimate the duration of an
        /// operation as the difference of two clock timestamps minus the clock
        /// access delay.
        ///
        /// Benchmark iteration counts should be tuned up until durations are an
        /// appropriate factor away from this figure of merit to achieve the
        /// desired timing precision. For example, if you want to measure
        /// timings with +/- 1% precision, then you should tune the benchmark
        /// iteration count such that a typical benchmark iteration takes at
        /// least 100x longer than the system clock's precision.
        udipe_duration_ns_t precision;

        /// Maximum benchmark duration with 1 % OS interrupt frequency,
        /// rounded down
        ///
        /// Application code can only use the CPU exclusively for a short while
        /// before it is interrupted by either 1/the preemptive OS scheduler
        /// (which normally checks for other runnable tasks at a tick rate of
        /// around 1 kHz or less) or 2/the processing of another hardware
        /// interrupt, typically from some system I/O device doing background
        /// work like network interfaces processing random Internet traffic.
        ///
        /// These interruptions are a problem for benchmarking because our
        /// timing measurements account for them whenever they occur between two
        /// clock readouts, yet they are not the code whose performance we are
        /// trying to measure and therefore act as an additive contribution with
        /// respect to the application code execution duration that we are
        /// interested in, which is unpredictable because it depends on the
        /// current system configuration/environment and may therefore vary from
        /// one benchmark run another.
        ///
        /// To avoid this problem, we attempt to tune down benchmark iteration
        /// counts (and therefore benchmark duration) until the probability that
        /// a purely CPU-bound benchmark is perturbed by system activity appears
        /// to become less than 1%. Failure to do so is not fatal since not all
        /// benchmarked workloads of interest are short enough for this, and
        /// some timing sources do not allow us to satisfy both this constraint
        /// and the precision one above, which is more important.
        ///
        /// As this tuning is time-consuming and requires a task with very
        /// predictable timing to distinguish OS interference from normal code
        /// timing variability, we perform it at benchmark harness
        /// initialization time on a known-good task, under the assumption that
        /// the system load pattern will remain constant throughout the rest of
        /// the benchmark run and does not depend on the workload at hand.
        udipe_duration_ns_t uninterrupted_window;

        #ifdef _WIN32
            /// Frequency of the performance counter clock in ticks per second
            ///
            /// To convert performance counter ticks to nanoseconds, multiply
            /// the number of ticks by one billion (`1000*1000*1000`) then
            /// divide it by this number.
            uint64_t frequency;
        #endif
    } benchmark_clock_t;

    // TODO: benchmark_clock_initialize(), benchmark_clock_finalize(), and also
    //       benchmark_clock_calibrate() that should be called automatically at
    //       regular (user-configurable?) time intervals during long-running
    //       benchmarks, between two timing data acquisition cycles.

    /// Raw system clock timestamp
    ///
    /// This type is OS-specific and its values should not be used directly.
    /// Instead they are meant to be read with instant() during a benchmark,
    /// buffered for a while, then post-processed using the duration() function
    /// which computes duration estimates from pairs of timestamps.
    #ifdef _POSIX_TIMERS
        typedef struct timespec instant_t;
    #elif defined(_WIN32)
        typedef LONG_INTEGER instant_t;
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif

    /// Read the system clock
    ///
    /// The output of this function is OS-specific and unrelated to any time
    /// base you may be familiar with like UTC or local time. The only thing you
    /// should do with these outputs is buffer them for a while, then
    /// post-process pairs of them into duration estimates using duration().
    ///
    /// \returns a timestamp representing the current time at some point between
    ///          the moment where now() was called and the moment where the call
    ///          to now() completed.
    static inline instant_t now() {
        instant_t result;
        #if defined(_POSIX_TIMERS)
            clockid_t clock;
            #ifdef __linux__
                clock = CLOCK_MONOTONIC_RAW;
            #elif defined(_POSIX_MONOTONIC_CLOCK)
                clock = CLOCK_MONOTONIC;
            #else
                clock = CLOCK_REALTIME;
            #endif
            exit_on_negative(clock_gettime(clock, &result),
                             "Failed to read the system clock");
        #elif defined(_WIN32)
            win32_exit_on_zero(QueryPerformanceCounter(&result),
                               "Failed to read the system clock");
        #else
            #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
        #endif
        return result;
    }

    /// Estimate the elapsed time between two system clock readouts
    ///
    /// Given the `start` and `end` timestamps returned by two calls to now(),
    /// where `start` was measured before `end`, this estimates the amount of
    /// time that elapsed between the end of the call to now() that returned
    /// `start` and the beginning of the call to now() that returned `end`.
    ///
    /// `clock->precision` provides an estimate of the precision of the result,
    /// which should be shown in user-facing output as `duration ± precision`.
    ///
    /// \param clock is a set of clock parameters that were previously measured
    ///              via benchmark_clock_initialize().
    /// \param start is the timestamp that was measured using now() at the start
    ///              of the time span of interest.
    /// \param end is the timestamp that was measured using now() at the end of
    ///            the time span of interest, which must be after the time where
    ///            `start` was measured.
    ///
    /// \returns an unbiased estimate of the amount of time that elapsed between
    ///          `start` and `end`, in nanoseconds, with a precision given by
    ///          `clock->precison`.
    UDIPE_NON_NULL_ARGS
    static inline udipe_duration_ns_t duration(benchmark_clock_t* clock,
                                               instant_t start,
                                               instant_t end) {
        const uint64_t nano = 1000 * 1000 * 1000;
        #if defined(_POSIX_TIMERS)
            assert(
                start.tv_sec < end.tv_sec
                || (start.tv_sec == end.tv_sec && start.tv_nsec <= end.tv_nsec)
            );
            uint64_t secs = end.tv_sec - start.tv_sec;
            if (start.tv_nsec > end.tv_nsec) secs -= 1;
            int nanosecs = abs((int)end.tv_nsec - (int)start.tv_nsec);
            return secs * nano + (udipe_duration_ns_t)nanosecs;
        #elif defined(_WIN32)
            assert(end.QuadPart >= start.QuadPart);
            return (end.QuadPart - start.QuadPart) * nano / clock->frequency;
        #else
            #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
        #endif
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

        // TODO: Will need some kind of cache-friendly buffer setup, may want to
        //       steal the one from buffer.h.
    };

#endif  // UDIPE_BUILD_BENCHMARKS