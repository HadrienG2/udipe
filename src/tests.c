#ifdef UDIPE_BUILD_TESTS

    #include <udipe/log.h>
    #include <udipe/tests.h>

    #include "bit_array.h"
    #include "buffer.h"
    #include "command.h"
    #include "countdown.h"
    #include "log.h"
    #include "visibility.h"

    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <strings.h>


    DEFINE_PUBLIC void udipe_unit_tests() {
        udipe_log_config_t log_config;
        memset(&log_config, 0, sizeof(udipe_log_config_t));

        logger_t logger = log_initialize(log_config);
        with_logger(&logger, {
            bit_array_unit_tests();
            buffer_unit_tests();
            command_unit_tests();
            countdown_unit_tests();
        });
    }

#endif  // UDIPE_BUILD_TESTS