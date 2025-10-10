#include "log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>


/// Default log callback
///
/// This is the \ref udipe_log_config_t::callback that is used when the user
/// does not specify one. It logs to `stderr` with basic formatting.
static void default_log_callback(void* /* context */,
                          udipe_log_level_t level,
                          const char location[],
                          const char message[]) {
    // Timestamp log as early as possible
    clock_t timestamp = clock();
    assert(timestamp > 0);
    size_t time_secs = (size_t)(timestamp / CLOCKS_PER_SEC);
    size_t time_fract = (size_t)(timestamp % CLOCKS_PER_SEC);

    // Translate log level into a textual representation
    const char* level_string;
    switch (level) {
    case UDIPE_LOG_TRACE:
        level_string = "TRACE";
        break;
    case UDIPE_LOG_DEBUG:
        level_string = "DEBUG";
        break;
    case UDIPE_LOG_INFO:
        level_string = "INFO";
        break;
    case UDIPE_LOG_WARNING:
        level_string = "WARNING";
        break;
    case UDIPE_LOG_ERROR:
        level_string = "ERROR";
        break;
    #ifndef NDEBUG
        default:
            fprintf(stderr, "Invalid log level %d\n", level);
            exit(EXIT_FAILURE);
    #endif
    };

    // Display the log on stderr
    fprintf(stderr, "[%5u.%06u %s %s] %s\n", time_secs, time_fract, level_string, location, message);
}


logger_t setup_log(udipe_log_config_t config) {
    // Select and configure log level
    switch (config.min_level) {
    case UDIPE_LOG_TRACE:
    case UDIPE_LOG_DEBUG:
    case UDIPE_LOG_INFO:
    case UDIPE_LOG_WARNING:
    case UDIPE_LOG_ERROR:
        break;
    case UDIPE_LOG_DEFAULT:
        #ifdef NDEBUG
            config.min_level = UDIPE_LOG_INFO;
        #else
            config.min_level = UDIPE_LOG_DEBUG;
        #endif
        break;
    default:
        fprintf(stderr, "Attempted to configure udipe logging with invalid min_level %d\n", config.min_level);
        exit(EXIT_FAILURE);
    }

    // Configure logging callback
    if (!config.callback) config.callback = default_log_callback;
    return config;
}


thread_local const logger_t* udipe_thread_logger = NULL;


#ifndef NDEBUG
    void validate_log(udipe_log_level_t level) {
        assert(("No logging allowed outside of with_log()", udipe_thread_logger));
        switch (level) {
        case UDIPE_LOG_TRACE:
        case UDIPE_LOG_DEBUG:
        case UDIPE_LOG_INFO:
        case UDIPE_LOG_WARNING:
        case UDIPE_LOG_ERROR:
            break;
        default:
            fprintf(stderr, "Encountered log() call with invalid log level %d\n", level);
            exit(EXIT_FAILURE);
        };
    }
#endif
