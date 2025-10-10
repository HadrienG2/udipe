#pragma once

//! \file udipe/log.h
//! \brief Logging configuration

/// Log level/priority
///
/// `udipe` uses the standard logging convention where logs have various
/// priorities. In LoggerConfig, a certain minimal priority can be specified,
/// and all logs above this priority are being logged.
typedef enum udipe_log_level_t {
    /// Detailed debugging logs
    ///
    /// This is used for very verbose logs that are only useful when debugging
    /// complicated problems, and should only be enabled on very simplified
    /// error repoducers as they will spill out an unmanageable flow of
    /// information on unmodified production applications.
    ///
    /// Examples include decomposition of complex user requests into simpler
    /// operations, e.g. logging information about the successful processing of
    /// every single incoming packet processed by an input data stream.
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
    /// detailed info about each lost packet (which will make the loss worse).
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

    /// Default configuration placeholder for LogConfig::min_level
    ///
    /// The default is to emit logs of priority >= `INFO` in `Release` builds
    /// and additionally emit logs of priority `DEBUG` in Debug builds.
    UDIPE_LOG_DEFAULT = 0
} udipe_log_level_t;

/// Logging callback
///
/// This callback will only be called for logs above the LogConfig::min_level
/// threshold. It takes the following arguments:
///
/// - User-defined context object (can be NULL if unused)
/// - Log level
/// - Location in the udipe source code where something happened
/// - A textual description of what happened
///
/// The logging callback will be called concurrently by `udipe` worker threads
/// and must therefore be thread-safe.
typedef void (*udipe_log_callback_t)(void* /*context */,
                                     udipe_log_level_t /*level*/,
                                     const char[] /*location*/,
                                     const char[] /*message*/);

/// Logging configuration
typedef struct udipe_log_config_t {
    /// Minimal log level/priority to be reported
    ///
    /// If this is left at `DEFAULT` (`0`), `udipe` will emit logs of priority
    /// >= `INFO` in `Release` builds and additionally emit logs of priority
    /// `DEBUG` in `Debug` builds.
    udipe_log_level_t min_level;

    /// User logging callback
    ///
    /// This is where you can plug `udipe` logs into your pre-existing logging
    /// infrastructure like syslog etc. If this is left unconfigured (`NULL`),
    /// `udipe` will print log messages on `stderr`.
    udipe_log_callback_t callback;

    /// User logging callback context
    ///
    /// This allows you to implement more sophisticated logging without using
    /// global variables. For example, if your log callback is a Rust or C++
    /// lambda, that's where is where its self/this pointer should go.
    ///
    /// This configuration is only used when \ref udipe_log_config_t::callback
    /// is also specified. Leave it at `NULL` if you don't need it.
    void* context;
} udipe_log_config_t;
