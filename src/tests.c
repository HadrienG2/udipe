#ifdef UDIPE_BUILD_TESTS

    #include <udipe/log.h>
    #include <udipe/tests.h>

    #include "bitmap.h"
    #include "log.h"

    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <strings.h>

    UDIPE_PUBLIC void udipe_unit_tests() {
        udipe_log_config_t log_config;
        memset(&log_config, 0, sizeof(udipe_log_config_t));

        const char* log_level = getenv("LOG_LEVEL");
        if (log_level) {
            if (strcasecmp(log_level, "ERROR") == 0) {
                log_config.min_level = UDIPE_LOG_ERROR;
            } else if (strcasecmp(log_level, "WARNING") == 0) {
                log_config.min_level = UDIPE_LOG_WARNING;
            } else if (strcasecmp(log_level, "INFO") == 0) {
                log_config.min_level = UDIPE_LOG_INFO;
            } else if (strcasecmp(log_level, "DEBUG") == 0) {
                log_config.min_level = UDIPE_LOG_DEBUG;
            } else if (strcasecmp(log_level, "TRACE") == 0) {
                log_config.min_level = UDIPE_LOG_TRACE;
            } else {
                fprintf(stderr, "Error: Invalid LOG_LEVEL %s\n", log_level);
                exit(EXIT_FAILURE);
            }
        }

        logger_t logger = log_initialize(log_config);
        with_logger(&logger, {
            bitmap_unit_tests();
        });
    }

#endif