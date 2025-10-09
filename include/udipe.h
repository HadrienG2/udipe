#pragma once


// === Configuration ===

// --- Logging ---

//< Log level/priority
//<
//< `udipe` uses the standard logging convention where logs have various
//< priorities. In LoggerConfig, a certain minimal priority can be specified,
//< and all logs above this priority are being logged.
//
// This enum should not be used directly by udipe internals. Instead, the
// configuration parser should pass it through parse_log_level() to detect
// incorrect log levels and handle DEFAULT. Individual log() statements
// should use should_log() to detemine if they should or should not emit a log.
typedef enum udipe_log_level_t {
    //< Detailed debugging logs
    //<
    //< This is used for very verbose logs that are only useful when debugging
    //< complicated problems, and only be enabled on very simplified error
    //< repoducers as they otherwise emit an unmanageable flow of information.
    //<
    //< Examples include decomposition of complex user requests into simpler
    //< operations, e.g. logging information about the normal processing of
    //< every single incoming packet processed by an input data stream.
    UDIPE_LOG_TRACE = 1,

    //< Basic debugging logs
    //<
    //< This is used for relatively verbose logs that are only useful when
    //< debugging `udipe`'s internal operation, best applied to simplified error
    //< reproducers (as they are spammy on realistic use cases), and may have
    //< an unacceptable performance impact in production.
    //<
    //< Examples include lifecycle tracing of individual one-short send/receive
    //< requests as they pass through the various components of `udipe`, or
    //< detailed info about each lost packet (which will make packet loss worse).
    UDIPE_LOG_DEBUG,

    //< "For your information" logs
    //<
    //< This is used for application lifecycle events that are normal and should
    //< not happen frequently in production applications.
    //<
    //< Examples include explaining the final `udipe` configuration after
    //< merging defaults with explicit user configuration, successfully setting
    //< up or tearing down a network interface, or starting to listen for
    //< incoming packets on some port.
    UDIPE_LOG_INFO,

    //< Warning logs
    //<
    //< This is used for logs that are suspicious and may indicate a problem,
    //< but are fine in certain circumstances, and do not prevent the
    //< application to operate in a possibly degraded way.
    //<
    //< Examples include detecting a system configuration that is suboptimal
    //< from a performance point of view, or low-frequency reporting of
    //< packet loss (once every N seconds where N is chosen to have no
    //< significant performance impact in production).
    UDIPE_LOG_WARNING,

    //< Error logs
    //<
    //< This is used for logs that indicate a clear-cut problem from which the
    //< application may or may not manage to recover.
    //<
    //< Examples include failing to honor a direct user request like listening
    //< for inbound packets on some port, or sending data to some destination.
    UDIPE_LOG_ERROR,

    //< Default configuration placeholder for `LogConfig::min_level`
    //<
    //< The default is to emit logs of priority >= `INFO` in `Release` builds
    //< and additionally emit logs of priority `DEBUG` in Debug builds.
    UDIPE_LOG_DEFAULT = 0
} udipe_log_level_t;

//< Logging callback
//<
//< This callback will only be called for logs above the `LogConfig::min_level`
//< threshold. Its first argument is the log threshold, its second argument is
//< the code location within udipe where a log was emitted, and its third
//< argument is the actual log payload.
typedef void (*udipe_log_callback_t)(udipe_log_level_t /*level*/, const char* /*location*/, const char* /*message*/);

//< Logging configuration
typedef struct udipe_log_config_t {
    //< Minimal log level/priority to be reported
    //<
    //< If this is left at `DEFAULT` (`0`), `udipe` will emit logs of priority
    //< >= `INFO` in `Release` builds and additionally emit logs of priority
    //< `DEBUG` in `Debug` builds.
    udipe_log_level_t min_level;

    //< User logging callback
    //<
    //< This is where the user can plug `udipe` logs into their pre-existing
    //< logging infrastructure like syslog etc. If this is left unconfigured
    //< (`NULL`), `udipe` will print log messages on `stderr`.
    udipe_log_callback_t log_callback;
} udipe_log_config_t;

// --- TODO: Other configurables ---

//< General `udipe` configuration
//<
//< Following standard C convention, this struct is designed such that
//< `memset()`-ing with an all-zeroes bit patterns results in sane defaults that
//< should be appropriate from many applications. You may then go from this
//< starting point
typedef struct udipe_config_t {
    udipe_log_config_t config;
    /*udipe_system_config_t system;
    udipe_streams_config_t initial_streams;*/
} udipe_config_t;
