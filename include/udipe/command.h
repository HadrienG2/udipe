#pragma once

//! \file
//! \brief Worker thread commands
//!
//! In `libudipe`, UDP communication is performed by sending commands to worker
//! threads, which asynchronously process them.
//!
//! The use of dedicated worker threads lets `libudipe` internally follow many
//! best practices for optimal UDP performance, without forcing your application
//! threads that interact with `libudipe` into the same discipline. But there is
//! a price to pay, which is that individual commands are rather expensive to
//! process as they involve inter-thread communication.
//!
//! This is why most commands that process a single UDP packet come with a
//! streaming variant that processes an arbitrarily long stream of UDP packets.
//! For example, udipe_recv(), which receives a single UDP packet, comes with a
//! udipe_recv_stream() streaming variant that processes an arbitrary amount of
//! incoming UDP packets using arbitrary logic defined by a callback.
//!
//! These callbacks are directly executed by `libudipe` worker threads, which
//! means that they operate without requiring any inter-thread communication.
//! But this also means that they also require careful programming practices
//! when top performance is desired. See the documentation of individual
//! streaming functions for more advice on how to do this.
//!
//! Finally, all commands come with two associated API entry points, a
//! synchronous one and an asynchronous one. For example, the udipe_recv() entry
//! point, which receives a UDP packet, comes with a udipe_start_recv()
//! asynchronous variant which starts receiving a UDP packet but does not wait
//! for it to be ready before returning. When you use the asynchronous version,
//! you get a \ref udipe_future_t handle that you can later use to wait for the
//! operation to complete through the udipe_wait() function.
//!
//! The main intended use of asynchronous commands is to let you start an
//! arbitrary amount of udipe tasks, then do arbitrary other work, and finally
//! wait for some of your udipe tasks to complete. In cases where you want to
//! wait for multiple tasks to complete, consider using udipe_wait_all().

#include "context.h"
#include "pointer.h"
#include "visibility.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>


/// \name Options and results of individual commands
/// \{

// TODO: Flesh out definitions, add docs
//
// TODO: Add max-size warnings in \internal, beware that they will not be the
//       same for options and results as for options we need to fit in the
//       internal command_t type.
typedef int udipe_connect_options_t;
typedef int udipe_connect_result_t;
typedef int udipe_disconnect_options_t;
typedef int udipe_disconnect_result_t;
typedef int udipe_send_options_t;
typedef int udipe_send_result_t;
typedef int udipe_recv_options_t;
typedef int udipe_recv_result_t;
typedef int udipe_send_stream_options_t;
typedef int udipe_send_stream_result_t;
typedef int udipe_recv_stream_options_t;
typedef int udipe_recv_stream_result_t;
typedef int udipe_reply_stream_options_t;
typedef int udipe_reply_stream_result_t;

/// \}


/// \name Genericity over the command type
/// \{

/// Variant payload from a \ref udipe_result_t
///
/// This union is normally paired with a \ref udipe_command_id_t that indicates
/// what command produced the result in question.
///
/// \internal
///
/// The size of this union should be kept such that \ref udipe_future_t fits in
/// one single cache line on all CPU platforms of interest. This currently
/// amounts to a size limit of 60B.
typedef union udipe_result_payload_u {
    udipe_connect_result_t connect;  ///< Result of udipe_connect()
    udipe_disconnect_result_t disconnect;  ///< Result of udipe_disconnect()
    udipe_send_result_t send;  ///< Result of udipe_send()
    udipe_recv_result_t recv;  ///< Result of udipe_recv()
    udipe_send_stream_result_t send_stream;  ///< Result of udipe_send_stream()
    udipe_recv_stream_result_t recv_stream;  ///< Result of udipe_recv_stream()
    udipe_reply_stream_result_t reply_stream;  ///< Result of udipe_reply_stream()
} udipe_result_payload_t;

/// Command identifier
///
/// This enumerated type has one positive value per `libudipe` command. It is
/// used to build types like \ref udipe_result_t that are generic over multiple
/// command types.
///
/// The zero sentinel variant \ref UDIPE_NO_COMMAND serves a dual purpose: it
/// enables zero-initialization and makes it possible to signal an absence of
/// result in situations where this is appropriate (like e.g. if a wait command
/// had a timeout and it passed without an operation completion notification).
typedef enum udipe_command_id_e {
    UDIPE_CONNECT = 1,  ///< udipe_connect()
    UDIPE_DISCONNECT,  ///< udipe_disconnect()
    UDIPE_SEND,  ///< udipe_send()
    UDIPE_RECV,  ///< udipe_recv()
    UDIPE_SEND_STREAM,  ///< udipe_send_stream()
    UDIPE_RECV_STREAM,  ///< udipe_recv_stream()
    UDIPE_REPLY_STREAM,  ///< udipe_reply_stream()
    UDIPE_NO_COMMAND = 0  ///< Sentinel value with no associated command
} udipe_command_id_t;

/// Generic result type
///
/// This type can encapsulate the result of any `libudipe` command, as well as
/// an absence of result.
typedef struct udipe_result_s {
    /// Result of the command, if any
    ///
    /// `command_id` can be used to check whether there is a result, and if so
    /// which command produced that result.
    udipe_result_payload_t payload;

    /// Command that returned this result, or \ref UDIPE_NO_COMMAND to denote an
    /// absence of result
    ///
    /// Even when one is using infaillible wait commands such as udipe_wait()
    /// with a `timeout` of 0, this field can be useful for debug assertions
    /// that a result is associated with the expected command type. It also
    /// enables having generic utilities that can handle all types of results.
    udipe_command_id_t command_id;
} udipe_result_t;

/// \}


/// \name Asynchronous operation futures
/// \{

/// Asynchronous operation future
///
/// A pointer to this opaque struct is built by every asynchronous command
/// (those whose name begins with `udipe_start_`). It can be used to query
/// whether the associated asynchronous operation is done executing, wait for it
/// to finish executing, and collect the result.
///
/// Its content is an opaque implementation detail of `libudipe` that you should
/// not attempt to read or modify.
///
/// It cannot be used again after the completion of the operation has been
/// successfully awaited using a function like udipe_wait().
typedef struct udipe_future_s udipe_future_t;

/// Truth that an asynchronous operation is finished
///
/// If this returns true, then a call to udipe_wait() for this future is
/// guaranteed to return the result immediately without blocking this thread.
///
/// If you find yourself needing to use this function for periodical polling
/// because you are also waiting for some events outside of `libudipe`, please
/// contact the `libudipe` developers. There _may_ be a way to provide a uniform
/// blocking wait interface for you, at the expense of reducing portability or
/// exposing more `libudipe` implementation details.
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
/// \ref udipe_result_t will have the nonzero `command_id` of the command that
/// was originally submitted. In this case, the future object is destroyed and
/// must not be used again.
///
/// If the asynchronous operation takes longer than the specified timeout to
/// complete, then this function will return an invalid result (with
/// `command_id` set to \ref UDIPE_NO_COMMAND). In this case, the future object
/// remains valid and can be awaited again.
///
/// \param future must be a future that was returned by an asynchronous entry
///               point (those whose name begins with `udipe_start_`), and that
///               has not been successfully awaited yet.
/// \param timeout_ns specifies a minimal time in nanoseconds during which
///                   udipe_wait() will wait for the asynchronous operation to
///                   complete. The actual delay will be rounded up to the next
///                   multiple of the system scheduler clock granularity and may
///                   be affected by system task scheduling overheads. If a
///                   delay of UINT64_MAX is specified, then udipe_wait() will
///                   not return until the specified operation completes.
///
/// \returns The result of the asynchronous operation if it completes, or an
///          invalid result (with `command_id` set to \ref UDIPE_NO_COMMAND) if
///          the operation did not complete before the timeout was reached.
//
// TODO: Wait for an asynchronous task to finish and fetch its result.
//       Recycle the future into the host thread's local cache.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
udipe_result_t udipe_wait(udipe_future_t* future, uint64_t timeout_ns);

/// Wait for the result of multiple asynchronous operations
///
/// This is a collective version of udipe_wait() that waits for multiple futures
/// to complete, or for the timeout to elapse. The output boolean indicates
/// whether all futures have completed or the request has timed out.
///
/// If the result is `true`, indicating full completion, then it is guaranteed
/// that the operations associated with all futures have completed. Therefore
/// none of the output `results` have their `command_id` field set to \ref
/// UDIPE_NO_COMMAND, and none of the input `futures` can be used afterwards.
///
/// If the result is `false`, indicating that the wait has timed out, then you
/// must check each entry of `result` to see which operations have completed. By
/// the same logic as udipe_wait(), those which have **not** completed will have
/// the `command_id` field of their \ref udipe_result_t set to \ref
/// UDIPE_NO_COMMAND.
///
/// As a reminder, futures associated with operations that have completed have
/// been destroyed and must not be used again.
///
/// \param num_futures must indicate the number of elements in the `futures`
///                    and `results` arrays.
/// \param futures must be an array of length `num_futures` containing futures
///                that have not been successfully awaited yet.
/// \param results must be an array of length `num_futures` of \ref
///                udipe_result_t. The initial value of these results does not
///                matter, they will be overwritten.
/// \param timeout_ns works as in udipe_wait().
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
                    uint64_t timeout_ns);

/// Wait for the result of at least one asynchronous operation
///
/// This is a collective version of udipe_wait() that waits for at least one
/// future to complete, or for the timeout to elapse. The result indicates how
/// many futures have completed, if it is 0 then the request has timed out.
///
/// Aside from the obvious difference that it waits for 1+ operation rather than
/// all of them, this function is used a lot like udipe_wait_all(), with a few
/// API tweaks. We will therefore mainly focus on the differences, and let you
/// check the documentation of udipe_wait_all() where they work identically.
///
/// \param num_futures works as in udipe_wait_all() except it also indicates
///                    the size of the `result_positions` array if there is one.
/// \param futures works as in udipe_wait_all()
/// \param results works as in udipe_wait_all()
/// \param result_positions can be `NULL`. If it is set, then it must point to
///                         an array of `size_t` of length `num_futures`.
///                         This array will be used to record the positions of
///                         the futures that did reach completion, the return
///                         value of the function will tell how many entries
///                         were filled this way.
/// \param timeout_ns works as in udipe_wait().
///
/// \returns the number of operations that have completed, which will be nonzero
///          if at least one operation has completed and zero otherwise.
//
// TODO: Implement by first checking futexes for completion, then converting the
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
                      uint64_t timeout_ns);

/// \}


/// \name Worker thread commands
/// \{

// TODO: document and implement
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* udipe_start_connect(udipe_context_t* context,
                                    udipe_connect_options_t options);

// TODO: document
static inline
UDIPE_NON_NULL_ARGS
udipe_connect_result_t udipe_connect(udipe_context_t* context,
                                     udipe_connect_options_t options) {
    udipe_future_t* future = udipe_start_connect(context, options);
    assert(future);
    udipe_result_t result = udipe_wait(future, 0);
    assert(result.command_id == UDIPE_CONNECT);
    return result.payload.connect;
}

// TODO: document and implement
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* udipe_start_disconnect(udipe_context_t* context,
                                       udipe_disconnect_options_t options);

// TODO: document
static inline
UDIPE_NON_NULL_ARGS
udipe_disconnect_result_t udipe_disconnect(udipe_context_t* context,
                                           udipe_disconnect_options_t options) {
    udipe_future_t* future = udipe_start_disconnect(context, options);
    assert(future);
    udipe_result_t result = udipe_wait(future, 0);
    assert(result.command_id == UDIPE_DISCONNECT);
    return result.payload.connect;
}

// TODO: document and implement
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* udipe_start_send(udipe_context_t* context,
                                 udipe_send_options_t options);

// TODO: document
static inline
UDIPE_NON_NULL_ARGS
udipe_send_result_t udipe_send(udipe_context_t* context,
                               udipe_send_options_t options) {
    udipe_future_t* future = udipe_start_send(context, options);
    assert(future);
    udipe_result_t result = udipe_wait(future, 0);
    assert(result.command_id == UDIPE_SEND);
    return result.payload.send;
}

// TODO: document and implement
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* udipe_start_recv(udipe_context_t* context,
                                 udipe_recv_options_t options);

// TODO: document
static inline
UDIPE_NON_NULL_ARGS
udipe_recv_result_t udipe_recv(udipe_context_t* context,
                               udipe_recv_options_t options) {
    udipe_future_t* future = udipe_start_recv(context, options);
    assert(future);
    udipe_result_t result = udipe_wait(future, 0);
    assert(result.command_id == UDIPE_RECV);
    return result.payload.recv;
}

// TODO: document and implement
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* udipe_start_send_stream(udipe_context_t* context,
                                        udipe_send_stream_options_t options);

// TODO: document
static inline
UDIPE_NON_NULL_ARGS
udipe_send_stream_result_t
udipe_send_stream(udipe_context_t* context,
                  udipe_send_stream_options_t options) {
    udipe_future_t* future = udipe_start_send_stream(context, options);
    assert(future);
    udipe_result_t result = udipe_wait(future, 0);
    assert(result.command_id == UDIPE_SEND_STREAM);
    return result.payload.send_stream;
}

// TODO: document and implement
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* udipe_start_recv_stream(udipe_context_t* context,
                                        udipe_recv_stream_options_t options);

// TODO: document
static inline
UDIPE_NON_NULL_ARGS
udipe_recv_stream_result_t
udipe_recv_stream(udipe_context_t* context,
                  udipe_recv_stream_options_t options) {
    udipe_future_t* future = udipe_start_recv_stream(context, options);
    assert(future);
    udipe_result_t result = udipe_wait(future, 0);
    assert(result.command_id == UDIPE_RECV_STREAM);
    return result.payload.recv_stream;
}

// TODO: document and implement
// TODO: This is sort of the combination of a send_stream() and a
//       recv_stream(). It combines an incoming and outgoing connection (which
//       may be the same connection) in such a way that for each incoming
//       packet on one connection, you can send a packet to the other
//       connection, which is derived from the incoming one.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* udipe_start_reply_stream(udipe_context_t* context,
                                         udipe_reply_stream_options_t options);

// TODO: document
static inline
UDIPE_NON_NULL_ARGS
udipe_reply_stream_result_t
udipe_reply_stream(udipe_context_t* context,
                   udipe_reply_stream_options_t options) {
    udipe_future_t* future = udipe_start_reply_stream(context, options);
    assert(future);
    udipe_result_t result = udipe_wait(future, 0);
    assert(result.command_id == UDIPE_REPLY_STREAM);
    return result.payload.reply_stream;
}

/// \}
