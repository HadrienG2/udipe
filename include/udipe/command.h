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
//! This is why most commands that process a single UDP datagram come with a
//! streaming variant that processes an arbitrarily long stream of UDP
//! datagrams. For example, udipe_recv(), which receives a single UDP datagram,
//! comes with a udipe_recv_stream() streaming variant that processes an
//! arbitrary amount of incoming UDP datagrams using arbitrary logic defined by
//! a callback.
//!
//! These callbacks are directly executed by `libudipe` worker threads, which
//! means that they operate without requiring any inter-thread communication.
//! But this also means that they also require careful programming practices
//! when top performance is desired. See the documentation of individual
//! streaming functions for more advice on how to do this.
//!
//! Finally, all commands come with two associated API entry points, a
//! synchronous one and an asynchronous one. For example, the udipe_recv() entry
//! point, which receives a UDP datagram, comes with a udipe_start_recv()
//! asynchronous variant which starts receiving a UDP datagram but does not wait
//! for it to be ready before returning. See \ref future.h for more information
//! of how these asynchronous commands work.

#include "connect.h"
#include "context.h"
#include "future.h"
#include "pointer.h"
#include "result.h"
#include "visibility.h"

#include <assert.h>


// TODO: document and implement
//
// TODO: Explain somewhere that a udipe connection is mostly like a POSIX socket
//       but may be implemented using multiple sockets under the hood.
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

// TODO: Add and implement
/*// TODO: document and implement
//
// TODO: Should have GSO-like semantics, i.e. if you give a large enough buffer
//       then multiple datagrams may be sent. If GSO is disabled, then it just
//       sends a single datagram. Do not attempt to send more than 64 datagrams.
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
//
// TODO: Should have GRO-like semantics, i.e. if you give a large enough buffer
//       then multiple datagrams may be received, and there will be anciliary
//       data telling you how large the inner segments are. If GRO is disabled,
//       then it just receives a single datagram.
static inline
UDIPE_NON_NULL_ARGS
udipe_recv_result_t udipe_recv(udipe_context_t* context,
                               udipe_recv_options_t options) {
    udipe_future_t* future = udipe_start_recv(context, options);
    assert(future);
    udipe_result_t result = udipe_wait(future, 0);
    assert(result.command_id == UDIPE_RECV);
    return result.payload.recv;
}*/
