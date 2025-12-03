#ifdef UDIPE_BUILD_TESTS

    #include <udipe/log.h>
    #include <udipe/tests.h>

    #include "bit_array.h"
    #include "buffer.h"
    #include "command.h"
    #include "log.h"
    #include "sys/memory.h"
    #include "sys/thread_name.h"
    #include "visibility.h"

    #include <string.h>


    DEFINE_PUBLIC void udipe_unit_tests() {
        udipe_log_config_t log_config;
        memset(&log_config, 0, sizeof(udipe_log_config_t));

        logger_t logger = logger_initialize(log_config);
        with_logger(&logger, {
            // Tests are ordered such that a piece of code is tested before
            // other pieces of code that may depend on it
            thread_name_unit_tests();
            memory_unit_tests();
            bit_array_unit_tests();
            buffer_unit_tests();
            command_unit_tests();
        });
        logger_finalize(&logger);
    }

#endif  // UDIPE_BUILD_TESTS