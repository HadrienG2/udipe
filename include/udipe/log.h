#pragma once

//! \file
//! \brief Logging configuration
//!
//! This header is the home of \ref udipe_log_config_t, the subset of \ref
//! udipe_config_t that controls `libudipe`'s logging behavior.

//! \example log_to_tempfile.c
//!
//! This example demonstrates a non-default \ref udipe_log_config_t setup that
//! enables `TRACE` logging and leverages a user-defined \ref
//! udipe_log_callback_t to write logs to a temporary file instead of stderr.

#include "visibility.h"


/// Log level/priority
///
/// `libudipe` uses the standard logging convention where logs have various
/// priorities. In \ref udipe_log_config_t, a certain minimal priority can be
/// specified. Logs above this priority are recorded, and logs below this
/// priority are not processed.
typedef enum udipe_log_level_e {
    /// Detailed debugging logs
    ///
    /// This is used for very verbose logs that are only useful when debugging
    /// complicated problems, and should only be enabled on very simplified
    /// error reproducers as they will spill out an unmanageable flow of
    /// information on unmodified production applications.
    ///
    /// Examples include decomposition of complex user requests into simpler
    /// operations, e.g. logs about every single packet that is successfully
    /// processed by a particular input/output data stream.
    ///
    /// \internal
    ///
    /// If you are unsure whether a particular event should be logged at `TRACE`
    /// level or not logged at all, ask yourself whether this log is needed to
    /// understand the control flow path that was taken within `libudipe`. A
    /// core goal of `TRACE` logs is to reduce the amount of debugging scenarios
    /// for which a dedicated debugger is needed.
    UDIPE_LOG_TRACE = 1,

    /// Basic debugging logs
    ///
    /// This is used for rather verbose logs that are only useful when
    /// debugging `udipe`'s internal operation, best applied to simplified error
    /// reproducers (as they are very chatty on realistic use cases), and may
    /// have an unacceptable performance impact in production applications.
    ///
    /// Examples include lifecycle tracing of individual one-shot send/receive
    /// requests as they pass through the various components of `udipe`, or
    /// detailed info about each and every lost packet (note that the
    /// performance impact of such logging will make packet loss worse).
    UDIPE_LOG_DEBUG,

    /// "For your information" logs
    ///
    /// This is used for application lifecycle events that are normal and
    /// infrequent in production applications.
    ///
    /// Examples include explaining the final `udipe` configuration after
    /// merging defaults and automatic system configuration detection with
    /// manual user configuration, or beginning to listen for incoming packets
    /// on some network port/address.
    UDIPE_LOG_INFO,

    /// Warning logs
    ///
    /// This is used for events that are suspicious and may indicate a problem,
    /// but are fine in certain circumstances, and do not prevent the
    /// application to operate in a possibly degraded manner.
    ///
    /// Examples include detecting a system configuration that is suboptimal
    /// from a performance point of view, or low-frequency reporting of
    /// packet loss (once every N seconds where N is chosen to have no
    /// significant performance impact in production).
    ///
    /// Because the value of `errno` is unreliable, as you never know which
    /// function set or overwrote it, `errno`-related logs are also displayed at
    /// the `WARNING` log level.
    UDIPE_LOG_WARNING,

    /// Error logs
    ///
    /// This is used for logs that indicate a clear-cut problem from which the
    /// application may not manage to recover, and even if it does it will do so
    /// at the expense of failing to correctly honor a direct user request.
    ///
    /// Basically, anytime a function that should not fail fails, an error log
    /// is emitted to explain why exactly it failed.
    UDIPE_LOG_ERROR,

    /// Default configuration placeholder for \ref udipe_log_config_t::min_level
    ///
    /// The default is to emit logs of priority >= `INFO` in all builds, and
    /// additionally emit logs of priority `DEBUG` in `Debug` builds.
    ///
    /// \internal
    ///
    /// This log level must not be applied to actual logs from the libudipe
    /// implementation. It is only a user-facing configuration helper.
    UDIPE_LOG_DEFAULT = 0
} udipe_log_level_t;


/// Get the textual name of a certain log level
///
/// For example, given the UDIPE_LOG_ERROR input, this function returns "ERROR".
///
/// As this function is meant to be used inside of logger implementations, it
/// will exceptionally log invalid parameter errors to stderr as opposed to the
/// user-specified logger.
///
/// \returns The textual name of the log level, as a static string.
UDIPE_PUBLIC
const char* udipe_log_level_name(udipe_log_level_t level);


/// Logging callback
///
/// This callback will only be called for logs above the \ref
/// udipe_log_config_t::min_level threshold. It takes the following arguments:
///
/// - User-defined \link #udipe_log_config_t::context context \endlink
/// - \link #udipe_log_level_t Level/priority \endlink of the incoming log
/// - Udipe source code location that the code originates from
/// - Textual description of what happened
///
/// The logging callback will be called concurrently by `udipe` worker threads
/// and must therefore be thread-safe.
typedef void (*udipe_log_callback_t)(void* /*context */,
                                     udipe_log_level_t /*level*/,
                                     const char[] /*location*/,
                                     const char[] /*message*/);


/// Logging configuration
///
/// This data structure controls `libudipe`'s logging behavior. Like other
/// configuration data structures, it is designed such that zero-initializing it
/// should result in sane defaults for many applications.
typedef struct udipe_log_config_s {
    /// Minimal log level/priority to be reported
    ///
    /// If this is left at `DEFAULT` (`0`), `udipe` will emit logs of priority
    /// >= `INFO` in all builds and additionally emit logs of priority
    /// `DEBUG` in `Debug` builds.
    udipe_log_level_t min_level;

    /// User logging callback
    ///
    /// This is where you can plug `udipe` logs into your pre-existing logging
    /// infrastructure like syslog etc. If this is left unconfigured (`NULL`),
    /// `udipe` will print log messages on `stderr`.
    ///
    /// If this pointer is not NULL, then you must ensure that it is valid to
    /// call the associated callback at any time, including from multiple
    /// threads, until the \ref udipe_context_t is destroyed by
    /// udipe_finalize(). And for this entire duration, the associated \link
    /// #udipe_log_config_t::context context \endlink, if any, must be valid to
    /// use too.
    udipe_log_callback_t callback;

    /// User logging callback context
    ///
    /// This pointer is not used by the `udipe` implementation, but merely
    /// passed down as the first argument to each call to your \link
    /// #udipe_log_config_t::callback callback \endlink.
    ///
    /// You can use it to implement more sophisticated logging that requires
    /// some kind of external state. For example, if your log callback is a Rust
    /// or C++ lambda, this is where is where its self/this pointer should go.
    ///
    /// If you do not specify a callback or if your callback does not need any
    /// supplementary state, you should leave this at `NULL` for clarity.
    void* context;
} udipe_log_config_t;
