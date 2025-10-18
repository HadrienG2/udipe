#include "log.h"

#include "error.h"

#include <assert.h>
#include <linux/prctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
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
    size_t secs = (size_t)(timestamp / CLOCKS_PER_SEC);
    size_t microsecs = (size_t)(timestamp) * 1000000 / CLOCKS_PER_SEC;

    // Translate log level into a textual representation
    const char* level_name = log_level_name(level, false);

    // Give each log a color
    const char* level_color;
    switch(level) {
    case UDIPE_LOG_TRACE:
        level_color = "\033[36m";
        break;
    case UDIPE_LOG_DEBUG:
        level_color = "\033[34m";
        break;
    case UDIPE_LOG_INFO:
        level_color = "\033[32m";
        break;
    case UDIPE_LOG_WARNING:
        level_color = "\033[33;1m";
        break;
    case UDIPE_LOG_ERROR:
        level_color = "\033[31;1m";
        break;
    case UDIPE_LOG_DEFAULT:
    default:
        fprintf(stderr, "libudipe: Called default_log_callback() with invalid level %d\n", level);
        exit(EXIT_FAILURE);
    }

    // Query the current thread's name
    char thread_name[16];
    int result = prctl(PR_GET_NAME, thread_name);
    assert(("Should never fail with a valid buffer", result == 0));

    // Display the log on stderr
    // TODO: Disable ANSI colors when not connected to a TTY.
    fprintf(stderr,
            "[\033[2m%5zu.%06zu\033[0m %s%5s \033[0;35m%16s/%s\033[0m] %s\n",
            secs, microsecs, level_color, level_name, thread_name, location, message);
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


LOGF_IMPL_ATTRIBUTES
void logf_impl(udipe_log_level_t level,
               const char* location,
               const char* format,
               ...) {
    // Get two copies of the variadic arguments
    va_list args1, args2;
    va_start(args1, format);
    va_copy(args2, args1);

    // Determine the message size
    int result = vsnprintf(NULL, 0, format, args1);
    va_end(args1);
    // This will log a static string on error, but that's fine as static string
    // logging does not go through the formatted string code path.
    exit_on_negative(result, "Failed to evaluate message size");
    size_t message_size = 1 + (size_t)result;

    // Allocate the message buffer
    char* message = alloca(message_size);
    exit_on_null(message, "Failed to allocate message buffer");

    // Generate the log string
    result = vsnprintf(message, message_size, format, args2);
    va_end(args2);
    exit_on_negative(result, "Failed to generate message");

    // Emit the log
    (udipe_thread_logger->callback)(udipe_thread_logger->context,
                                    level,
                                    location,
                                    message);
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
