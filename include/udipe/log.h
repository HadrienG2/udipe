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

#include "nodiscard.h"
#include "pointer.h"
#include "visibility.h"

#include <stddef.h>


/// Log level/priority
///
/// The logs emitted by `libudipe` have various priorities, and a minimal
/// priority can be configured in \ref udipe_log_config_t. Logs above the
/// configured minimal priority are recorded, whereas logs below this priority
/// are not processed.
typedef enum udipe_log_level_e {
    /// Detailed debugging logs
    ///
    /// At this maximal log level, udipe logs enough details about the execution
    /// of each function that a debugger should not be necessary to understand
    /// the control flow path taken through each function or extract quantities
    /// that are only known at runtime like fd numbers or allocation addresses.
    ///
    /// This also means that even with filtering, this level of log detail will
    /// often be overwhelming outside of specially crafted small code samples,
    /// and will also have a huge impact on performance, as it allows even a
    /// single function call to emit thousands of logs per second.
    ///
    /// This is why the `DEBUG` log level exists as a less verbose alternative,
    /// and `TRACE` logs are not displayed by default even in debug builds. They
    /// can, however, be easily displayed by tuning down the minimal log level
    /// in the udipe configuration.
    ///
    /// In contrast, release builds filter out `TRACE` and `DEBUG` logs **at
    /// compile time** by default due to their performance impact which is major
    /// even when they are not displayed. If you need these logs to investigate
    /// an problem that only occurs in release builds, you will therefore need
    /// to explicitly enable them using the `ENABLE_EXPENSIVE_LOGS` CMake option
    /// (TODO implement), before you will be able to configure udipe to display
    /// them.
    ///
    /// \internal
    ///
    /// If you are unsure whether a particular event should be logged at `TRACE`
    /// level or not logged at all, ask yourself whether this log is needed to
    /// understand the control flow path that was taken within `libudipe`. A
    /// core goal of `TRACE` logs is to reduce the amount of debugging scenarios
    /// for which the use of a debugger is needed.
    UDIPE_TRACE = 1,

    /// Basic debugging logs
    ///
    /// Like `TRACE` logs, `DEBUG` logs are primarily intended as a debugging
    /// aid and feature detailed technical information that will only make sense
    /// to someone who is familiar with the udipe implementation.
    ///
    /// They also have a significant performance impact even when not displayed,
    /// which is why release builds will filter them out at compile time by
    /// default, as done for `TRACE` logs.
    ///
    /// But in contrast with `TRACE` logs, `DEBUG` logs aim to be comprehensible
    /// when studied in isolation (e.g. calling a function once while disabling
    /// logging from downstream functions via depth filtering). Which is why
    /// they are displayed by default in debug builds, unlike `TRACE` logs.
    ///
    /// To achieve this reduction in cognitive load, `DEBUG` logs refrain from
    /// reporting on individual iterations of loops whose iteration count is
    /// known to be large or unbounded, and whose iteration duration is known or
    /// expected to be short. These are covered by `TRACE` logs instead.
    ///
    /// Examples of logs that are emitted at `DEBUG` level include lifecycle
    /// tracing of individual one-shot send/receive requests as they pass
    /// through the various components of udipe, or detailed info about each and
    /// every lost packet (note that the performance impact of such logging will
    /// make packet loss worse).
    UDIPE_DEBUG,

    /// "For your information" logs
    ///
    /// This level is used to report basic application lifecycle events that are
    /// normal, relatively infrequent in production, and should make basic sense
    /// to people who do not work on the udipe implementation.
    ///
    /// Examples include spelling out the final udipe configuration (after
    /// merging defaults, system autodetection and user configuration),
    /// reporting the startup a worker thread, or beginning to listen for
    /// incoming packets on some network port/address.
    UDIPE_INFO,

    /// Warning logs
    ///
    /// This level is used for logs that are suspicious and may indicate a
    /// problem, but are fine in certain circumstances, and do not prevent the
    /// application to operate in a possibly degraded manner.
    ///
    /// Examples include detecting a system configuration that is suboptimal
    /// from a performance point of view, or low-frequency reporting of
    /// packet loss (once every N seconds where N is chosen to have no
    /// significant performance impact in production).
    ///
    /// Because the value of `errno` is unreliable, as you never know which
    /// function set or overwrote it, `errno`-related logs are also displayed at
    /// the `WARN` log level.
    UDIPE_WARN,

    /// Error logs
    ///
    /// This level is used for logs that indicate a clear-cut problem from which
    /// the application may not manage to recover, and even if it does it will
    /// do so at the expense of failing to correctly honor a user request.
    ///
    /// Basically, anytime a function that should not fail fails, an error log
    /// is emitted to explain why exactly it failed.
    UDIPE_ERROR,

    /// Default configuration placeholder for \ref udipe_log_config_t::min_level
    ///
    /// If the `UDIPE_LOG_LEVEL` environment variable is set, then it determines
    /// this default. For example, setting `UDIPE_LOG_LEVEL=WARN` has the same
    /// effect as setting the log level to \ref UDIPE_WARN using the
    /// configuration struct.
    ///
    /// When the configuration struct is left at its default value and the
    /// `UDIPE_LOG_LEVEL` environment variable is unset, the default is to emit
    /// logs of priority >= `INFO` in all builds, and additionally emit logs of
    /// priority `DEBUG` in `Debug` builds.
    ///
    /// \internal
    ///
    /// This log level must not be applied to actual logs from the libudipe
    /// implementation. It is only a user-facing configuration helper.
    UDIPE_DEFAULT_LOG_LEVEL = 0
} udipe_log_level_t;

/// Get the textual name of a certain log level
///
/// For example, when applied to \ref UDIPE_ERROR, this function returns
/// "ERROR".
///
/// As this function is meant to be used inside of logger implementations, it
/// will exceptionally log invalid parameter errors to stderr as opposed to the
/// user-specified logger.
///
/// \returns The textual name of the log level, as a static string.
UDIPE_NODISCARD
UDIPE_NON_NULL_RESULT
UDIPE_PUBLIC
const char* udipe_log_level_name(udipe_log_level_t level);

/// Logging callback
///
/// This callback will only be called for logs above the \ref
/// udipe_log_config_t::min_level threshold. It takes the following arguments:
///
/// - User-defined \link #udipe_log_config_t::context context \endlink
/// - \link #udipe_log_level_t Level/priority \endlink of the incoming log
/// - \link #udipe_log_config_t::max_debug_depth Abstraction depth \endlink of the
///   incoming log
/// - Udipe source code location that the code originates from
/// - Textual description of what happened
///
/// The logging callback can be called concurrently by application threads and
/// udipe worker threads and must therefore be thread-safe.
typedef void (*udipe_log_callback_t)(void* /*context */,
                                     udipe_log_level_t /*level*/,
                                     size_t /*depth*/,
                                     const char[] /*location*/,
                                     const char[] /*message*/);

/// Logging configuration
///
/// This data structure controls udipe's logging behavior. Like other
/// configuration data structures, it is designed such that zero-initializing it
/// should result in sane defaults for many applications.
typedef struct udipe_log_config_s {
    /// User logging callback
    ///
    /// This configuration parameter lets you forward most udipe logs into a
    /// pre-existing logging infrastructure such as syslog, nyx-node, etc. If
    /// this callback is left unconfigured (`NULL`), udipe will send logs to
    /// `stderr` by default.
    ///
    /// If this pointer is not `NULL`, then you must ensure that it is valid to
    /// call the associated callback at any time, including from multiple
    /// threads, until the \ref udipe_context_t is destroyed by
    /// udipe_finalize(). And for this entire duration, the associated \link
    /// #udipe_log_config_t::context context \endlink, if any, must be valid to
    /// use too.
    ///
    /// Sadly, some rare udipe logs (which should only be emitted in exceptional
    /// situations) cannot be forwarded to this callback and must always go to
    /// `stderr` instead. Examples include...
    ///
    /// - Initialization errors occuring before the the logger is fully set up.
    ///   The logger is purposely set up as early and quickly as possible to
    ///   reduce the amount of affected code.
    /// - Runtime errors originating from the logger implementation itself.
    ///   These errors cannot be logged normally because that could result in
    ///   infinite logger recursion.
    /// - Finalization errors for thread-local and process-global entities, used
    ///   sparingly in the management of OS resources that are intrinsically
    ///   linked with a process or thread (Windows thread names, process memory
    ///   locking limit, ...). These errors can occur long after the udipe
    ///   context is destroyed, at a point where your logging callback may not
    ///   be safe to call anymore, which makes stderr the only sane option.
    udipe_log_callback_t callback;

    /// User logging callback context
    ///
    /// This pointer is not used by the udipe implementation, but merely passed
    /// down as the first argument to each call to your \link
    /// #udipe_log_config_t::callback callback \endlink.
    ///
    /// You can use it to implement more sophisticated logging that requires
    /// some kind of external state. For example, if your log callback is a Rust
    /// or C++ lambda, this is where is where its self/this pointer should go.
    ///
    /// If you do not specify a callback or if your callback does not need any
    /// supplementary state, then you should leave this at `NULL`.
    void* context;

    /// Minimal log level/priority to be reported
    ///
    /// See \ref UDIPE_DEFAULT_LOG_LEVEL for more information about what this
    /// setting does and what happens when you leave it at 0 (\ref
    /// UDIPE_DEFAULT_LOG_LEVEL).
    udipe_log_level_t min_level;

    /// Maximal depth at which `DEBUG` and `TRACE` logs are reported
    ///
    /// The `libudipe` logging infrastructure tracks the abstraction depth at
    /// which `DEBUG` and `TRACE` logs are emitted. For example, logs from a
    /// public `libudipe` entry point `udipe_something()` have an abstraction
    /// depth of 1, logs from a utility `foo()` that is directly called by
    /// `udipe_something()` have an abstraction depth of 2, logs from another
    /// utility `bar()` that is called by `foo()` have an abstraction depth of
    /// 3, and so on.
    ///
    /// You can use this setting to control the maximal abstraction depth at
    /// which `DEBUG` and `TRACE` logs can be displayed, ensuring that logs
    /// emitted at a greater abstraction depth do not get displayed to avoid
    /// being flooded unmanageable log volumes and reduce the performance cost
    /// of logging.
    ///
    /// If this setting is left at 0, the maximal logging abstraction depth is
    /// controlled by the `UDIPE_LOG_DEPTH` environment variable. And if this
    /// variable is unset, a rather conservative maximal abstraction depth is
    /// automatically applied by default. This ensures that very detailed
    /// logging only happens to the people who ask for it with a clear debugging
    /// intent, and is never exposed to users by accident.
    size_t max_debug_depth;
} udipe_log_config_t;
