#pragma once

//! \file
//! \brief Code span duration measurement
//!
//! This code module is used to track how much time a certain piece of code
//! takes to execute. It is used to correctly handle timeouts, no matter how
//! much some operating systems want to break them as soon as signal handling /
//! asynchronous procedure calls get involved.

#include <udipe/duration.h>
#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include <assert.h>
#include <time.h>


/// Mechanism to track the duration of code spans
///
/// After being initialized with stopwatch_initialize(), a stopwatch can be
/// periodically queried with stopwatch_measure(). Each measurement returns the
/// time elapsed since initialization or the last measurement.
typedef struct stopwatch_s {
    struct timespec last_time;  ///< Timestamp as of the last clock check
} stopwatch_t;

/// Set up a \ref stopwatch_t
///
/// \returns a \ref stopwatch_t object that can be regularly queried with
///          stopwatch_measure() in order to track the passage of time.
UDIPE_NODISCARD
static inline stopwatch_t stopwatch_initialize() {
    stopwatch_t stopwatch;
    timespec_get(&stopwatch.last_time, TIME_UTC);
    return stopwatch;
}

/// Measure the amount of elapsed time since the last measurement (or
/// initialization).
///
///
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
static inline udipe_duration_ns_t stopwatch_measure(stopwatch_t* stopwatch) {
    struct timespec start_time = stopwatch->last_time;
    struct timespec current_time;
    timespec_get(&current_time, TIME_UTC);
    assert(current_time.tv_sec >= start_time.tv_sec);

    udipe_duration_ns_t elapsed_time =
        (current_time.tv_sec - start_time.tv_sec) * UDIPE_SECOND;
    if (current_time.tv_nsec >= start_time.tv_nsec) {
        elapsed_time += current_time.tv_nsec - start_time.tv_nsec;
    } else {
        elapsed_time += UDIPE_SECOND;
        elapsed_time -= start_time.tv_nsec - current_time.tv_nsec;
    }

    stopwatch->last_time = current_time;
    return elapsed_time;
}
