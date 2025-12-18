#ifdef UDIPE_BUILD_BENCHMARKS

    #include <udipe/micro_benchmarks.h>

    #include <udipe/log.h>

    #include "error.h"
    #include "log.h"
    #include "name_filter.h"
    #include "visibility.h"

    #include <string.h>


    DEFINE_PUBLIC void udipe_micro_benchmarks(int argc, char *argv[]) {
        // Set up logging
        udipe_log_config_t log_config;
        memset(&log_config, 0, sizeof(udipe_log_config_t));
        logger_t logger = logger_initialize(log_config);
        with_logger(&logger, {
            // Warn about bad build/runtime configurations
            #ifndef NDEBUG
                warning("You are running micro-benchmarks on a Debug build. "
                        "This will bias measurements!");
            #else
                if (logger.min_level <= UDIPE_DEBUG) {
                    warning("You are running micro-benchmarks with DEBUG/TRACE "
                            "logging enabled. This will bias measurements!");
                }
            #endif

            // Set up name-based benchmark filtering
            ensure_le(argc, 2);
            const char* filter_key = (argc == 2) ? argv[1] : "";
            name_filter_t filter = name_filter_initialize(filter_key);

            // Microbenchmarks are ordered such that a piece of code is
            // benchmarked before other pieces of code that may depend on it
            // TODO: NAME_FILTERED_CALL(filter, xyz_micro_benchmarks);

            name_filter_finalize(&filter);
            info("All micro-benchmarks executed successfully!");
        });
        logger_finalize(&logger);
    }

#endif  // UDIPE_BUILD_BENCHMARKS