#ifdef UDIPE_BUILD_BENCHMARKS

    #include <udipe/benchmarks.h>

    #include <udipe/log.h>

    #include "error.h"
    #include "log.h"
    #include "name_filter.h"
    #include "visibility.h"

    #include <string.h>


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

        // TODO: Clock and friends
    };

    DEFINE_PUBLIC
    UDIPE_NON_NULL_ARGS
    UDIPE_NON_NULL_RESULT
    udipe_benchmark_t* udipe_benchmark_initialize(int argc, char *argv[]) {
        // Our goal is to fill up this struct
        udipe_benchmark_t* benchmark = (udipe_benchmark_t*)malloc(sizeof(udipe_benchmark_t));
        memset(benchmark, 0, sizeof(udipe_benchmark_t));

        // Set up logging
        benchmark->logger = logger_initialize((udipe_log_config_t){ 0 });
        with_logger(&benchmark->logger, {
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
            if (matches) callable(context, benchmark);
        });
        return matches;
    }

    DEFINE_PUBLIC
    UDIPE_NON_NULL_ARGS
    void udipe_benchmark_finalize(udipe_benchmark_t** benchmark) {
        with_logger(&(*benchmark)->logger, {
            info("All micro-benchmarks completed successfully!");

            debug("Finalizing benchmark name filter...");
            name_filter_finalize(&(*benchmark)->filter);

            debug("Finalizing logger, deallocating, and poisoning...");
        });
        logger_finalize(&(*benchmark)->logger);
        free(*benchmark);
        *benchmark = NULL;
    }

    DEFINE_PUBLIC void udipe_micro_benchmarks(udipe_benchmark_t* benchmark) {
        // Microbenchmarks are ordered such that a piece of code is
        // benchmarked before other pieces of code that may depend on it
        // TODO: UDIPE_BENCHMARK(benchmark, xyz_micro_benchmarks, NULL);
    }

#endif  // UDIPE_BUILD_BENCHMARKS