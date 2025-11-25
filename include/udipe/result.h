#pragma once

//! \file
//! \brief Generic result type
//!
//! This header defines the \ref udipe_result_t type, which can encapsulate the
//! result of any of the `libudipe` commands defined in \ref command.h, along
//! with related lower-level definitions.

#include "connect.h"


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
    // TODO: Add and implement
    /*udipe_send_result_t send;  ///< Result of udipe_send()
    udipe_recv_result_t recv;  ///< Result of udipe_recv()*/
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
    // TODO: Add and implement
    /*UDIPE_SEND,  ///< udipe_send()
    UDIPE_RECV,  ///< udipe_recv()*/
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
