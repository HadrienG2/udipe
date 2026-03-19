#pragma once

//! \file
//! \brief Generic result type
//!
//! This header defines the \ref udipe_result_t type, which can encapsulate the
//! result of any of the `libudipe` commands defined in \ref command.h, along
//! with related lower-level definitions.

#include "connect.h"

#include <stdalign.h>
#include <stddef.h>


/// Result payloads from network futures
///
/// This result payload pairs with a \ref udipe_result_type_t that indicates what
/// network command produced the result in question.
///
/// \internal
///
/// The size of this union should be kept such that \ref udipe_future_t fits in
/// one single cache line on all CPU platforms of interest. With the current
/// implementation, this amounts to a size limit of 56B.
typedef union udipe_network_payload_u {
    udipe_connect_result_t connect;  ///< Result of udipe_connect()
    udipe_disconnect_result_t disconnect;  ///< Result of udipe_disconnect()
    // TODO: Add and implement
    /*udipe_send_result_t send;  ///< Result of udipe_send()
    udipe_recv_result_t recv;  ///< Result of udipe_recv()*/
} udipe_network_payload_t;

/// Result payload from custom futures created via udipe_start_custom()
///
/// User-defined futures can return data of any type that can be encoded into
/// the inner `bytes` field. If your intended result type doesn't fit in there,
/// then you will need to either heap-allocate a memory block for it or somehow
/// have the user pass in a memory block under their control.
///
/// \internal
///
/// The size of `bytes` should be maintained such that it is as large as the
/// largest variant of \ref udipe_network_payload_t, but in a manual way such
/// that we will not accidentally shrink it later as the implementation and API
/// of network operations evolves. Indeed, the number of bytes available here is
/// part of udipe's public API contract.
typedef struct udipe_custom_payload_s {
    /// Bytes of data that you can fill with any payload of your choosing
    ///
    alignas(void*) char bytes[2*sizeof(void*)];
} udipe_custom_payload_t;

// Forward declaration of \ref udipe_future_t
typedef struct udipe_future_s udipe_future_t;

/// Result payload from unordered futures created via udipe_start_unordered()
///
/// \internal
///
/// The size of this struct should be kept such that \ref udipe_future_t fits in
/// one single cache line on all CPU platforms of interest. With the current
/// implementation, this corresponds to a size limit of 40B.
typedef struct udipe_unordered_payload_s {
    /// Index of the future that reached completion within the `futures` array
    /// that was specified at the time where udipe_start_unordered() was called.
    size_t ready_idx;

    /// Future of subsequent completions, if any
    ///
    /// If there are more futures within `futures` whose completion was not
    /// reported yet, then this pointer will be set fo a future that can be used
    /// to await the completion of those futures or cancel the associated wait.
    ///
    /// Otherwise this pointer will be set to `NULL`, signaling the end of the
    /// unordered wait synchronization transaction.
    udipe_future_t* next;
} udipe_unordered_payload_t;

/// Result payload from repeating timer futures created via
/// udipe_start_timer_repeat()
///
/// \internal
///
/// The size of this struct should be kept such that \ref udipe_future_t fits in
/// one single cache line on all CPU platforms of interest. With the current
/// implementation, this corresponds to a size limit of 48B.
typedef struct udipe_timer_repeat_payload_s {
    /// Number of timer ticks that were missed since the last reported timer
    /// result, or since the timer was initially set if no result was reported
    /// so far
    size_t missed_ticks;

    /// Future of subsequent timer ticks
    ///
    /// This future can be used to resume the wait for subsequent timer ticks,
    /// and should be canceled once you don't need a particular timer anymore.
    udipe_future_t* next;
} udipe_timer_repeat_payload_t;

/// Result type
///
/// This enumerated type has one positive value per `libudipe` command. It is
/// used to build types like \ref udipe_result_t that are generic over multiple
/// command types.
///
/// It also has sentinel values whose presence should be checked as they
/// indicate absence of a valid result as a result of some issue. See the
/// documentation of these sentinel values for more info.
typedef enum udipe_result_type_e {
    // TODO describe associated payload types
    UDIPE_CONNECT = 1,  ///< udipe_start_connect()
    UDIPE_DISCONNECT,  ///< udipe_start_disconnect()
    // TODO: Add and implement
    /*UDIPE_SEND,  ///< udipe_start_send()
    UDIPE_RECV,  ///< udipe_start_recv()*/
    UDIPE_CUSTOM, ///< udipe_start_custom()
    UDIPE_JOIN,  ///< udipe_start_join()
    UDIPE_UNORDERED, ///< udipe_start_unordered()
    UDIPE_TIMER_ONCE, ///< udipe_start_timer_once()
    UDIPE_TIMER_REPEAT, ///< udipe_start_timer_repeat()

    /// Invalid result type
    ///
    /// This result type should never be observed by user code, and its presence
    /// is a direct indication that either the application or udipe has a bug.
    ///
    /// \internal
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
    UDIPE_RESULT_INVALID = 0,

    // FIXME: Cover more cases, including canceled etc
} udipe_result_type_t;

/// Generic result type
///
/// This type can encapsulate the result of any `libudipe` command, as well as
/// an absence of result.
typedef struct udipe_result_s {
    /// Result of the command, if any
    ///
    /// `type` can be used to check whether there is a result, and if so what
    /// kind of payload it is.
    union {
        // TODO doc
        udipe_network_payload_t network;
        // TODO doc
        udipe_custom_payload_t custom;
        // TODO doc
        udipe_unordered_payload_t unordered;
        // TODO doc
        udipe_timer_repeat_payload_t timer_repeat;
    } payload;

    /// Command that returned this result, or sentinel value that indicates that
    /// this result is invalid and its payload shouldn't be processed.
    ///
    /// Even when one is using infaillible wait commands, this field can be
    /// useful for debug assertions that a result is associated with the
    /// expected command type. It also enables having generic utilities that can
    /// handle all types of results.
    udipe_result_type_t type;
} udipe_result_t;
