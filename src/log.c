#include "log.h"

#include "error.h"

#include <assert.h>
#include <errno.h>
#include <linux/prctl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>


/// Name of a certain log level
///
/// This is a utility shared between the public udipe_log_level_name() function
/// and the internal default_log_callback(). In the former case \link
/// #UDIPE_DEFAULT_LOG_LEVEL `DEFAULT`\endlink is a valid input, whereas in the
/// latter case it is not a valid input. We differentiate between these two
/// cases using the `allow_default` parameter.
static const char* log_level_name(udipe_log_level_t level, bool allow_default) {
    switch(level) {
    case UDIPE_TRACE:
        return "TRACE";
    case UDIPE_DEBUG:
        return "DEBUG";
    case UDIPE_INFO:
        return "INFO";
    case UDIPE_WARNING:
        return "WARN";
    case UDIPE_ERROR:
        return "ERROR";
    case UDIPE_DEFAULT_LOG_LEVEL:
        if (allow_default) return "DEFAULT";
        #ifdef __GNUC__
            __attribute__ ((fallthrough));
        #endif
    default:
        fprintf(stderr,
                "libudipe: Called log_level_name() with invalid level %d!\n",
                level);
        exit(EXIT_FAILURE);
    }
}

/// Truth that stderr is connected to a TTY
///
/// Set upon default_log_callback() initialization. Used to decide whether
/// stderr logs should use ANSI color highlighting.
#ifdef __unix__
    static atomic_bool stderr_is_tty;
#endif

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

    // Give each log a color on Unix systems only
    bool use_colors = false;
    const char* level_color = "";
    #ifdef __unix__
        use_colors = atomic_load_explicit(&stderr_is_tty, memory_order_relaxed);
        if (use_colors) {
            switch(level) {
            case UDIPE_TRACE:
                level_color = "\033[36m";
                break;
            case UDIPE_DEBUG:
                level_color = "\033[34m";
                break;
            case UDIPE_INFO:
                level_color = "\033[32m";
                break;
            case UDIPE_WARNING:
                level_color = "\033[93;1m";
                break;
            case UDIPE_ERROR:
                level_color = "\033[91;1m";
                break;
            case UDIPE_DEFAULT_LOG_LEVEL:
            default:
                fprintf(stderr,
                        "libudipe: Called default_log_callback() "
                        "with invalid level %d!\n",
                        level);
                exit(EXIT_FAILURE);
            }
        }
    #endif

    // Query the current thread's name
    // TODO: Add Windows version once Windows CI build is running
    char thread_name[16];
    int result = prctl(PR_GET_NAME, thread_name);
    assert(("No documented failure case", result == 0));

    // Display the log on stderr
    if (use_colors) {
        fprintf(stderr,
                "[%5zu.%06zu %s%5s\033[0m] \033[33m%s %s: "
                "%s%s\033[0m\n",
                secs, microsecs, level_color, level_name, thread_name, location,
                level_color, message);
    } else {
        fprintf(stderr,
                "[%5zu.%06zu %5s] %s %s: %s\n",
                secs, microsecs, level_name, thread_name, location, message);
    }
}

UDIPE_PUBLIC const char* udipe_log_level_name(udipe_log_level_t level) {
    return log_level_name(level, true);
}

logger_t log_initialize(udipe_log_config_t config) {
    // Select and configure log level
    switch (config.min_level) {
    case UDIPE_TRACE:
    case UDIPE_DEBUG:
    case UDIPE_INFO:
    case UDIPE_WARNING:
    case UDIPE_ERROR:
        break;
    case UDIPE_DEFAULT_LOG_LEVEL:
        #ifdef NDEBUG
            config.min_level = UDIPE_INFO;
        #else
            config.min_level = UDIPE_DEBUG;
        #endif
        break;
    default:
        fprintf(stderr,
                "libudipe: Called log_initialize() with invalid min_level %d!\n",
                config.min_level);
        exit(EXIT_FAILURE);
    }

    // Configure logging callback
    if (!config.callback) {
        config.callback = default_log_callback;
        #ifdef __unix__
            int result = isatty(STDERR_FILENO);
            if (result == 1) {
                atomic_store_explicit(&stderr_is_tty,
                                      true,
                                      memory_order_relaxed);
            } else {
                assert(("No other return value expected", result == 0));
                assert(("No other error expected", errno == ENOTTY));
                atomic_store_explicit(&stderr_is_tty,
                                      false,
                                      memory_order_relaxed);
            }
        #endif
    }
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
    // This will log a static string on error, but that's fine because static
    // string logging does not go through the formatted string code path.
    exit_on_negative(result, "Failed to evaluate message size!");
    size_t message_size = 1 + (size_t)result;

    // Allocate the message buffer
    char* message = alloca(message_size);
    exit_on_null(message, "Failed to allocate message buffer!");

    // Generate the log string
    result = vsnprintf(message, message_size, format, args2);
    va_end(args2);
    exit_on_negative(result, "Failed to generate message!");

    // Emit the log
    (udipe_thread_logger->callback)(udipe_thread_logger->context,
                                    level,
                                    location,
                                    message);
}

thread_local const logger_t* udipe_thread_logger = NULL;

thread_local udipe_log_level_t udipe_thread_log_level = UDIPE_INFO;

#ifndef NDEBUG
    void validate_log(udipe_log_level_t level) {
        assert(("Should not make logging calls "
                "outside a with_logger() scope",
                udipe_thread_logger));
        switch (level) {
        case UDIPE_TRACE:
        case UDIPE_DEBUG:
        case UDIPE_INFO:
        case UDIPE_WARNING:
        case UDIPE_ERROR:
            break;
        default:
            fprintf(stderr,
                    "libudipe: Called validate_log() with invalid level %d!\n",
                    level);
            exit(EXIT_FAILURE);
        }
    }
#endif  // NDEBUG

void trace_expr_impl(const char* format_template,
                     const char* expr_format,
                     ...) {
    // Determine the format string size
    int result = snprintf(NULL, 0, format_template, expr_format);
    exit_on_negative(result, "Failed to evaluate format string size!");
    size_t format_size = 1 + (size_t)result;

    // Allocate the format string buffer
    char* format = alloca(format_size);
    exit_on_null(format, "Failed to allocate format string!");

    // Generate the format string
    result = snprintf(format, format_size, format_template, expr_format);
    exit_on_negative(result, "Failed to generate format string!");

    // Get two copies of the variadic arguments
    va_list args1, args2;
    va_start(args1, expr_format);
    va_copy(args2, args1);

    // Determine the trace message size
    result = vsnprintf(NULL, 0, format, args1);
    va_end(args1);
    exit_on_negative(result, "Failed to evaluate trace message size!");
    size_t trace_message_size = 1 + (size_t)result;

    // Allocate the trace message buffer
    char* trace_message = alloca(trace_message_size);
    exit_on_null(trace_message, "Failed to allocate trace message buffer!");

    // Generate the trace message
    result = vsnprintf(trace_message, trace_message_size, format, args2);
    va_end(args2);
    exit_on_negative(result, "Failed to generate trace message!");

    // Finally log the trace message
    trace(trace_message);
}
