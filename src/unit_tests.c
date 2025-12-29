#ifdef UDIPE_BUILD_TESTS

    #include "unit_tests.h"

    #include <udipe/log.h>

    #include "address_wait.h"
    #include "benchmark.h"
    #include "bit_array.h"
    #include "buffer.h"
    #include "command.h"
    #include "error.h"
    #include "log.h"
    #include "memory.h"
    #include "name_filter.h"
    #include "thread_name.h"
    #include "visibility.h"

    #include <string.h>


    void configure_rand() {
        const char* seed_str = getenv("UDIPE_SEED");
        if (seed_str) {
            int seed = atoi(seed_str);
            ensure_gt(seed, 0);
            debugf("Reproducing execution enforced via UDIPE_SEED=%u.",
                   seed);
            srand(seed);
        } else {
            unsigned seed = time(NULL);
            debugf("To reproduce this execution, set UDIPE_SEED=%u.", seed);
            srand(seed);
        }
    }

    DEFINE_PUBLIC void udipe_unit_tests(int argc, char *argv[]) {
        // Set up logging
        logger_t logger = logger_initialize((udipe_log_config_t){ 0 });
        with_logger(&logger, {
            // Warn about bad build configurations
            #ifdef NDEBUG
                warning("You are running unit tests with debug assertions "
                        "turned off. Bugs may go undetected!");
            #endif

            // Set up name-based test filtering
            ensure_le(argc, 2);
            const char* filter_key = (argc == 2) ? argv[1] : "";
            name_filter_t filter = name_filter_initialize(filter_key);

            // Tests are ordered such that a piece of code is tested before
            // other pieces of code that may depend on it
            NAME_FILTERED_CALL(filter, thread_name_unit_tests);
            NAME_FILTERED_CALL(filter, name_filter_unit_tests);
            NAME_FILTERED_CALL(filter, address_wait_unit_tests);
            NAME_FILTERED_CALL(filter, memory_unit_tests);
            NAME_FILTERED_CALL(filter, bit_array_unit_tests);
            NAME_FILTERED_CALL(filter, buffer_unit_tests);
            NAME_FILTERED_CALL(filter, benchmark_unit_tests);
            NAME_FILTERED_CALL(filter, command_unit_tests);

            name_filter_finalize(&filter);
            info("All executed tests completed successfully!");
        });
        logger_finalize(&logger);
    }

#endif  // UDIPE_BUILD_TESTS