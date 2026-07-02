#pragma once

//! \file
//! \brief Type of \ref udipe_future_t


/// Future type
///
/// This identifier is set at the time where a future is created, and never
/// changes afterwards until the future is liberated. It is used by future
/// clients to tell which future type they are dealing with, which is a piece of
/// information that they need when interpreting the various union fields of the
/// \ref udipe_future_t struct.
///
/// The future type is currently stored in the future status word because there
/// is enough room for it and it saves client threads from the trouble of
/// needing to read two different future fields to tell what future they're
/// dealing with and in which state it is. However, since this field is constant
/// metadata that does not change during a future's lifetime, it never needs to
/// be atomically modified along with other status information, and therefore
/// does not _need_ to be stored into the future status word. It is therefore
/// fine to extract this enum from the status word to another field if either
/// that identifier grows too large or too many extra status bits end up being
/// needed for other purposes.
typedef enum future_type_e /* : _BitInt(4) */ {
    /// Invalid future type
    ///
    /// This placeholder type is only set on unallocated futures and should
    /// never be observed on a properly initialized future.
    TYPE_INVALID = 0,

    /// First network operation type
    ///
    /// All network operations have a type code between \ref TYPE_NETWORK_START
    /// inclusive and \ref TYPE_NETWORK_END exclusive.
    TYPE_NETWORK_START,

    /// Connection setup request, see udipe_start_connect()
    ///
    TYPE_NETWORK_CONNECT = TYPE_NETWORK_START,

    /// Connection teardown request, see udipe_start_disconnect()
    ///
    TYPE_NETWORK_DISCONNECT,

    /// Datagram send request, see udipe_start_send()
    ///
    TYPE_NETWORK_SEND,

    /// Datagram receive request, see udipe_start_recv()
    ///
    TYPE_NETWORK_RECV,

    /// First future type past the end of the list of network operations
    ///
    /// See also \ref TYPE_NETWORK_START.
    TYPE_NETWORK_END,

    /// Custom operation, see udipe_start_custom()
    ///
    TYPE_CUSTOM = TYPE_NETWORK_END,

    /// Joining several futures, see udipe_start_join()
    ///
    TYPE_JOIN,

    /// Unordered future execution, see udipe_start_unordered()
    ///
    TYPE_UNORDERED,

    /// Single-shot/deadline timer, see udipe_start_timer_once()
    ///
    TYPE_TIMER_ONCE,

    /// Multi-shot/periodic timer, see udipe_start_timer_repeat()
    ///
    TYPE_TIMER_REPEAT,

    /// Not a true type, only needed to count how many future types there are
    ///
    NUM_TYPES,


    // NOTE: If this enum gets more than 16 variants, excluding NUM_TYPES
    //       which is never assigned to it and TYPE_NETWORK_START/END which only
    //       mark the start/end of a variant range and are aliased with other
    //       variants, reallocate the bit budget of the type field of the
    //       future_status_word_t then adjust the _BitInt comment above for
    //       future C23 migration accordingly.
} future_type_t;
