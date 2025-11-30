#pragma once

//! \file
//! \brief Asynchronous operation management
//!
//! Asynchronous `libudipe` commands such as udipe_start_connect() do not
//! directly return a result, but instead return a pointer to a \ref
//! udipe_future_t proxy that is later used to wait for the result to come up.
//!
//! Adding this intermediary stage where the command has been submitted to
//! worker threads, but has not been awaited yet, allows you to schedule more
//! commands before you wait for the result of the initial command to come up,
//! and to flexibly and efficiently wait for multiple operations using
//! collective operations udipe_wait_all() and udipe_wait_any().

#include "pointer.h"
#include "result.h"
#include "time.h"
#include "visibility.h"

#include <stdbool.h>
#include <stddef.h>


/// Asynchronous operation future
///
/// Every asynchronous `libudipe` command (function whose name begins with
/// `udipe_start_`) returns a pointer to a future, which acts as a proxy for the
/// associated asynchronous operation.
///
/// This future **must** later be awaited using udipe_wait() or a collective
/// version thereof, which is the point at which the operation's result or
/// errors will be reported, and associated resources will be liberated.
///
/// After a future has been awaited to completion, the ressources associated
/// with it have been liberated, and it must not be used again.
///
/// The content of a future is an opaque implementation detail of `libudipe`
/// that you should not attempt to read or modify in any way.
typedef struct udipe_future_s udipe_future_t;

/// Truth that an asynchronous operation is finished
///
/// If this returns true, then a subsequent call to udipe_wait() for this future
/// is guaranteed to return the result immediately without blocking the caller,
/// even if a timeout of 0 is used.
///
/// If you find yourself needing to use this function for periodical polling
/// because you are also waiting for some events outside of `libudipe`'s
/// control, please consider getting in touch with the `libudipe` development
/// team. There _may_ be a way for us to provide a uniform blocking wait
/// interface that lets you wait for everything at once, at the expense of
/// reducing portability or exposing more `libudipe` implementation details.
///
/// \param future must be a future that was returned by an asynchronous entry
///               point (those whose name begins with `udipe_start_`), and that
///               has not been successfully awaited yet.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
bool udipe_done(const udipe_future_t* future);

/// Wait for the result of an asynchronous operation
///
/// This command will wait until the asynchronous operation designated by
/// `future` completes or the timeout delay specified by `timeout_ns` (see
/// below) elapses.
///
/// If the asynchronous operation completes before the timeout, then the output
/// \ref udipe_result_t will have the nonzero \ref command_id_t of the command
/// that was originally submitted. In this case the future is liberated and must
/// not be used again.
///
/// If the asynchronous operation takes longer than the specified timeout to
/// complete, then this function will return a pending result (with `command_id`
/// set to \ref UDIPE_COMMAND_PENDING). In this case the future remains valid
/// and can be awaited again.
///
/// It is possible to await a future on a thread other than the one which
/// started the asynchronous operation, however that will come at the expense of
/// a performance hit and less optimal resource management.
///
/// If you need to wait for multiple asynchronous operations, then you may want
/// to look into use udipe_wait_all() or udipe_wait_any() instead of awaiting
/// them one by one.
///
/// \param future must be a future that was returned by an asynchronous command
///               (those functions whose name begins with `udipe_start_`), and
///               has not been successfully awaited yet.
/// \param timeout specifies a minimal time in nanoseconds during which
///                udipe_wait() will wait for the asynchronous operation to
///                complete, unless set to zero in which case it means "wait
///                indefinitely for something to happens". See \ref
///                udipe_duration_ns_t for more information.
///
/// \returns The result of the asynchronous operation if it completes, or a
///          pending result (with `command_id` set to \ref
///          UDIPE_COMMAND_PENDING) if the operation did not complete before the
///          timeout was reached.
//
// TODO: Wait for an asynchronous task to finish and fetch its result.
//       Recycle the future into the host thread's local cache.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
udipe_result_t udipe_wait(udipe_future_t* future, udipe_duration_ns_t timeout);

/// Wait for the result of multiple asynchronous operations
///
/// This is a collective version of udipe_wait() that waits for multiple futures
/// to complete, or for the timeout to elapse. The output boolean indicates
/// whether all futures have completed or the request has timed out.
///
/// If the result is `true`, indicating full completion, then it is guaranteed
/// that the operations associated with all futures have completed. Therefore
/// all of the output `results` will be set to the result of the associated
/// operations, and none of the input futures can be used again.
///
/// If the result is `false`, indicating that the wait has timed out before all
/// operations reached completion, then you must check each entry of `result` to
/// see which operations have completed. By the same logic as udipe_wait(),
/// those operations that have **not** completed will have the `command_id`
/// field of their \ref udipe_result_t set to \ref UDIPE_COMMAND_PENDING.
///
/// As a reminder, futures associated with operations that have completed have
/// been liberated and must not be used again.
///
/// \param num_futures must indicate the number of elements in the `futures`
///                    and `results` arrays.
/// \param futures must be an array of length `num_futures` containing futures
///                that have not been successfully awaited yet.
/// \param results must be an array of length `num_futures` of \ref
///                udipe_result_t. The initial value of these results does not
///                matter, they will be overwritten.
/// \param timeout works as in udipe_wait().
///
/// \returns `true` if all asynchronous operations completed, and `false` if the
///          operation did not complete before the timeout was reached.
//
// TODO: Implement through repeated calls to udipe_wait_any() or an optimized
//       version thereof that uses an internal API to recycle internal fds,
//       epoll context, etc.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
bool udipe_wait_all(size_t num_futures,
                    udipe_future_t* futures[],
                    udipe_result_t results[],
                    udipe_duration_ns_t timeout);

/// Wait for the result of at least one asynchronous operation
///
/// This is a collective version of udipe_wait() that waits for at least one
/// future to complete, or for the timeout to elapse. The result indicates how
/// many futures have completed, if it is 0 then the request has timed out.
///
/// Aside from the obvious difference that it waits for one or more operations
/// rather than operations, this function is used a lot like udipe_wait_all(),
/// with a few API tweaks. We will therefore mainly focus on the differences,
/// and let you check the documentation of udipe_wait_all() for those areas
/// where both functions work identically.
///
/// \param num_futures works as in udipe_wait_all() except it also indicates
///                    the size of the `result_positions` array if there is one.
/// \param futures works as in udipe_wait_all()
/// \param results works as in udipe_wait_all()
/// \param result_positions can be `NULL`. If it is set, then it must point to
///                         an array of `size_t` of length `num_futures`.
///                         This array will be used to record the positions of
///                         the futures that did reach completion, and the
///                         return value of the function will tell how many
///                         entries were filled this way.
/// \param timeout works as in udipe_wait().
///
/// \returns the number of operations that have completed, which will be nonzero
///          if at least one operation has completed and zero otherwise.
//
// TODO: Implement by first polling futexes for completion, then converting the
//       remaining futexes to file descriptors using FUTEX_FD, then polling
//       these fds using epoll(), then discarding everything. Consider having an
//       internal variant that keeps the context around instead, used by the
//       implementation of udipe_wait_any().
UDIPE_PUBLIC
UDIPE_NON_NULL_SPECIFIC_ARGS(2, 3)
size_t udipe_wait_any(size_t num_futures,
                      udipe_future_t* futures[],
                      udipe_result_t results[],
                      size_t* result_positions,
                      udipe_duration_ns_t timeout);
