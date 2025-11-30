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
/// one single cache line on all CPU platforms of interest. With the current
/// implementation, this amounts to a size limit of 60B.
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
/// It also has two sentinel values, \ref UDIPE_COMMAND_INVALID and \ref
/// UDIPE_COMMAND_PENDING, whose presence should be checked as appropriate. See
/// the documentation of these sentinel values for more info.
typedef enum udipe_command_id_e {
    UDIPE_CONNECT = 1,  ///< udipe_connect()
    UDIPE_DISCONNECT,  ///< udipe_disconnect()
    // TODO: Add and implement
    /*UDIPE_SEND,  ///< udipe_send()
    UDIPE_RECV,  ///< udipe_recv()*/

    /// Invalid command identifier
    ///
    /// Every freshly zero-initialized command identifier gets this sentinel
    /// value and every allocatable struct that contains a command identifier
    /// sets it back to this value upon liberation.
    ///
    /// This helps with the detection of several kinds of invalid struct usage:
    ///
    /// - Incorrectly initialized struct (every initialized struct should have
    ///   its command ID set to a different value).
    /// - Use-after-free (a freed struct's command ID gets back to this value)
    /// - Double allocation (after allocation, a struct's command ID gets
    ///   configured to a different value).
    ///
    /// These checks are typically reserved to Debug builds, but for operations
    /// that are not critical to runtime performance they can be performed in
    /// Release builds too.
    UDIPE_COMMAND_INVALID = 0,

    /// Incomplete asynchronous command identifier
    ///
    /// Wait operations that can return before a particular command is done
    /// executing (e.g. due to a timeout) set the command identifier of the
    /// associated result to this value, which indicates that...
    ///
    /// - The associated command is not done executing and has not yielded a
    ///   result yet, and the associated \ref udipe_result_t is therefore
    ///   invalid and should be discarded without looking up its payload.
    /// - The associated future is still valid and can be awaited again.
    UDIPE_COMMAND_PENDING = -1
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

    /// Command that returned this result, or sentinel value that indicates that
    /// this result is invalid and its payload shouldn't be processed.
    ///
    /// Even when one is using infaillible wait commands such as udipe_wait()
    /// with a `timeout` of 0, this field can be useful for debug assertions
    /// that a result is associated with the expected command type. It also
    /// enables having generic utilities that can handle all types of results.
    udipe_command_id_t command_id;
} udipe_result_t;
