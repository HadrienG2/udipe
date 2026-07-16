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

/// Truth that a future type can be scheduled after other futures
///
/// This implies that the future can end up in \ref STATE_WAITING or have \ref
/// OUTCOME_FAILURE_DEPENDENCY, which is impossible otherwise.
///
/// This function must be called within a logging scope.
static inline
bool future_type_has_dependencies(future_type_t type) {
    LOGGED_FUNCTION_START("%d", type)
        switch (type) {
        case TYPE_INVALID:
        case TYPE_CUSTOM:
        case TYPE_TIMER_ONCE:
        case TYPE_TIMER_REPEAT:
            return false;
        case TYPE_NETWORK_CONNECT:
        case TYPE_NETWORK_DISCONNECT:
        case TYPE_NETWORK_SEND:
        case TYPE_NETWORK_RECV:
        case TYPE_JOIN:
        case TYPE_UNORDERED:
            return true;
        case NUM_TYPES:
        default:
            exit_with_error("Unexpected future type!");
        }
    LOGGED_FUNCTION_END
}

/// Truth that a future type involves processing other than awaiting other
/// futures
///
/// This implies that the future can end up in \ref STATE_PROCESSING or have
/// \ref OUTCOME_FAILURE_INTERNAL, which is impossible otherwise.
///
/// This function must be called within a logging scope.
static inline
bool future_type_has_processing(future_type_t type) {
    LOGGED_FUNCTION_START("%d", type)
        switch (type) {
        case TYPE_NETWORK_CONNECT:
        case TYPE_NETWORK_DISCONNECT:
        case TYPE_NETWORK_SEND:
        case TYPE_NETWORK_RECV:
        case TYPE_CUSTOM:
        case TYPE_TIMER_ONCE:
        case TYPE_TIMER_REPEAT:
            return true;
        case TYPE_INVALID:
        case TYPE_JOIN:
        case TYPE_UNORDERED:
            return false;
        case NUM_TYPES:
        default:
            exit_with_error("Unexpected future type!");
        }
    LOGGED_FUNCTION_END
}

/// Truth that a future type is awaited under the protection of `lazy_lock`
///
/// One example is the Linux implementation of JOIN/UNORDERED/TIMER_REPEAT,
/// where various thread-unsafe operations are performed upon successful wait
/// including inpoll_detach(), setting the payload field of the future... These
/// operations are only safe for a single thread to perform, which is why they
/// are guarded by a lock.
///
/// This function must be called within a logging scope.
static inline
bool future_type_uses_lazy_lock(future_type_t type) {
    LOGGED_FUNCTION_START("%d", type)
        switch (type) {
        case TYPE_INVALID:
        case TYPE_NETWORK_CONNECT:  // Aliases TYPE_NETWORK_START
        case TYPE_NETWORK_DISCONNECT:
        case TYPE_NETWORK_SEND:
        case TYPE_NETWORK_RECV:
        case TYPE_CUSTOM:  // Aliases TYPE_NETWORK_END
        case TYPE_TIMER_ONCE:
            return false;
        #ifdef __linux__
            case TYPE_JOIN:
            case TYPE_UNORDERED:
            case TYPE_TIMER_REPEAT:
                return true;
        #else
            // TODO: Add windows versions
            #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
        #endif
        case NUM_TYPES:
        default:
            exit_with_error("Unexpected future type!");
        }
    LOGGED_FUNCTION_END
}

/// Truth that a future type is meant to be asynchronously signaled by a worker
/// thread upon operation completion
///
/// This affects many things including the future cancelation procedure.
///
/// Future types for which this is false are directly signaled by the operating
/// system or never exposed in an unsignaled state and can therefore be canceled
/// by simply switching them to \ref OUTCOME_FAILURE_CANCELED and \ref
/// STATE_RESULT directly, without any risk of udipe_finish() racing with said
/// worker thread.
///
/// Future types for which this is true must instead go through a more elaborate
/// process where at first they get \ref OUTCOME_FAILURE_CANCELED and \ref
/// STATE_CANCELING, then later the worker thread acknowledges the cancelation
/// by setting them to \ref STATE_RESULT once it is guaranteed that it will
/// never access the future or any associated state again. This will have the
/// effect of unblocking udipe_finish() and letting it liberate the future.
///
/// Other implications of this property include...
///
/// - `status_sync.event` is used if and only if this property is true.
/// - future_type_uses_lazy_lock() is mutually exclusive with this property as
///   if there is the worker thread, it is responsible for setting the future
///   status and therefore no synchronization between clients is necessary.
/// - `status_sync.event` is always opted-in via `notify_event_or_lazy_lock`.
///
/// This function must be called within a logging scope.
static inline
bool future_type_uses_worker_thread(future_type_t type) {
    LOGGED_FUNCTION_START("%d", type)
        switch (type) {
        case TYPE_NETWORK_CONNECT:  // Aliases TYPE_NETWORK_START
        case TYPE_NETWORK_DISCONNECT:
        case TYPE_NETWORK_SEND:
        case TYPE_NETWORK_RECV:
            // TODO: Answer is backend-specific as inline backend performs
            //       operations directly on udipe_start_. Use backend
            //       infrastructure, once available, to answer this.
            exit_with_error("Not implemented yet!");
        case TYPE_CUSTOM:  // Aliases TYPE_NETWORK_END
            return true;
        #ifdef __linux__
            case TYPE_JOIN:
            case TYPE_UNORDERED:
            case TYPE_TIMER_REPEAT:
                return false;
        #else
            // TODO: Add windows versions
            #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
        #endif
        case TYPE_INVALID:
        case TYPE_TIMER_ONCE:
            return false;
        case NUM_TYPES:
        default:
            exit_with_error("Unexpected future type!");
        }
    LOGGED_FUNCTION_END
}
