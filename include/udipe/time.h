#pragma once

//! \file
//! \brief Time-related definitions
//!
//! At the time of writing, the public `libudipe` API only exposes support for
//! timeouts, but when a given worker thread awaits multiple timeouts, its
//! implementation is closer in spirit to deadline scheduling. So if the need
//! emerges, support for deadlines would not be hard to add to the public API.

#include <stdint.h>


/// Duration in nanoseconds (0, 1 and the maximum value are special)
///
/// This type, which is typically used for timeouts as an abstraction over the
/// many different time formats used by operating system APIs, can encode
/// durations up to a bit more than 584 years.
///
/// Because processing a network command takes an amount of time which is much
/// greater than a nanosecond (it's closer to the microsecond scale), timeouts
/// should be understood as a lower bound on the duration for which network
/// operations will be awaited, rather than as an absolute deadline by which a
/// given command should have completed.
///
/// The following values of \ref udipe_duration_ns_t will be treated specially:
///
/// - 0 aka \ref UDIPE_DURATION_DEFAULT will be translated to the appropriate
///   default duration value for the function or struct member at hand. For
///   timeouts this is \ref UDIPE_DURATION_MAX.
/// - 1 aka \ref UDIPE_DURATION_MIN represents an infinitely small,
///   instantaneous duration. It would be zero if "zero means default" weren't
///   the custom in C. For timeouts this expresses a desire for nonblocking
///   operation: if the result is ready then it should be returned immediately,
///   otherwise the function should fail immediately with a timeout error.
/// - \ref UDIPE_DURATION_MAX, which is the maximal value of this type
///   represents an infinitely long duration. For timeouts this expresses a
///   desire for fully blocking oeration: wait for the operation to happen or
///   for an error to prevent it from happening.
typedef uint64_t udipe_duration_ns_t;

/// Default value of \ref udipe_duration_ns_t
///
/// When used as a parameter, this function means that the default duration
/// should be used. For timeouts, this is \ref UDIPE_DURATION_MAX.
///
/// \internal
///
/// This value is only valid as a user parameter and should be translated to the
/// matching default value by the user-facing entry point before being passed to
/// the rest of the udipe implementation.
#define UDIPE_DURATION_DEFAULT ((udipe_duration_ns_t)0)

/// Minimal significant value of \ref udipe_duration_ns_t
///
/// When used as a timeout, this value indicates a desire for non-blocking
/// operation i.e. if something can be done immediately then it is done,
/// otherwise the function should fail with a timeout error immediately.
#define UDIPE_DURATION_MIN ((udipe_duration_ns_t)1)

/// Maximal significant value of \ref udipe_duration_ns_t
///
/// When used as a timeout, this value indicates a desire for unbounded blocking
/// i.e. wait indefinitely until the event of interest happens or an error
/// prevents it from happening.
#define UDIPE_DURATION_MAX ((udipe_duration_ns_t)UINT64_MAX)
