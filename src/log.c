#include "udipe_internal.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>


atomic_int MIN_LOG_LEVEL = INT_MIN;

//< Pointer to the udipe_log_callback_t that should be used for logging
atomic_uintptr_t LOG_CALLBACK = 0;


//< Default logging logic when the user does not override it
void default_log_callback(udipe_log_level_t level, const char* location, const char* message) {
    // Timestamp log as early as possible
    clock_t timestamp = clock();
    clock_t time_secs = timestamp / CLOCKS_PER_SEC;
    clock_t time_fract = timestamp % CLOCKS_PER_SEC;

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


void setup_log(udipe_log_level_t level, udipe_log_callback_t callback) {
    // Select and configure log level, memory ordering is enforced below
    switch (level) {
    case UDIPE_LOG_TRACE:
    case UDIPE_LOG_DEBUG:
    case UDIPE_LOG_INFO:
    case UDIPE_LOG_WARNING:
    case UDIPE_LOG_ERROR:
        atomic_store_explicit(&MIN_LOG_LEVEL, level, memory_order_relaxed);
        break;
    case UDIPE_LOG_DEFAULT:
        #ifdef NDEBUG
            atomic_store_explicit(&MIN_LOG_LEVEL, UDIPE_LOG_INFO, memory_order_relaxed);
        #else
            atomic_store_explicit(&MIN_LOG_LEVEL, UDIPE_LOG_DEBUG, memory_order_relaxed);
        #endif
        break;
    default:
        fprintf(stderr, "Attempted to set up udipe with invalid log level %d\n", level);
        exit(EXIT_FAILURE);
    }

    // Configure logging callback + make sure any thread which sees the logging
    // callback also sees all previous logging setup steps via release ordering
    if (!callback) callback = default_log_callback;
    #ifdef NDEBUG
        atomic_store_explicit(&LOG_CALLBACK, (uintptr_t)callback, memory_order_release);
    #else
        uintptr_t old_callback = atomic_exchange_explicit(&LOG_CALLBACK, (uintptr_t)callback, memory_order_release);
        // FIXME: Consider using a non-global logger instead, but this will
        //        require each and every log function to include a context
        //        parameter which gets painful quickly. A possible middle ground
        //        is to configure logging using a thread-local via some
        //        log_enter()/log_exit() pair, that can be abstracted out using
        //        defer-like functionality or a WITH_LOGGER(ctx, ...) variadic
        //        macro.
        assert(("Cannot setup udipe twice", old_callback == 0));
    #endif
}


#ifndef NDEBUG
    void validate_log(udipe_log_level_t level) {
        // Debug assertions cannot enforce meaningful memory ordering as they
        // are not present in all builds
        uintptr_t callback = atomic_load_explicit(&LOG_CALLBACK, memory_order_relaxed);
        assert(("No logging allowed before setup_log() is called", callback));
        switch (level) {
        case UDIPE_LOG_TRACE:
        case UDIPE_LOG_DEBUG:
        case UDIPE_LOG_INFO:
        case UDIPE_LOG_WARNING:
        case UDIPE_LOG_ERROR:
            break;
        default:
            fprintf(stderr, "Encountereds log() call with invalid log level %d\n", level);
            exit(EXIT_FAILURE);
        };
    }
#endif


void do_log(udipe_log_level_t level, const char* location, const char* message) {
    // Acquire ordering to ensure that if log callback is visible, logger state
    // is visible too from the perspective of the current thread
    udipe_log_callback_t callback = (udipe_log_callback_t)atomic_load_explicit(&LOG_CALLBACK, memory_order_acquire);
    assert(("No logging allowed before setup_log() is called", callback));
    callback(level, location, message);
}
