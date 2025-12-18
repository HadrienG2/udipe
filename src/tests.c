#ifdef UDIPE_BUILD_TESTS

    #include <udipe/log.h>
    #include <udipe/tests.h>

    #include "atomic_wait.h"
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


    DEFINE_PUBLIC void udipe_unit_tests(int argc, char *argv[]) {
        // Set up logging
        udipe_log_config_t log_config;
        memset(&log_config, 0, sizeof(udipe_log_config_t));
        logger_t logger = logger_initialize(log_config);
        with_logger(&logger, {
            // Set up name-based test filtering
            ensure_le(argc, 2);
            const char* filter_key = (argc == 2) ? argv[1] : "";
            name_filter_t filter = name_filter_initialize(filter_key);

            // Tests are ordered such that a piece of code is tested before
            // other pieces of code that may depend on it
            NAME_FILTERED_CALL(filter, thread_name_unit_tests);
            NAME_FILTERED_CALL(filter, name_filter_unit_tests);
            NAME_FILTERED_CALL(filter, atomic_wait_unit_tests);
            NAME_FILTERED_CALL(filter, memory_unit_tests);
            NAME_FILTERED_CALL(filter, bit_array_unit_tests);
            NAME_FILTERED_CALL(filter, buffer_unit_tests);
            NAME_FILTERED_CALL(filter, command_unit_tests);
            name_filter_finalize(&filter);
            info("All executed tests completed successfully!");
        });
        logger_finalize(&logger);
    }

#endif  // UDIPE_BUILD_TESTS