#pragma once

#include <udipe.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <threads.h>


// === Logging ===

//< Validate and apply user-provided logging configuration
//<
//< This should be done as early as possible during the `udipe` configuration
//< process in order to reduce the amount of code that cannot perform logging.
void setup_log(udipe_log_level_t level, udipe_log_callback_t callback);

//< Currently configured logging threshold
//<
//< This value is only valid after `setup_log()` has been called.
extern atomic_int MIN_LOG_LEVEL;

//< Validate that a logging call is valid (`Debug` builds only)
#ifndef NDEBUG
    void validate_log(udipe_log_level_t level);
#endif

//< Decide if a user log should be emitted
//<
//< This function can only be called after `setup_log()` has been called.
static inline bool should_log(udipe_log_level_t level) {
    #ifndef NDEBUG
        validate_log(level);
    #endif
    // MIN_LOG_LEVEL is not used to synchronize reads and writes to other
    // variables, so relaxed ordering is fine.
    return level >= atomic_load_explicit(&MIN_LOG_LEVEL, memory_order_relaxed);
}

//< Unconditionally emit an individual log statement (backend of `log()` macro)
//<
//< This function can only be called after `setup_log()` has been called.
void do_log(udipe_log_level_t level, const char* location, const char* message);

//< Log a message if `level` is above configured logging threshold
//<
//< You should prefer using the log level specific macros `error()`,
//< `warning()`, `info()`, `debug()` and `trace()` instead of this one, except
//< in specific circumstances where you truly want to dynamically adjust the log
//< level depending on unpredictable runtime circumstances.
//<
//< This macro can only be used after `setup_log()` has been called.
#define log(level, message)  \
    do {  \
        const LogLevel lvl = (level);  \
        if (should_log(lvl)) do_log(lvl, __func__, (message));  \
    } while(false)

//< Log a `TRACE` message (see `log()` for more info on general semantics)
#define trace(message)  log(UDIPE_LOG_TRACE, (message))
//< Log a `DEBUG` message (see `log()` for more info on general semantics)
#define debug(message)  log(UDIPE_LOG_DEBUG, (message))
//< Log an `INFO` message (see `log()` for more info on general semantics)
#define info(message)  log(UDIPE_LOG_INFO, (message))
//< Log a `WARNING` message (see `log()` for more info on general semantics)
#define warning(message)  log(UDIPE_LOG_WARNING, (message))
//< Log an `ERROR` message (see `log()` for more info on general semantics)
#define error(message)  log(UDIPE_LOG_ERROR, (message))
