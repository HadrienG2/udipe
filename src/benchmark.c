#ifdef UDIPE_BUILD_BENCHMARKS

    #include "benchmark.h"

    #include <udipe/log.h>

    #include "memory.h"
    #include "visibility.h"

    #include <string.h>


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

            // TODO: Clock calibration. The goal here is to...
            //
            //       - Perform any syscall needed to convert a pair of OS time
            //         measurements into a nanosecond duration, cache the
            //         results in udipe_benchmark_t.
            //       - Measure the clock precision, which is the minimal nonzero
            //         duration that can be measured. This is either equal to
            //         the clock resolution or measurement overhead, whichever
            //         is greater. They are the same from our perspective
            //         because we cannot presume that a clock reading will
            //         always occur at the same point within the measurement
            //         overhead window and must pessimistically assume that it
            //         can occur at any point within this window.
            //          - We'll later use as a hard lower bound in workload
            //            iteration count autotuning: workloads should last at
            //            least 100x the clock precision, so that the associated
            //            time measurements have at least 1% precision.
            //       - Measure the 1% worst-case OS interrupt periods as the 99%
            //         quantile of delays between two abnormally high
            //         time measurements on a trivial workload.
            //          -  We'll late use this as a soft upper bound in workload
            //             iteration count autotuning. Workloads should be tuned
            //             to last less than 1/100 of the typical interrupt
            //             period in order to ensure that only ~1% of timing
            //             samples will be polluted by interference from OS
            //             interrupts, and thus that such pollution will be
            //             flushed from the final distribution. If a user
            //             workload lasts long enough at iteration count 1 that
            //             this is not possible, then the benchmark harness
            //             should print a warning before proceeding with
            //             iteration count 1.
            //
            //       Linux backend should use CLOCK_MONOTONIC_RAW, Windows
            //       backend shoud use QueryPerformanceCounter().
            //
            // TODO: Later, when measuring workloads, measure at iteration count
            //       1 and 2+ in order to subtract the basic overhead of
            //       setup/teardown from the measurement and only get the
            //       contribution that varies with iteration count, which
            //       represents the true user workload.
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
                // TODO: Make this a realtime thread with a priority given by
                //       environment variable UDIPE_PRIORITY_BENCHMARK if set,
                //       by default halfway from the bottom of the realtime
                //       priority range to UDIPE_PRIORITY_WORKER, which itself
                //       is by default halfway through the entire realtime
                //       priority range. Start writing an environment variable
                //       doc that covers this + UDIPE_LOG.
                callable(context, benchmark);
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