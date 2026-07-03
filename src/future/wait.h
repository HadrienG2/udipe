#pragma once

//! \file
//! \brief Functions for awaiting \ref udipe_future_t
//!
//! After a bit of context setup, the user-facing udipe_wait() function calls
//! into a lower-level future_wait() backend, which exposes the internal status
//! information and is thus suitable for use inside of the implementation of
//! futures that await other futures.
//!
//! future_wait() then proceeds to dispatch into more specific
//! `future_wait_xyz()` functions that are specialized for a set of future types
//! that share a common awaiting procedure.

#include <udipe/future.h>
#include <udipe/duration.h>
#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "status.h"
#include "status_ops.h"

#include "../stopwatch.h"


/// Backend of udipe_wait() that returns the latest future status after the wait
///
/// The wait is considered successful if the final status has \ref STATE_RESULT,
/// which should always be the case when using \ref UDIPE_DURATION_MAX and \ref
/// UDIPE_DURATION_DEFAULT unbounded timeouts.
///
/// The output status is used by operations like udipe_finish() that not only
/// await the final future status, but also process it.
///
/// This function must be called within a logging scope.
///
/// \param future must be a future that was returned by an asynchronous function
///               (those whose name begins with `udipe_start_`) and has not been
///               liberated by udipe_finish() or udipe_cancel() since.
/// \param timeout works as in wait_on_address(). In particular it cannot take
///                value \ref UDIPE_DURATION_DEFAULT, which should have been
///                translated into \ref UDIPE_DURATION_MAX higher up the stack.
/// \param count_policy controls how the future's downstream count should be
///                     manipulated as part of this waiting transaction.
///
/// \returns the final future status at the end of the wait.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait(udipe_future_t* future,
                            udipe_duration_ns_t timeout,
                            downstream_count_policy_t count_policy);

/// Backend of future_wait() for all future types that get eagerly signaled by a
/// separate thread
///
/// This function must be called within a logging scope.
///
/// \param future must be a future that was returned by an asynchronous network
///               operation (those whose name begins with `udipe_start_`) or by
///               udipe_start_custom() and has not been liberated by
///               udipe_finish() or udipe_cancel() since.
/// \param latest_status should be the latest known future status at the time
///                      where this function is called.
/// \param timeout works as in wait_on_address(). In particular it cannot take
///                value \ref UDIPE_DURATION_DEFAULT, which should have been
///                translated into \ref UDIPE_DURATION_MAX higher up the stack.
/// \param count_policy controls how the future's downstream count should be
///                     manipulated as part of this waiting transaction.
///
/// \returns the final future status.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait_eager(udipe_future_t* future,
                                  future_status_t latest_status,
                                  udipe_duration_ns_t timeout,
                                  downstream_count_policy_t count_policy);

/// Backend of future_wait() for \ref TYPE_JOIN
///
/// This function must be called within a logging scope.
///
/// \param future must be a future that was returned by udipe_start_join() and
///               has not been liberated by udipe_finish() or udipe_cancel()
///               since.
/// \param latest_status should be the latest known future status at the time
///                      where this function is called.
/// \param timeout works as in wait_on_address(). In particular it cannot take
///                value \ref UDIPE_DURATION_DEFAULT, which should have been
///                translated into \ref UDIPE_DURATION_MAX higher up the stack.
/// \param count_policy controls how the future's downstream count should be
///                     manipulated as part of this waiting transaction.
///
/// \returns the final future status.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait_join(udipe_future_t* future,
                                 future_status_t latest_status,
                                 udipe_duration_ns_t timeout,
                                 downstream_count_policy_t count_policy);

/// Backend of future_wait() for \ref TYPE_UNORDERED
///
/// This function must be called within a logging scope.
///
/// \param future must be a future that was returned by udipe_start_unordered()
///               and has not been liberated by udipe_finish() or udipe_cancel()
///               since.
/// \param latest_status should be the latest known future status at the time
///                      where this function is called.
/// \param timeout works as in wait_on_address(). In particular it cannot take
///                value \ref UDIPE_DURATION_DEFAULT, which should have been
///                translated into \ref UDIPE_DURATION_MAX higher up the stack.
/// \param count_policy controls how the future's downstream count should be
///                     manipulated as part of this waiting transaction.
///
/// \returns the final future status.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait_unordered(udipe_future_t* future,
                                      future_status_t latest_status,
                                      udipe_duration_ns_t timeout,
                                      downstream_count_policy_t count_policy);

/// Backend of future_wait() for \ref TYPE_TIMER_ONCE
///
/// This function must be called within a logging scope.
///
/// \param future must be a future that was returned by udipe_start_timer_once()
///               and has not been liberated by udipe_finish() or udipe_cancel()
///               since.
/// \param latest_status should be the latest known future status at the time
///                      where this function is called.
/// \param timeout works as in wait_on_address(). In particular it cannot take
///                value \ref UDIPE_DURATION_DEFAULT, which should have been
///                translated into \ref UDIPE_DURATION_MAX higher up the stack.
/// \param count_policy controls how the future's downstream count should be
///                     manipulated as part of this waiting transaction.
///
/// \returns the final future status.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait_timer_once(udipe_future_t* future,
                                       future_status_t latest_status,
                                       udipe_duration_ns_t timeout,
                                       downstream_count_policy_t count_policy);

/// Backend of future_wait() for \ref TYPE_TIMER_REPEAT
///
/// This function must be called within a logging scope.
///
/// \param future must be a future that was returned by
///               udipe_start_timer_repeat() and has not been liberated by
///               udipe_finish() or udipe_cancel() since.
/// \param latest_status should be the latest known future status at the time
///                      where this function is called.
/// \param timeout works as in wait_on_address(). In particular it cannot take
///                value \ref UDIPE_DURATION_DEFAULT, which should have been
///                translated into \ref UDIPE_DURATION_MAX higher up the stack.
/// \param count_policy controls how the future's downstream count should be
///                     manipulated as part of this waiting transaction.
///
/// \returns the final future status.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait_timer_repeat(udipe_future_t* future,
                                         future_status_t latest_status,
                                         udipe_duration_ns_t timeout,
                                         downstream_count_policy_t count_policy);

/// Outcome of future_wait_by_adress()
///
/// This type is returned by future_wait_by_address() to tell the caller what
/// happened and what it should do next.
typedef enum address_wait_outcome_e {
    /// The target future reached \ref STATE_RESULT.
    ///
    ADDRESS_WAIT_SUCCESS,

    /// The user-specified timeout elapsed without the target future reaching
    /// \ref STATE_RESULT.
    ADDRESS_WAIT_TIMEOUT,

    /// The lock on the target future was released without the future reaching
    /// \ref STATE_RESULT
    ///
    /// This outcome can only happen for future types like \ref TYPE_JOIN where
    /// the waiting procedure involves awaiting an \ref inpoll_t. Because the
    /// design of inpoll_wait() makes it hard to use from multiple threads, this
    /// waiting method is implemented by having one thread await the \ref
    /// inpoll_t while other threads wait for it to report its conclusion via a
    /// futex. The selection of the thread that will call inpoll_wait() is
    /// performed via simple locking of a flag in the future status word.
    ///
    /// One limitation of this design, however is that something like the
    /// following can happen:
    ///
    /// - Thread A starts waiting for future F with a timeout of 1s
    /// - Thread A locks state and starts waiting via inpoll_wait().
    /// - Thread B starts waiting for future F with a timeout of 2s.
    /// - Thread B observes that thread A got there first and starts waiting for
    ///   thread A via the futex method.
    /// - Thread A reaches its 1s timeout without getting a notification from
    ///   inpoll_wait(), so it stops waiting and reports the timeout.
    /// - At this point, thread B still has 1s of timeout to go, but it cannot
    ///   passively wait for thread A via the futex anymore, instead it must
    ///   switch to active waiting via inpoll_wait().
    ///
    /// This situation is handled by unblocking **one** of the threads that's
    /// waiting on the futex (to avoid thundering herds), which will retult in
    /// is future_wait_by_address() call returning this outcome. Upon receiving
    /// this outcome, the thread must either lock the future status and start an
    /// inpoll_wait() or wake another futex waiter if it cannot call
    /// inpoll_wait() because its timeout elapsed at the same time.
    ADDRESS_WAIT_UNLOCKED,
} address_wait_outcome_t;

/// Wait for a future's status to change via the wait-by-address path
///
/// This waiting path is used...
///
/// - Anytime an eager future (network & custom operations) does not initially
///   have a ready status.
/// - Whenever a lazy future does not initially have a ready status and another
///   thread already has taken the lock to update its status.
///
/// Before calling this function, you must...
///
/// - Increment the `downstream_count` field of the future status word if
///   directed by the \ref downstream_count_policy_t.
/// - Set the `notify_address` field of said status word.
/// - Perform any other setup step required by the specific future type.
///
/// ...with at least acquire memory ordering, so that no future manipulation is
/// reordered before the status word setup.
///
/// After calling this function, you must decrement the `downstream_count` field
/// of the future status word if directed by the \ref downstream_count_policy_t,
/// with at least release ordering, so that no future manipulation is reordered
/// after the downstream count decrement.
///
/// This function must be called within a logging scope.
///
/// \param future must be a future that supports address-based wakeup, which has
///               not been liberated by udipe_finish() or udipe_cancel(). Its
///               status word must have been updated as directed above, and may
///               need to be updated again after calling this function as
///               directed above..
/// \param latest_status must be initially set to the latest known future status
///                      at the time where this function is called. It will be
///                      updated to the latest known future status at the time
///                      where this function returns.
/// \param timeout should initially be set to the desired timeout with respect
///                to the timestamp denoted by `stopwatch`. This initial value
///                cannot be \ref UDIPE_DURATION_DEFAULT, it should have been
///                translated into \ref UDIPE_DURATION_MAX higher up the stack.
///                By the time this function returns, the timeout will have been
///                updated to correspond to the latest value of `stopwatch`.
/// \param stopwatch must be a stopwatch that was initialized at the start of
///                  the future waiting procedure. It may be updated in an
///                  unspecified fashion.
///
/// \returns a summary of the final status of the future and the actions that
///          must be taken by the caller.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
address_wait_outcome_t future_wait_by_adress(udipe_future_t* future,
                                             future_status_t* latest_status,
                                             udipe_duration_ns_t* timeout,
                                             stopwatch_t* stopwatch);


// TODO: Add tests once future allocation is ready
