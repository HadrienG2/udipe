#pragma once

//! \file
//! \brief Implementation of \ref udipe_future_t
//!
//! TODO: Outline new implementation.

#include <udipe/future.h>

#include <udipe/context.h>
#include <udipe/duration.h>
#include <udipe/nodiscard.h>
#include <udipe/result.h>

#include "address_wait.h"
#include "arch.h"
#include "error.h"
#include "event.h"
#include "log.h"
#include "stopwatch.h"

#include <assert.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>


// TODO: This code module is becoming too big and should be split into a
//       directory of smaller code modules. Could be one module for the struct
//       definitions, one for the status word manipulations, one for the
//       allocator, one for the waiting functions...


/// \name Future data structure
/// \{

/// Future state machine
///
/// This is the core state machine of \ref udipe_future_t, which can be used to
/// follow each step of the basic lifecycle of a future.
///
/// It is encoded into selected bits of the \ref future_status_word_t so that
/// 1/it can be atomically checked and modified along with other aspects of the
/// future state and 2/changes to it can be awaited using wait_on_address() for
/// future types that support it.
typedef enum future_state_e /* : _BitInt(3) */ {
    /// Uninitialized state
    ///
    /// Freshly allocated futures are initialized to this state and should be
    /// transitioned into another state before being used. Liberated futures are
    /// transitioned back to this state in debug builds, enabling debug
    /// assertions to cross-check that futures are not used after liberation.
    STATE_UNINITIALIZED = 0,

    /// Waiting-for-futures state
    ///
    /// Futures that depend on other futures (either because they are collective
    /// unordered/join operations or because they are network operations
    /// scheduled after other futures) start in this state when the final state
    /// of the future cannot be directly determined from the initial state of
    /// its dependencies.
    ///
    /// Most futures remain in this state as long as **all** dependencies are in
    /// \ref STATE_WAITING or \ref STATE_PROCESSING, and exit it as soon as one
    /// dependency exits this state. Join futures instead remain in this state
    /// until either 1/**no** dependency is in \ref STATE_WAITING or \ref
    /// STATE_PROCESSING state anymore; or 2/at least one dependency has a moved
    /// to \ref STATE_RESULT with a failure outcome.
    STATE_WAITING,

    /// Work-in-progress state
    ///
    /// Futures that await something other than another future (such a network
    /// operation, a user thread notification, a timer tick...) either begin in
    /// this state if all dependencies are initially ready or transition to this
    /// state after dependencies become ready.
    STATE_PROCESSING,

    /// Cancelation-in-progress state
    ///
    /// Futures whose cancelation require a thread other than the udipe_cancel()
    /// caller to do some work (e.g. removing a network operation from a network
    /// thread's schedule) will transition to this state after a user thread has
    /// expressed its intent to cancel the operation or a dependency has failed,
    /// but before other threads are done processing this cancelation request.
    ///
    /// The purpose of this state is to ensure that waiting functions like
    /// udipe_finish() do not return until all udipe threads are done processing
    /// an asynchronous operation, to avoid race conditions over user-provided
    /// pointers provided at the time where said operation was initiated.
    STATE_CANCELING,

    /// Result-ready state
    ///
    /// Futures reach this state at the point where the associated asynchronous
    /// operation has fully reached a success or error final state and it is
    /// guaranteed that no udipe thread will process this operation anymore.
    ///
    /// Once a future gets to this state, udipe_finish() is guranteed to return
    /// a successful result in a non-blocking manner, and pointer-based
    /// parameters to the asynchronous operation are guaranteed to never be
    /// accessed by internal udipe threads again (unless they were passed to
    /// other pending asynchronous operations, obviously).
    STATE_RESULT,

    /// Not a true state, only needed to count how many states there are
    ///
    NUM_STATES,

    // NOTE: If this enum gets more than 8 variants, reallocate the bit budget
    //       of the `state` field of the future_status_word_t accordingly.
} future_state_t;

/// Future execution outcome
///
/// After a future enters \ref STATE_CANCELING or \ref STATE_RESULT, this enum
/// tracks whether the associated asynchronous operation completed successfully
/// or errored out in some fashion.
///
/// The switch from \ref OUTCOME_UNKNOWN to another outcome only occurs once in
/// the lifetime of a particular future, after which the outcome remains fixed
/// for the rest of the future's useful life. Even if multiple problems could
/// prevent a future from reaching a successful outcome (e.g. a future is
/// canceled, then the operation errors out), only the first problem will be
/// reported via this outcome enum.
///
/// Outcome information is encoded into selected bits of the \ref
/// future_status_word_t so that 1/it can be atomically checked and modified
/// along with other future status info and 2/changes can be awaited using
/// address_wait() for future types that support this synchronization method.
typedef enum future_outcome_e /* : _BitInt(3) */ {
    /// Outcome is not (yet) known
    ///
    /// Futures keep this outcome until their \ref future_state_t reaches \ref
    /// STATE_CANCELING or \ref STATE_RESULT, which is the state where the
    /// outcome of the associated asynchronous operation is actually known.
    /// After being liberated, futures go back to this dummy state.
    OUTCOME_UNKNOWN = 0,

    /// Successful outcome
    ///
    /// Because futures are about asynchronous operation scheduling, the notion
    /// of success that they live by is that it is valid for an operation
    /// scheduled after the current operation to run.
    ///
    /// Future types that exhibit non-fatal failure modes (e.g. packet loss in
    /// UDP) should supplement such a "successful" outcome with warning
    /// information, reported via logging and/or supplementary status fields in
    /// the result payload that is specific to this future type.
    OUTCOME_SUCCESS,

    /// Dependency-induced failure
    ///
    /// This outcome is reported when the asynchronous operation associated with
    /// this future could not start, because it was scheduled after other
    /// futures and at least one of them has reached this outcome or one of the
    /// other failure outcomes described below.
    ///
    /// It cannot be reached for future types that have no dependency.
    OUTCOME_FAILURE_DEPENDENCY,

    /// Internal failure
    ///
    /// This outcome is reported when the asynchronous operation associated with
    /// this future started being processed, but the processing then failed due
    /// to reasons specific to this operation type. For example, failing to send
    /// a network packet due to a "network unreachable" error will lead to this
    /// future outcome.
    ///
    /// Individual future types will provide more specific error reporting via
    /// logging and/or operation-specific metadata in their result payload.
    OUTCOME_FAILURE_INTERNAL,

    /// Cancelation-induced failure
    ///
    /// This outcome is reported when the asynchronous processing associated
    /// with this future was not carried out because the user expressed loss of
    /// interest by passing this specific future to udipe_cancel().
    ///
    /// To make it possible to differentiate such direct cancelation from
    /// indirect cancelation through cancelation of an upstream future which
    /// this future depends on in constant time, the latter is reported via \ref
    /// OUTCOME_FAILURE_DEPENDENCY.
    OUTCOME_FAILURE_CANCELED,

    /// Not a true outcome, only needed to count how many outcomes there are
    ///
    NUM_OUTCOMES,

    // NOTE: If this enum gets more than 8 variants, reallocate the bit budget
    //       of the `state` field of the future_status_word_t accordingly.
} future_outcome_t;

/// Future type
///
/// This identifier is set at the time where a future is created, and never
/// changes afterwards until the future is liberated. It is used by future
/// clients to tell which future type they are dealing with, which is a piece of
/// information that they need when interpreting the various union fields of the
/// \ref udipe_future_t type.
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

    /// Connection setup request from udipe_start_connect()
    ///
    TYPE_NETWORK_CONNECT = TYPE_NETWORK_START,

    /// Connection teardown request from udipe_start_disconnect()
    ///
    TYPE_NETWORK_DISCONNECT,

    /// Datagram send request from udipe_start_send()
    ///
    TYPE_NETWORK_SEND,

    /// Datagram receive request from udipe_start_recv()
    ///
    TYPE_NETWORK_RECV,

    /// First future type past the end of the list of network operations
    ///
    /// See also \ref TYPE_NETWORK_START.
    TYPE_NETWORK_END,

    /// Custom operation
    ///
    TYPE_CUSTOM = TYPE_NETWORK_END,

    /// Join (aka await all)
    ///
    TYPE_JOIN,

    /// Unordered (aka await any)
    ///
    TYPE_UNORDERED,

    /// Single-shot/deadline timer
    ///
    TYPE_TIMER_ONCE,

    /// Multi-shot/periodic timer
    ///
    TYPE_TIMER_REPEAT,

    /// Not a true type, only needed to count how many types there are
    ///
    NUM_TYPES,

    // NOTE: If this enum gets more than 16 variants, reallocate the bit budget
    //       of the `type` field of the future_status_word_t accordingly.
} future_type_t;

/// Maximal value of a future's `downstream_count`
///
/// Attempts to increase a future's downstream count above this should fail and
/// be rolled back.
#define MAX_DOWNSTREAM_COUNT  UINT16_MAX

/// Future status bitfield
///
/// This contains the subset of a future's state that can be read and modified
/// via atomic CPU operations, and whose changes can be awaited via futex
/// operations in some circumstances.
///
/// It can be converted to its machine word representation and back using \ref
/// future_status_word_t.
typedef struct future_status_s {
    /// Number of threads or downstream futures that have expressed interest
    /// in this future's final state and have not processed it yet
    ///
    /// This reference count is initialized to 0 at the time where a future
    /// is created. It is incremented with `memory_order_acquire` when...
    ///
    /// - This future is registered as the `after` dependency of another
    ///   operation (only possible for network operations at the time of
    ///   writing).
    /// - A collective join/unordered future is created and its array of
    ///   upstream futures includes this future.
    /// - A user thread enters a udipe_wait() for this future and observes a
    ///   non-ready state, which means that it truly needs to wait.
    ///
    /// ...and it is decremented with `memory_order_release` when it is
    /// guaranteed that the aforementioned waiter will not be accessing this
    /// future anymore. For example when...
    ///
    /// - A user thread exists udipe_wait() for this future after waiting.
    /// - A collective join/unordered future that had this future as a
    ///   dependency is liberated by udipe_finish().
    /// - A network operation scheduled after this future has read and
    ///   processed its final status, and either was canceled or started
    ///   executing as a result.
    ///
    /// `downstream_count` works together with `available` to enable
    /// use-after-finish detection:
    ///
    /// - By checking that `downstream_count` is zero at the time where the
    ///   `available` flag gets cleared, we can assert that udipe_finish() is
    ///   not called until all previous users of a future are done with it.
    /// - By checking that `available` is set at the time where
    ///   `downstream_count` gets manipulated, we can detect most of the cases
    ///   where another future operation starts executing after udipe_finish()
    ///   has begun.
    ///
    /// It also enables some optimizations where signaling of a future's status
    /// word or output file descriptors can be elided because no one expects
    /// such a signal.
    ///
    /// As this half-word is located in the leading bits of the bitfield, it
    /// can be incremented by incrementing the whole 32-bit word on little
    /// endian CPUs, which are the vast majority of today's CPUs, though
    /// special care will be needed if said increment overflows. See
    /// `downstream_count_overflow` below.
    unsigned downstream_count : 16;

    // --- Byte boundary ---

    /// Guard bit used to detect `downstream_count` overflow
    ///
    /// This bit should always be zero in normal operation. If it ends up
    /// being set to 1, it means that an attempt to increment
    /// `downstream_count` by incrementing the whole status word has
    /// resulted in overflow, and corrective actions must be taken to
    /// restore a valid state (namely the increment must be rolled back and
    /// the attempt to attach a downstream future must be rejected).
    bool downstream_count_overflow : 1;

    /// Truth that this future can be targeted by a public API
    ///
    /// This flag is initially cleared for unallocated futures. It is set with
    /// `memory_order_acq_rel` at the time where a future is initialized, and
    /// cleared with `memory_order_acq_rel` at the start of udipe_finish().
    ///
    /// In combination with `downstream_count` tracking, this enables
    /// use-after-finish detection, as explained in the documentation of
    /// `downstream_count`.
    bool available : 1;

    /// Future state machine
    ///
    /// This tracks at which stage of the future lifecycle this future
    /// currently is. It is a \ref future_state_t value casted into an
    /// integer, see the documentation of this enum for more information.
    unsigned state : 3;

    /// Asynchronous operation outcome
    ///
    /// This tracks the outcome of the asynchronous operation associated
    /// with this future. It is a \ref future_outcome_t value casted into an
    /// integer, see the documentation of this enum for more information.
    unsigned outcome : 3;

    // --- Byte boundary ---

    /// Type of future
    ///
    /// This constant field, which is a \ref future_type_t casted into an
    /// integer, is needed to correctly interprete and manipulate other
    /// fields of this status word, and \ref udipe_future_t in general.
    ///
    /// As mentioned in the docs of \ref future_type_t, this information
    /// does not strictly need to be in the status word, and can be moved
    /// out to another future field if we start running out of precious
    /// status bits in a future udipe version.
    unsigned type : 4;

    /// Truth that changes to this status word should be notified through a
    /// call to wake_by_address_all()
    ///
    /// This flag is initially unset when a future is set up. It is set on the
    /// first time where a thread starts waiting for state changes via
    /// wait_on_address(), and cannot be unset afterwards until the future is
    /// liberated. From the point where this flag is set, all status word
    /// changes will be notified via a variant of `wake_by_address` (usually
    /// wake_by_address_all(), but lazy futures also use
    /// wake_by_address_single() to avoid thundering herds when they transfer
    /// the lazy update lock from one waiter to another).
    ///
    /// The reason why this is a sticky flag and not a counter of waiters is
    /// that we don't have enough bits in this status word to afford more than
    /// one counter of reasonable range... So we can afford to avoid unnecessary
    /// futex syscalls on futures that never need a futex syscall, but not on
    /// futures that intermittently need these.
    bool notify_address : 1;

    /// Request for `output_fd.event` signaling or lazy future state locking
    ///
    /// The meaning of this field depends on whether you are dealing with an
    /// "eager" future, whose status is automatically changed by a dedicated
    /// thread, or with a "lazy" future, whose status is changed as a result of
    /// polling a file descriptor.
    ///
    /// # Eager future: Request for `output_fd.event` signaling
    ///
    /// "Eager" futures support address-based signaling, in contrast to "lazy"
    /// futures which only support file descriptor signaling. Therefore eager
    /// futures do not always need to signal changes through their output event
    /// object, and require a flag to be set before they start engaging in this
    /// form of signaling.
    ///
    /// For these futures, this flag works just like `notify_address`: initially
    /// unset, set the first time a thread expresses interest in receiving
    /// updates through the `output_fd.event` path, and cannot be unset
    /// afterwards until the future is destroyed.
    ///
    /// # Lazy future: Lock for lazily updating the future state
    ///
    /// "Lazy" future types are not eagerly updated by a thread which is in
    /// charge of performing the asynchronous work. Instead they get lazily
    /// updated, usually at the point where a user thread starts directly or
    /// indirectly waiting for their output epollfd to signal a status change.
    ///
    /// Because these future types may be concurrently awaited by multiple
    /// threads, access to their lazily updated internal state must be
    /// synchronized somehow. For collective and repeating timer futures, which
    /// have complex internal state that cannot be updated in a single atomic
    /// RMW operation, this is ensured by using this flag as a lock. When such a
    /// future's output fd becomes ready, indicating a possible state change,
    /// the thread that gets awakened as a result must...
    ///
    /// - Check if this locking flag is already set.
    ///   - If so, another thread is already in the process of querying the file
    ///     descriptor, and this thread can do nothing but wait. To do this, set
    ///     the `notify_address` flag if needed, then use a wait_on_address()
    ///     loop to wait for the other thread that arrived first to report the
    ///     final state (or release the lock in some other way).
    ///   - If not, attempt to set this flag, and if successful perform any
    ///     required state update operation finishing with the status word
    ///     (clearing this lock flag along the way), then signal the status word
    ///     change via wake_by_address_all() if `notify_address` is set. If you
    ///     need to exit before the outcome is available due to a timeout, then
    ///     pass on the lock to the next waiter with wake_by_address_single().
    bool notify_event_or_lazy_lock : 1;

    /// Those spare bits are reserved for future use and must be set to 0
    ///
    unsigned reserved : 2;

    // NOTE: This bitfield cannot grow beyond the end of the above byte.
} future_status_t;

/// Future status word
///
/// This union holds a bitfield which encodes most future state relevant to
/// thread synchronization into a 32-bit integer, such that said state can be
/// atomically read and modified with a single RMW CPU instruction and can be
/// atomically awaited via address_wait() on future types that support it.
///
/// Use the `as_bitfield` variant to access its contents in a fine-grained
/// manner, and the `as_word` variant to translate back and forth between this
/// fine-grained representation and the 32-bit representation that is required
/// to be able to use C11 `_Atomic` and Linux futex operations.
typedef union future_status_word_u {
    /// Bitfield representation
    ///
    /// Used for any kind of logical status word readout or manipulation.
    future_status_t as_bitfield;

    /// Integral representation of a future status word
    ///
    /// Used to encode the `as_bitfield` state into an integer for the purpose
    /// of later injecting it into \ref udipe_future_t::status_word via atomic
    /// read-modify-write operations.
    uint32_t as_word;
} future_status_word_t;

/// Set of upstream futures which collective futures depend on
///
/// Collective joined and unordered futures all need to lazily poll a set of
/// other futures, which is tracked via this struct.
typedef struct collective_upstream_s {
    /// Array of upstream futures that this future is awaiting
    ///
    /// This array must not be accessed after the point where `status_word` is
    /// set to \ref STATE_RESULT, as the user is allowed to liberate the
    /// associated memory after this point.
    ///
    /// Must have been checked to contain no duplicates and no `NULL`s at
    /// collective future construction time. As long as we do not expose an
    /// output fd accessor that lets users call `dup()`, `epoll_ctl()` should
    /// take care of the former for us.
    udipe_future_t* const* array;

    /// Number of upstream futures in `array`
    ///
    /// This is needed in order to be able to detach all upstream futures when a
    /// collective operation gets canceled. It can also be used for
    /// bounds-checking assertions in debug builds.
    //
    // TODO: Make sure len is not above UINT32_MAX when creating a collective.
    uint32_t length;

    /// Number of upstream futures that have not yet reached \ref
    /// OUTCOME_SUCCESS
    ///
    /// This counter is initialized to `len` then decremented every time one of
    /// the upstream futures reaches \ref OUTCOME_SUCCESS. If it reaches 0,
    /// all upstream futures have reached \ref OUTCOME_SUCCESS.
    ///
    /// - Join futures do not switch to \ref OUTCOME_SUCCESS until all upstream
    ///   futures have reached \ref OUTCOME_SUCCESS.
    /// - Unordered futures emit a new future whenever this counter is
    ///   decremented, with a `next` pointer that points to a successor future
    ///   if this counter has not reached zero yet. If this counter has reached
    ///   zero, `next` is set to `NULL`, marking the end of the unordered chain.
    ///
    /// When one of the upstream futures reaches a non-successful outcome, this
    /// counter becomes useless and is allowed to take any value.
    ///
    /// This counter must be read and written under `lazy_lock` protection.
    uint32_t remaining;
} collective_upstream_t;

/// eventfd used to mark a lazy future as permanently ready once it has reached
/// its final outcome, after its final status has been set
///
/// Most lazy futures (those whose status is set by the client thread that
/// awaits them, as opposed to eager future which are processed by a background
/// thread) must have an `output_fd` which is an epollfd. One limitation of
/// epollfds as an output file descriptor is that they stop being ready after
/// their readiness notifications have been processed and the associated file
/// descriptor has been detached, thus requiring an additional eventfd in the
/// epollfd interest list to ensure continued readiness after the final status
/// has becomes available.
///
/// The exception is \ref TYPE_TIMER_ONCE, which is simple enough to avoid the
/// need for an output epollfd, and can instead expose its internal timerfd
/// directly as its `output.timer`. This timerfd does not need to be read by
/// clients and can thus remain in the perpetually ready unread state, which
/// acts as that future type's output readiness notification.
///
/// These eventfds should be set to under `lazy_lock` protection, and must be
/// reset and recycled along with the associated `output.epoll_with_event` at
/// the time where the associated future is liberated.
//
// TODO: Find the Windows equivalent of this pattern. Since windows does not
//       have epoll, the simplest option might be to make all futures eager and
//       use the Win32 thread pool to await dependencies + an output event
//       object or WakeByAddress to signal dependents.
typedef event_t outcome_event_t;

/// \copydoc udipe_future_t
struct udipe_future_s {
    /// udipe context which this future belongs to
    ///
    /// Used to ensure that future methods do not need an additional context
    /// parameter after future allocation.
    alignas(FALSE_SHARING_GRANULARITY) udipe_context_t* context;

    /// State that is specific to a particular future type
    ///
    /// At most one of these fields will be set. Which will be set (if any)
    /// depends on the \ref future_type_t that is set inside of the bitpacked
    /// `status_word`.
    union {
        /// Eager command result
        ///
        /// Eager commands are, as the name suggests, eagerly processed by some
        /// thread, which is an internal udipe thread for network commands and a
        /// user thread for custom commands. Once the thread responsible for
        /// processing a command is done, its result will be written down to
        /// this field before signaling \ref OUTCOME_SUCCESS with
        /// `memory_order_release`.
        ///
        /// This union variant will be used if either the \ref future_type_t is
        /// in range from \ref TYPE_NETWORK_START inclusive to \ref
        /// TYPE_NETWORK_END exclusive, or it is \ref TYPE_CUSTOM.
        union {
            /// Network command result payload
            ///
            /// This union variant will be set before signaling the outcome if
            /// the \ref future_type_t is in range from \ref TYPE_NETWORK_START
            /// inclusive to \ref TYPE_NETWORK_END exclusive.
            ///
            /// The precise \ref future_type_t you are dealing with will tell
            /// you which variant of this payload union has been set.
            udipe_network_payload_t network;

            /// Custom user command result payload
            ///
            /// This union variant will be set before signaling \ref
            /// OUTCOME_SUCCESS if the \ref future_type_t is \ref TYPE_CUSTOM.
            udipe_custom_payload_t custom;
        } eager;

        /// Joined future state
        ///
        /// This union variant corresponds to \ref TYPE_JOIN. It tracks the
        /// state needed to wait for all specified upstream futures to reach
        /// \ref OUTCOME_SUCCESS or at least one of them to reach a failing
        /// outcome. And when this happens, it makes it possible to signal
        /// availability of the final status after it has been set.
        struct {
            /// Set of upstream futures awaited by this collective future
            ///
            collective_upstream_t upstream;

            /// Event object used to mark this future as permanently ready after
            /// it has reached its final outcome
            ///
            /// See \ref outcome_event_t for more information.
            outcome_event_t outcome_event;
        } join;

        /// Unordered future state and result
        ///
        /// This union variant corresponds to \ref TYPE_UNORDERED. It tracks the
        /// state needed to wait for at least one of the specified upstream
        /// futures to reach its final outcome. And when this happens, it makes
        /// it possible to report which future got ready and how to await
        /// subsequent futures (if any).
        struct {
            /// Set of upstream futures awaited by this collective future
            ///
            collective_upstream_t upstream;

            /// Result of the asynchronous operation
            ///
            /// This result is set before signaling \ref OUTCOME_SUCCESS. It
            /// indicates which of the upstream futures became ready and how to
            /// await the rest of the upstream futures.
            ///
            /// Must be written under `lazy_lock` protection. Inner future (if
            /// any) must not be recycled on udipe_finish(), as it will be fed
            /// to the caller which is responsible for liberating it.
            udipe_unordered_payload_t payload;

            /// Inner epollfd that contains the set of upstream futures, which
            /// is awaited by the output epollfd
            ///
            /// This epollfd indirection is used to easily migrate the waiting
            /// set to successor unordered futures without affecting clients of
            /// this specific unordered future.
            ///
            /// Must be awaited under `lazy_lock` protection, detached from the
            /// output epollfd and attached to the successor future (if any)
            /// once a result is ready.
            ///
            /// Must be destroyed when the latest future in the unordered chain
            /// is liberated. There seems to be little point in trying to
            /// recycle epollfds attached to all upstreams because resetting
            /// them requires an arbitrary amount of epoll_ctl() calls and
            /// setting up the next future also requires an arbitrary amount of
            /// epoll_ctl() calls, so it's not expected that epollfd
            /// allocation/liberation will ever be the bottleneck.
            //
            // TODO: Prove the above assertion through benchmarking and
            //       profiling of real-world workloads.
            ///
            /// Unlike the output epollfd, this epoll set does not contain an
            /// eventfd and will only yield valid upstream indices.
            //
            // TODO: Find an epoll replacement for Windows. Will most likely be
            //       based on the Win32 thread pool driving an eager future.
            int upstream_epollfd;

            /// Event object used to mark this future as permanently ready after
            /// it has reached its final outcome
            ///
            /// See \ref outcome_event_t for more information.
            outcome_event_t outcome_event;
        } unordered;

        /// Repeating timer state
        ///
        /// This union variant corresponds to \ref TYPE_TIMER_REPEAT. It tracks
        /// the state needed to report how many timer ticks elapsed and how to
        /// await subsequent timer ticks.
        struct {
            /// Result of the asynchronous operation
            ///
            /// This field is set before signaling \ref OUTCOME_SUCCESS. It
            /// indicates how many clock ticks were missed and how to await
            /// further clock ticks if desired.
            udipe_timer_repeat_payload_t payload;

            /// timerfd which the output epollfd is awaiting
            ///
            /// This epollfd indirection is used to easily migrate the timerfd
            /// to successor unordered futures without affecting clients of this
            /// specific unordered future.
            ///
            /// Must be read under `lazy_lock` protection, and detached from the
            /// output epollfd then attached to the successor future's output
            /// epollfd when a result is ready.
            ///
            /// Must be destroyed when the future is liberated, for now. May
            /// switch to disarming and recycling if timerfd
            /// creation/destruction ever becomes a bottleneck, but that seems
            /// unlikely under correct usage since there is no envisioned use
            /// case where one would need lots of periodic futures with
            /// different periodicities.
            //
            // TODO: Prove the above assertion through benchmarking and
            //       profiling of real-world workloads.
            // TODO: Find a windows equivalent, based on Win32 thread pool
            //       timers? That seems necessary to be able to count missed
            //       deadlines, which is a very nice feature to have.
            int timerfd;

            /// Event object used to mark this future as permanently ready after
            /// it has reached its final outcome
            ///
            /// See \ref outcome_event_t for more information.
            outcome_event_t outcome_event;
        } timer_repeat;
    } specific;

    /// Status word
    ///
    /// This innocent-looking 32-bit word actually contains most of the
    /// synchronization-critical state of a future, bitpacked via \ref
    /// future_status_word_t::as_word so that it can be used for atomic
    /// read-modify-write operations and futex syscalls.
    ///
    /// A future's status word does double duty as a futex that can sometimes
    /// (but not always) be awaited with wait_for_address() to await
    /// `status_word` changes. When a future supports this signaling protocol,
    /// it must be requested first by setting the `notify_address` field of the
    /// status word, before beginning the wait for status changes via
    /// wait_for_address().
    ///
    /// Please refer to \ref future_status_word_t::as_bitfield for more
    /// information about what information is stored into this word.
    ///
    /// As status changes are often preceded by other future state changes, bear
    /// in mind that changes to `status_word` must often be carried out with
    /// `memory_order_release` and status word readouts must often be carried
    /// out with `memory_order_acquire`.
    _Atomic uint32_t status_word;

    /// Output file descriptor
    ///
    /// It is possible to await future status changes by adding a file
    /// descriptor to an epollfd's interest list, but the kind of file
    /// descriptor that is used and the way it should be awaited by clients
    /// varies depending on which future type you're dealing with.
    ///
    /// Check the `type` field of the status word to know more about which
    /// variant of this union you are dealing with, then read the associated
    /// description for more info.
    //
    // TODO: Windows version, based on NT synchronization objects?
    union {
        /// Event object, used for eager futures
        ///
        /// This output type is used for "eager" future types where the
        /// asynchronous operation is processed by a dedicated thread, namely
        /// network and custom operation futures.
        ///
        /// Because these futures can also signal completion via a futex or even
        /// via a mere atomic RMW operation when no one is waiting for
        /// completion yet, event signaling is optional for these future types
        /// and must be explicitly requested by setting the
        /// `notify_event_or_lazy_lock` bit of `status_word` before awaiting
        /// this event object (and checking that the status word did not switch
        /// to a ready state concurrently).
        ///
        /// When the task's outcome has been filed into the status word, if
        /// `notify_event_event_or_lazy_lock` is set, the event object will be
        /// signaled, which will mark it as ready for all threads that are
        /// monitoring it. These threads will then proceed to read the outcome
        /// in the status word, completing the synchronization transaction.
        ///
        /// Must be reset and recycled when the future is liberated.
        event_t event;

        /// timerfd, used for \ref TYPE_TIMER_ONCE
        ///
        /// This output file descriptor type is used for single-shot "timer"
        /// futures that become ready once the system clock reaches a certain
        /// time point.
        ///
        /// When this file descriptor becomes ready, \ref OUTCOME_SUCCESS must
        /// be signaled by one of the thread which observes this status to be
        /// unset, without reading the timerfd. The timerfd will therefore stay
        /// ready and thus remain effective as a downstream readiness signal.
        ///
        /// Must be destroyed when the future is liberated, for now. May switch
        /// to disarming and recycling if timerfd creation/destruction ever
        /// becomes a bottleneck, but that seems unlikely under correct udipe
        /// API usage given that recuring timerfds seem to cover the main use
        /// case for which one might want to create lots of single-shot timers.
        //
        // TODO: Prove the above assertion through benchmarking and profiling of
        //       real-world workloads.
        // TODO: Find a Windows equivalent, could likely be a Win32 waitable
        //       timer object?
        int timer;

        /// epollfd with an attached \ref outcome_event_t, for most lazy futures
        ///
        /// This output file descriptor type is mainly used for "collective"
        /// future types that await several other futures. It is, as the name
        /// implies, an epollfd that monitors the readiness of the output file
        /// descriptors of all upstream futures, along with an additional
        /// eventfd which indicates availability of this future's (successful or
        /// failing) outcome.
        ///
        /// How exactly dependencies are awaited depends on the kind of
        /// future that you are dealing with:
        ///
        /// - Join futures use a single epollfd that encompasses all fds of
        ///   interest. Dependency fds use their index in the array of
        ///   dependencies as an epoll identifier, while the outcome
        ///   availability eventfd uses an identifier of `UINT64_MAX`.
        /// - Unordered futures use a cascaded pair of epollfds. Their
        ///   dependencies are attached to an "inner"
        ///   `specific.unordered.upstream_epollfd` with index-based signaling
        ///   as before, but no accompanying eventfd. This "inner" epollfd is in
        ///   turn attached with identifier 0 to this "outer" epollfd, which is
        ///   additionally attached to the \ref outcome_event_t with identifier
        ///   `UINT64_MAX`. This curious cascading epollfd structure makes it
        ///   possible to later detach the inner epollfd and migrate it to the
        ///   next future in the unordered chain, while leaving the original
        ///   future's `output.epoll_with_event` in a signaled state.
        /// - Repeating timers produce a chain output futures using the same
        ///   trick, except instead of cascading epollfds they simply have one
        ///   output epollfd which is connected to a timerfd (that performs
        ///   time-based signaling and can be detached and migrated to the next
        ///   future in the chain) and an eventfd (that eventually remains
        ///   attached to the epollfd in a perpetually signaled state to
        ///   broadcast the information that the final outcome is available).
        ///
        /// Because epoll's API design is not very friendly to multi-threaded
        /// use, `epoll_wait()` on the inner epollfds requires `lazy_lock`
        /// protection, which effectively acts as a mutex to control access to
        /// the epollfd and associated future state.
        ///
        /// Whenever `epoll_wait()` output indicates that a particular upstream
        /// future has underwent a status change or this future has been
        /// canceled, the status word of the upstream future (if any) must be
        /// checked, then the fields of this collective future must be modified
        /// accordingly, and finally any other thread which registered to be
        /// notified of state changes while we were probing `epoll_wait()` must
        /// be notified via `wake_by_address_`. Once the future outcome is
        /// known, whether successful or unsuccessful, its availability must be
        /// signaled via the dedicated eventfd if at least one other future
        /// awaited this future, as indicated by `downstream_count`.
        ///
        /// At least for joined futures, this epollfd must be destroyed along
        /// with the host future. There seems to be little point in trying to
        /// recycle epollfds for these futures, because resetting their epollfd
        /// requires an arbitrary amount of epoll_ctl() calls and setting up the
        /// next epollfd for another futures also requires an arbitrary amount
        /// of epoll_ctl() calls. It is therefore dubious that epollfd
        /// allocation/liberation will ever be such a bottleneck that the extra
        /// overhead of recycling (which is high in the case of epollfds)
        /// becomes worthwhile.
        ///
        /// For unordered and periodic timer futures, however, the epollfd only
        /// has a small amount of futures attached to it (1 eventfd + either one
        /// epollfd for unordered or one timerfd for periodic timers), and many
        /// such epollfd+eventfd pairs may be needed by the arbitrary many
        /// continuation futures that will follow in the chain. In this case, it
        /// is expected that recycling the output epollfd along with its
        /// (still-attached) associated \ref outcome_event_t could be
        /// worthwhile.
        //
        // TODO: Prove the above assertions through benchmarking and profiling
        //       of real-world workloads.
        // TODO: Find an epoll replacement for Windows. Will most likely be
        //       based on the Win32 thread pool driving an event object.
        int epoll_with_event;

        /// Catch-all file descriptor type
        ///
        /// Use this union variant in situations where the active file
        /// descriptors doesn't matter, such as when managing attachment of
        /// output fds to collective future epollfds.
        //
        // TODO: Figure out if Windows can have this convenience too, I think
        //       that is the case if we use `HANDLE` as the catch-all type for
        //       all Win32 synchronization objects. In that case, we just need
        //       to make the wording less file descriptor-specific.
        int any;
    } output_fd;
};
static_assert(alignof(udipe_future_t) == FALSE_SHARING_GRANULARITY,
              "Each future potentially synchronizes different workers and "
              "client threads, and should therefore reside on its own "
              "false sharing granule");
static_assert(sizeof(udipe_future_t) == FALSE_SHARING_GRANULARITY,
              "Should not need more than one false sharing granule per future");
static_assert(
    offsetof(udipe_future_t, output_fd) + sizeof(uint32_t) <= CACHE_LINE_SIZE,
    "Should fit on a single cache line for optimal memory access performance "
    "on CPUs where the FALSE_SHARING_GRANULARITY upper bound is pessimistic"
);
static_assert(sizeof(udipe_result_t) <= CACHE_LINE_SIZE,
              "Should always be true because future is a superset of result");

/// \}


/// \name Future status word manipulation
/// \{

/// Check that a future's status word is internally consistent in debug builds
///
/// This function can be used whenever a new value of a future's status word is
/// observed or injected, to ensure that no inconsistent state slips in as a
/// result of a bug.
///
/// \param status is the observed/injected future status
/// \param is_allocated indicates whether the future is currently allocated to
///                     some work or simply lying around in some recycling pool
///                     waiting to be picked up for another task.
void future_status_debug_check(future_status_t status,
                               bool is_allocated);

/// How the downstream count of a future should be changed during a wait
///
/// The downstream count is a reference count, stored within a future's status
/// word, which is used to detect use-after-free patterns where udipe_finish()
/// is called before all users of a future are done with it.
///
/// When a thread waits for a future to be ready, it normally increments the
/// downstream count at the beginning of the wait and decrements it at the end,
/// as modeled by the `DOWNSTREAM_COUNT_CYCLE` variant of this enum. But there
/// are a few exceptions to this general rule, motivated by performance and
/// correctness reasons. Hence this tuning knob.
typedef enum downstream_count_policy_e {
    /// Increment the downstream count at the start and decrement it at the end
    ///
    /// This is the normal policy of udipe_wait(). Any other policy assumes
    /// something about the way the future has been previously manipulated or
    /// will be manipulated in the future, and must therefore only be used in
    /// special circumstances.
    DOWNSTREAM_COUNT_CYCLE = 0,

    /// Do not change the downstream count during the waiting process
    ///
    /// This special policy is needed in two circumstances:
    ///
    /// - udipe_finish(), which ends a future's lifetime, does not manipulate
    ///   the downstream count. It will instead clear the `available` flag at
    ///   the beginning and make sure the downstream count is zero at that time.
    /// - Collective futures like \ref TYPE_JOIN and \ref TYPE_UNORDERED stay
    ///   attached to their upstream futures until they are done waiting for
    ///   them. Thus the downstream count of upstream futures is incremented
    ///   initially as the collective future is created, and only decremented as
    ///   the collective future is liberated by udipe_finish(). This has two
    ///   benefits:
    ///     - The performance overhead of incrementing and decrementing the
    ///       downstream count of upstream futures multiple times throughout the
    ///       waiting process is avoided.
    ///     - The downstream count stays at a nonzero value as long as the
    ///       collective future may access it, thus reducing the odds of
    ///       use-after-finish detection failure.
    DOWNSTREAM_COUNT_KEEP,
} downstream_count_policy_t;

/// Apply the effect of future_downstream_count_try_inc() to a local status word
///
/// This function can be used when a thread is getting ready to await a
/// non-ready future and needs to change fields of the status word other than
/// the `downstream_count`. When no other status word field needs to be changed,
/// future_downstream_count_try_inc() should be used instead.
///
/// Once you are ready to commit this status word change through a
/// `compare_exchange` transaction, you should make sure that said transaction
/// has a memory ordering of `acquire` or stronger (`acq_rel`, `seq_cst`) on
/// success. This is needed to ensure that no later thread action targeting
/// the future may be reordered before the `downstream_count` increment.
///
/// It only makes sense to apply this sort of fine-grained transaction to the
/// status word of a live future that's allocated to some work. Unallocated
/// futures can be manipulated by simply overwriting their status with
/// future_status_store().
///
/// \param status should initially point to the latest known future status word,
///               or a version of it which already started receiving some of the
///               desired changes. This function will take care of applying any
///               needed downstream_count change.
/// \param count_policy indicates the downstream count policy which the caller
///                     of this function operates under.
UDIPE_NON_NULL_ARGS
static inline
void prepare_downstream_count_inc(future_status_t* status,
                                  downstream_count_policy_t count_policy) {
    future_status_debug_check(*status, true);
    assert(status->state != STATE_RESULT);
    ensure(!status->downstream_count_overflow);
    switch (count_policy) {
    case DOWNSTREAM_COUNT_CYCLE:
        ensure_lt((size_t)status->downstream_count,
                  (size_t)MAX_DOWNSTREAM_COUNT);
        ++(status->downstream_count);
        break;
    case DOWNSTREAM_COUNT_KEEP:
        break;
    }
}

/// Apply the effect of future_downstream_count_dec() to a local status word
///
/// This function can be used when a thread is done awaiting a future and needs
/// to change fields of the status word other than the `downstream_count`. When
/// no other status word field needs to be changed,
/// future_downstream_count_dec() should be used instead.
///
/// Once you are ready to commit this status word change through a
/// `compare_exchange` transaction, you should make sure that said transaction
/// has a memory ordering of `release` or stronger (`acq_rel`, `seq_cst`) on
/// success. This is needed to ensure that no previous thread action targeting
/// the future may be reordered after the `downstream_count` decrement.
///
/// It only makes sense to apply this sort of fine-grained transaction to the
/// status word of a live future that's allocated to some work. Unallocated
/// futures can be manipulated by simply overwriting their status with
/// future_status_store().
///
/// \param status should initially point to the latest known future status word,
///               or a version of it which already started receiving some of the
///               desired changes. This function will take care of applying any
///               needed downstream_count change.
/// \param count_policy indicates the downstream count policy which the caller
///                     of this function operates under.
UDIPE_NON_NULL_ARGS
static inline
void prepare_downstream_count_dec(future_status_t* status,
                                  downstream_count_policy_t count_policy) {
    future_status_debug_check(*status, true);
    ensure(!status->downstream_count_overflow);
    switch (count_policy) {
    case DOWNSTREAM_COUNT_CYCLE:
        ensure_ge((size_t)status->downstream_count,
                  (size_t)1);
        --(status->downstream_count);
        break;
    case DOWNSTREAM_COUNT_KEEP:
        break;
    }
}

/// Check if two future status words are equal
///
static inline
bool future_status_eq(future_status_t s1, future_status_t s2) {
    return (future_status_word_t){ .as_bitfield = s1 }.as_word
        == (future_status_word_t){ .as_bitfield = s2 }.as_word;
}

/// Initialize a future's status word
///
/// This operation is not atomic and must never be called from multiple threads.
/// It should only be used when a future is initially allocated, and must never
/// be called at a time where other threads might be accessing the future.
///
/// At any point of a future's lifetime after initialization, including when the
/// future is recycled into a thread-local cache and later recalled from said
/// cache, future_status_store() should be preferred over this function.
UDIPE_NON_NULL_ARGS
static inline
void future_status_initialize(udipe_future_t* future,
                              future_status_t status) {
    future_status_debug_check(status, false);
    atomic_init(&future->status_word,
                (future_status_word_t){ .as_bitfield = status }.as_word);
}

/// Atomically read a future's current status
///
/// This operation has the semantics of `atomic_load_explicit()`. In particular,
/// any value from it should be treated as potentially stale as the future
/// status continues evolving after readout.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
static inline
future_status_t future_status_load(const udipe_future_t* future,
                                   memory_order order) {
    return (future_status_word_t){
        .as_word = atomic_load_explicit(&future->status_word, order)
    }.as_bitfield;
}

/// Atomically change a future's status
///
/// This operation has the semantics of `atomic_store_explicit()`. It is mostly
/// used as a safer alternative to future_status_init() during the process of
/// recycling a future via the thread-local cache. But it generally cannot be
/// used for thread synchronization due to the risk of overwriting status
/// changes caused by other threads since the last readout. You will usually
/// need `compare_exchange` operations for such use cases.
UDIPE_NON_NULL_ARGS
static inline
void future_status_store(udipe_future_t* future,
                         future_status_t status,
                         memory_order order) {
    atomic_store_explicit(
        &future->status_word,
        (future_status_word_t){ .as_bitfield = status }.as_word,
        order
    );
}

/// Atomically change a future's status assuming a certain initial status
///
/// This operation has the semantics of
/// `atomic_compare_exchange_strong_explicit()`. It is the main way through
/// which a future's status can be changed in a thread-safe manner.
///
/// If you can do nothing but call this operation in a loop until the write
/// succeeds, you should use future_status_compare_exclange_weak() instead.
///
/// If the only field you want to change is the `downstream_count`, consider
/// using `future_downstream_count_` operations as a more efficient alternative.
///
/// It only makes sense to apply this sort of fine-grained transaction to the
/// status word of a live future that's allocated to some work. Unallocated
/// futures can be manipulated by simply overwriting their status with
/// future_status_store().
///
/// \param future must be a future that was returned by an asynchronous function
///               (those whose name begins with `udipe_start_`) and has not been
///               liberated by udipe_finish() or udipe_cancel() since.
/// \param expected points to the status that the future is initially expected
///                 to have. If the future turns out not to have this initial
///                 status, then the actual status will be read with `failure`
///                 ordering then stored at this memory location.
/// \param desired indicates the desired new future status. The future will
///                switch to this status with `success` ordering if it turns out
///                that its initial status is indeed `expected`.
/// \param success is the memory ordering that this operation should have in
///                case the comparison of the future's status with `expected`
///                succeeds and its status does change to `desired`.
/// \param failure is the memory ordering that this operation should have in
///                case the comparison of the future's status with `expected`
///                fails and its status does not change (it is only read for the
///                sake of updating `expected` in a non-atomic fashion).
///
/// \returns the truth that the future's status did change to `desired`.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
static inline
bool future_status_compare_exchange_strong(udipe_future_t* future,
                                           future_status_t* expected,
                                           future_status_t desired,
                                           memory_order success,
                                           memory_order failure) {
    future_status_debug_check(*expected, true);
    future_status_debug_check(desired, true);
    future_status_word_t expected_word = (future_status_word_t){
        .as_bitfield = *expected
    };
    bool result = atomic_compare_exchange_strong_explicit(
        &future->status_word,
        &expected_word.as_word,
        (future_status_word_t){ .as_bitfield = desired }.as_word,
        success,
        failure
    );
    *expected = expected_word.as_bitfield;
    if (!result) future_status_debug_check(*expected, true);
    return result;
}

/// Atomically change a future's status assuming a certain initial status,
/// allowing for spurious failure
///
/// This operation has the semantics of
/// `atomic_compare_exchange_weak_explicit()`. It is used as a more efficient
/// alternative to future_status_compare_exchange_strong() in situations where
/// it would be called in a loop until the write succeeds.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
static inline
bool future_status_compare_exchange_weak(udipe_future_t* future,
                                         future_status_t* expected,
                                         future_status_t desired,
                                         memory_order success,
                                         memory_order failure) {
    future_status_debug_check(*expected, true);
    future_status_debug_check(desired, true);
    future_status_word_t expected_word = (future_status_word_t){
        .as_bitfield = *expected
    };
    bool result = atomic_compare_exchange_weak_explicit(
        &future->status_word,
        &expected_word.as_word,
        (future_status_word_t){ .as_bitfield = desired }.as_word,
        success,
        failure
    );
    *expected = expected_word.as_bitfield;
    if (!result) future_status_debug_check(*expected, true);
    return result;
}

/// Wait for a future's status word to change away from a certain value, return
/// truth that it may have changed (otherwise the wakeup was spurious)
///
/// This function has the same semantics as wait_on_address(), but it is meant
/// to be called and operates on their status word in its decoded bitfield form.
///
/// It only makes sense to apply this sort of synchronization to the status word
/// of a live future that's allocated to some work. Unallocated futures can be
/// manipulated by simply overwriting their status with future_status_store().
///
/// This function must be called within the scope of with_logger().
UDIPE_NON_NULL_ARGS
static inline
bool future_status_wait(udipe_future_t* future,
                        future_status_t expected,
                        udipe_duration_ns_t timeout) {
    future_status_debug_check(expected, true);
    return wait_on_address(
        &future->status_word,
        (future_status_word_t){ .as_bitfield = expected }.as_word,
        timeout
    );
}

/// Atomically decrement a future's downstream count, return the new future
/// status
///
/// This should be done whenever a downstream entity, such as a join future, is
/// done inspecting the state of this future and will never touch it again.
///
/// As with future_downstream_count_try_inc(), if you need to modify other
/// fields of the future status word, you should batch these two updates into a
/// single `future_status_compare_exchange_` transaction. In this case, you can
/// use prepare_downstream_count_dec() as preparation for your custom CAS
/// transaction.
///
/// It only makes sense to apply this sort of fine-grained transaction to the
/// status word of a live future that's allocated to some work. Unallocated
/// futures can be manipulated by simply overwriting their status with
/// future_status_store().
///
/// \param future must be a future that was returned by an asynchronous function
///               (those whose name begins with `udipe_start_`) and has not been
///               liberated by udipe_finish() or udipe_cancel() since.
/// \param order is the memory ordering associated with the downstream count
///              decrement. To ensure that no future operation is reordered
///              after the downstream count decrement, this ordering should
///              almost always be `release` or stronger (`acq_rel`, `seq_cst`).
///
/// \returns the future status word after this operations has been applied. If
///          you use or expose this value, the memory ordering of this operation
///          should usually be at least `acq_rel` so that you synchronize-with
///          future state changes signaled by other threads.
UDIPE_NON_NULL_ARGS
static inline
future_status_t future_downstream_count_dec(udipe_future_t* future,
                                            memory_order order) {
    const future_status_t pre_op_status = (future_status_word_t){
        .as_word = atomic_fetch_sub_explicit(&future->status_word,
                                             1,
                                             order)
    }.as_bitfield;
    future_status_debug_check(pre_op_status, true);
    future_status_t result = pre_op_status;
    assert(result.downstream_count >= 1);
    --result.downstream_count;
    future_status_debug_check(result, true);
    return result;
}

/// Attempt to increment a future's downstream count as preparation for
/// awaiting its result
///
/// This should be done in situations where...
///
/// - An application thread has observed that a future is not ready yet, and
///   is getting ready to wait for that future to reach \ref STATE_RESULT.
/// - No other change to the future status word is necessary.
///
/// If you need to perform other changes to the future's status word, then you
/// should batch up all desired changes into a single
/// `future_status_compare_exchange_` loop, as it will be more efficient than
/// performing multiple RMW operations on the future status word. In this case,
/// you can use prepare_downstream_count_inc() as preparation for your custom
/// CAS transaction.
///
/// If the future switches to \ref STATE_RESULT as this change is being
/// performed, then this function will either revert the `downstream_count`
/// change or refrain from performing it at all, then return `false`. Otherwise
/// it will return `true` and keep the `downstream_count` change in.
///
/// In both cases, the status word manipulations have acquire ordering:
///
/// - If the increment is successfully performed, this is necessary to ensure
///   that no operation on the future state is reordered before the
///   downstream_count increment, thus maximizing the odds that overly early
///   calls to udipe_finish() and similar can be detected.
/// - If the increment is not performed due to a switch to \ref STATE_RESULT,
///   this is necessary to fully synchronize with the final future state.
///
/// In both cases, `latest_status`, which should initially be set to the latest
/// known future status, will be updated to the new future status.
///
/// It only makes sense to apply this sort of fine-grained transaction to the
/// status word of a live future that's allocated to some work. Unallocated
/// futures can be manipulated by simply overwriting their status with
/// future_status_store().
///
/// This function must be called within the scope of with_logger().
///
/// \param future must be a future that was returned by an asynchronous function
///               (those whose name begins with `udipe_start_`) and has not been
///               liberated by udipe_finish() or udipe_cancel() since.
/// \param latest_status should be initially set to the latest known future
///                      status. It will be updated to the final future status
///                      after all operations have been performed.
///
/// \returns the truth that the downstream_count was incremented. The increment
///          will either not be carried out or be rolled back (depending on
///          which is fastest/most scalable on the host CPU) if the future
///          concurrently switches to \ref STATE_RESULT.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
static inline
bool future_downstream_count_try_inc(udipe_future_t* future,
                                     future_status_t* latest_status) {
    trace("Incrementing downstream_count...");
    future_status_debug_check(*latest_status, true);
    future_status_t pre_op_status = (future_status_word_t){
        // Acquire ordering needed because subsequent operations on the future
        // should not be reordered before this downstream_count increment.
        .as_word = atomic_fetch_add_explicit(&future->status_word,
                                             1,
                                             memory_order_acquire)
    }.as_bitfield;
    future_status_debug_check(pre_op_status, true);
    if (pre_op_status.downstream_count_overflow
        || pre_op_status.downstream_count == MAX_DOWNSTREAM_COUNT)
    {
        errorf("Sorry, the current future implementation does not support "
               "attaching more than %zu waiters to a future",
               (size_t)MAX_DOWNSTREAM_COUNT);
        exit(EXIT_FAILURE);
    } else if (pre_op_status.state == STATE_RESULT) {
        trace("Future concurrently switched to STATE_RESULT, reverting...");
        // This is a rare case where the decrement does not need release
        // ordering because it directly follows the acquire increment, without
        // any other manipulation of the future meanwhile. An acquire barrier is
        // still needed, however, because we do want to synchronize with the
        // final future state if it changes again (which is unlikely).
        *latest_status = future_downstream_count_dec(future,
                                                     memory_order_acquire);
        assert(latest_status->state == STATE_RESULT);
        future_status_debug_check(*latest_status, true);
        return false;
    } {
        trace("Updating latest_status after successful increment...");
        *latest_status = pre_op_status;
        ++(latest_status->downstream_count);
        future_status_debug_check(*latest_status, true);
        return true;
    }
}

/// \}


/// \name Basic future lifecycle
/// \{

/// Liberate a future
///
/// The future will be reset to an unallocated state, then shelved into a
/// thread-local cache where later calls to future_allocate() will be able to
/// find and reuse it instead of resorting to a global allocation.
///
/// This function must be called within the scope of with_logger().
///
/// \param future must point to a future that was previously allocated to some
///               asynchronous operation, and has been liberated via
///               udipe_finish() if it was ever exposed to the user. This future
///               cannot be used again afterwards.
//
// TODO: Add GNU attributes to mark this + future_allocate() as an
//       allocator/liberator pair if possible.
UDIPE_NON_NULL_ARGS
void future_liberate(udipe_future_t* future);

/// Allocate a future
///
/// The future is provided in a partially initialized state:
///
/// - `context` pointer is forwarded from this function's parameter
/// - `status_word` has...
///   * `downstream_count` set to 0
///   * `downstream_count_overflow` cleared
///   * `active` bit cleared (must be set once this future is ready for use)
///   * \ref STATE_UNINITIALIZED (must be set according to the presence/absence
///     of upstream futures, their initial status, etc.)
///   * \ref OUTCOME_UNKNOWN (may need to be set if the outcome is determined
///     right from the start).
///   * `type` set as appropriate to the specified future type.
///   * `notify_address` unset.
///   * `notify_event_or_lazy_lock` unset.
/// - `output` and `specific` are partially configured according to the future
///   type, in such a way that all required system resources are preallocated
///   and relations between these are already set up, but other state which
///   requires access to other future configuration parameters is not set up.
///   * `output.event`, is is allocated and in an unsignaled state.
///   * `output.timer` is allocated but in an unspecified state. It may be set
///     to a particular deadline/period or be unset. You must set it to the
///     desired deadline with no period before use.
///   * `output.epoll_with_event` (Linux-only) is already allocated and attached
///     to the \ref outcome_event_t with identifier `U64_MAX`, and...
///     - ...nothing else yet for \ref TYPE_JOIN. You must attach to it the
///       output fds of upstream futures, identified with their index in \ref
///       concurrent_upstream_t before use.
///     - ...the `upstream_epollfd` for \ref TYPE_UNORDERED, which is
///       preallocated but not yet attached to any file descriptor. See the \ref
///       TYPE_JOIN case described above, except upstream fds must be attached
///       to `upstream_epollfd` not `output.epoll`.
///     - ...the `timerfd` for \ref TYPE_TIMER_REPEAT, which must be configured
///       as in the case of `output.timer` above, but with a period.
///
/// No other type-specific state is initially configured. For example the \ref
/// collective_upstream_t of collective futures is left uninitialized as
/// configuring it requires extra information unknown to this function.
///
/// This function must be called within the scope of with_logger().
///
/// \param context must be a udipe context that was set up with
///                udipe_initialized() and not yet liberated with
///                udipe_finalize(). It must not be liberated until the output
///                future is liberated.
/// \param type indicates the type of the future that is being built. It will
///             be used to allocate associated system resources which are
///             partially type-specific.
///
/// \returns a future that must later be liberated with future_liberate().
//
// TODO: May need to replace the boolean switch of
//       future_status_debug_check() with a 3-states enum to account for the
//       fact that futures will now have three states: unallocated, allocated
//       but not yet fully initialized, and under active use.
// TODO: Implement. Should go through the thread-local cache first, then through
//       the global cache after locking it, and if the global cache is empty too
//       then should allocate a new page of futures, register it into the global
//       cache for liberation by atexit(), release the global cache lock,
//       put the futures in a zeroed/invalid state, and add all but one
//       future to the thread-local cache. The future which we set aside will
//       then be returned by this function.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* future_allocate(udipe_context_t* context,
                                future_type_t type);

// TODO: Ensure that 1/when a user thread exits, its thread-local unallocated
//       future cache is spilled into a global unallocated future cache and 2/on
//       atexit(), this global future cache is fully wiped: not just individual
//       futures, but also the memory pages as part of which these futures were
//       allocated. I think it makes most sense for the global future cache to
//       not be specific to any udipe context but shared across all udipe
//       contexts.

/// \}


/// \name Awaiting future results
/// \{

/// Backend of udipe_wait() that returns the latest future status after the wait
///
/// The wait is considered successful if the final status has \ref STATE_RESULT,
/// which should always be the case when using \ref UDIPE_DURATION_MAX and \ref
/// UDIPE_DURATION_DEFAULT unbounded timeouts.
///
/// The output status is used by operations like udipe_finish() that not only
/// await the final future status, but also process it.
///
/// Must be called within the scope of with_logger().
///
/// \param future must be a future that was returned by an asynchronous function
///               (those whose name begins with `udipe_start_`) and has not been
///               liberated by udipe_finish() or udipe_cancel() since.
/// \param timeout works as in wait_on_address(). In particular it cannot take
///                value \ref UDIPE_DURATION_DEFAULT, which should have been
///                translated into \ref UDIPE_DURATION_MAX higher up the stack.
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
/// Must be called within the scope of with_logger().
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
/// Must be called within the scope of with_logger().
///
/// \param future must be a future that was returned by udipe_start_join() and
///               has not been liberated by udipe_finish() or udipe_cancel()
///               since.
/// \param latest_status should be the latest known future status at the time
///                      where this function is called.
/// \param timeout works as in wait_on_address(). In particular it cannot take
///                value \ref UDIPE_DURATION_DEFAULT, which should have been
///                translated into \ref UDIPE_DURATION_MAX higher up the stack.
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
/// Must be called within the scope of with_logger().
///
/// \param future must be a future that was returned by udipe_start_unordered()
///               and has not been liberated by udipe_finish() or udipe_cancel()
///               since.
/// \param latest_status should be the latest known future status at the time
///                      where this function is called.
/// \param timeout works as in wait_on_address(). In particular it cannot take
///                value \ref UDIPE_DURATION_DEFAULT, which should have been
///                translated into \ref UDIPE_DURATION_MAX higher up the stack.
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
/// Must be called within the scope of with_logger().
///
/// \param future must be a future that was returned by udipe_start_timer_once()
///               and has not been liberated by udipe_finish() or udipe_cancel()
///               since.
/// \param latest_status should be the latest known future status at the time
///                      where this function is called.
/// \param timeout works as in wait_on_address(). In particular it cannot take
///                value \ref UDIPE_DURATION_DEFAULT, which should have been
///                translated into \ref UDIPE_DURATION_MAX higher up the stack.
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
/// Must be called within the scope of with_logger().
///
/// \param future must be a future that was returned by
///               udipe_start_timer_repeat() and has not been liberated by
///               udipe_finish() or udipe_cancel() since.
/// \param latest_status should be the latest known future status at the time
///                      where this function is called.
/// \param timeout works as in wait_on_address(). In particular it cannot take
///                value \ref UDIPE_DURATION_DEFAULT, which should have been
///                translated into \ref UDIPE_DURATION_MAX higher up the stack.
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
    /// the waiting procedure involves awaiting an epollfd. Because the design
    /// of epoll_wait() makes it hard to use from multiple threads, this waiting
    /// method is implemented by having one thread probe the epollfd while other
    /// threads wait for it to report its conclusion via a futex. The selection
    /// of the thread that will call epoll_wait() is performed via simple
    /// locking of a flag in the future status word.
    ///
    /// One limitation of this design, however is that something like the
    /// following can happen:
    ///
    /// - Thread A starts waiting for future F with a timeout of 1s
    /// - Thread A locks state and starts waiting via epoll_wait().
    /// - Thread B starts waiting for future F with a timeout of 2s.
    /// - Thread B observes that thread A got there first and starts waiting for
    ///   thread A via the futex method.
    /// - Thread A reaches its 1s timeout without getting a notification from
    ///   epoll_wait(), so it stops waiting and reports the timeout.
    /// - At this point, thread B still has 1s of timeout to go, but it cannot
    ///   passively wait for thread A via the futex anymore, instead it must
    ///   switch to active waiting via epoll_wait().
    ///
    /// This situation is handled by unblocking **one** of the threads that's
    /// waiting on the futex (to avoid thundering herds), which will retult in
    /// is future_wait_by_address() call returning this outcome. Upon receiving
    /// this outcome, the thread must either lock the future status and start an
    /// epoll_wait() or wake another futex waiter if it cannot call epoll_wait()
    /// because its timeout elapsed at the same time.
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
/// This function must be called within the scope of with_logger().
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

/// \}


#ifdef UDIPE_BUILD_TESTS
    /// Unit tests
    ///
    /// This function runs all the unit tests for this module. It must be called
    /// within the scope of with_logger().
    void future_unit_tests();
#endif
