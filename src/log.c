#include "log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>


/// Name of a certain log level
///
/// This is a utility shared between the public udipe_log_level_name() function
/// and the internal default_log_callback(). In the former case DEFAULT is a
/// valid input, whereas in the latter case it is not a valid input. We
/// differentiate between these two cases using the allow_default parameter.
static const char* log_level_name(udipe_log_level_t level, bool allow_default) {
    switch(level) {
        case UDIPE_LOG_TRACE:
            return "TRACE";
        case UDIPE_LOG_DEBUG:
            return "DEBUG";
        case UDIPE_LOG_INFO:
            return "INFO";
        case UDIPE_LOG_WARNING:
            return "WARN";
        case UDIPE_LOG_ERROR:
            return "ERROR";
        case UDIPE_LOG_DEFAULT:
            if (allow_default) return "DEFAULT";
            __attribute__ ((fallthrough));
        default:
            fprintf(stderr, "libudipe: Called log_level_name() with invalid level %d\n", level);
            exit(EXIT_FAILURE);
    }
}


/// Default log callback
///
/// This is the \ref udipe_log_config_t::callback that is used when the user
/// does not specify one. It logs to `stderr` with basic formatting.
static void default_log_callback(void* /* context */,
                          udipe_log_level_t level,
                          const char location[],
                          const char message[]) {
    // Compute log timestamp and its display representation
    clock_t timestamp = clock();
    assert(timestamp >= 0);
    size_t time_secs = (size_t)(timestamp / CLOCKS_PER_SEC);
    size_t time_fract = (size_t)(timestamp) * 1000000 / CLOCKS_PER_SEC;

    // Translate log level into a textual representation
    const char* level_string = log_level_name(level, false);

    // Display the log on stderr
    fprintf(stderr, "[%5zu.%06zu %5s %s] %s\n", time_secs, time_fract, level_string, location, message);
}


UDIPE_PUBLIC const char* udipe_log_level_name(udipe_log_level_t level) {
    return log_level_name(level, true);
}


logger_t log_initialize(udipe_log_config_t config) {
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
        fprintf(stderr, "libudipe: Called log_initialize() with invalid min_level %d\n", config.min_level);
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
            fprintf(stderr, "libudipe: Called validate_log() with invalid level %d\n", level);
            exit(EXIT_FAILURE);
        };
    }
#endif
