#pragma once

//! \file
//! \brief Implementation of \ref udipe_future_t
//!
//! TODO: Outline new implementation.

#include <udipe/future.h>

#include <udipe/context.h>
#include <udipe/nodiscard.h>
#include <udipe/result.h>
#include <udipe/time.h>

#include "address_wait.h"
#include "arch.h"
#include "error.h"
#include "log.h"

#include <assert.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>


/// \name Future data structure
/// \{

/// Future state machine
///
/// This is the core state machine of \ref udipe_future_t, which can be used to
/// follow each step of the basic lifecycle of a future.
///
/// It is encoded into selected bits of the \ref future_status_word_t so that
/// 1/it can be atomically checked and modified along with other aspects of the
/// future state and 2/changes to it can be awaited using address_wait() for
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
    /// dependency exits this state. But join futures instead remain in this
    /// state until either 1/**no** dependency is in \ref STATE_WAITING or \ref
    /// STATE_PROCESSING state anymore or 2/at least one dependency has a moved
    /// to \ref STATE_RESULT with a failure outcome.
    STATE_WAITING,

    /// Work-in-progress state
    ///
    /// Futures that await something other than a dependency (such a network
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
    /// the result payload specific to this future type.
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
/// dealing with and in which state it is. However, since it is constant
/// metadata that does not change, it never needs to be atomically modified
/// along with other status information, and therefore does not _need_ to be
/// stored into the future status word. It is therefore fine to extract this
/// enum from the status word to another field if either that identifier grows
/// too large or too many extra status bits end up being needed for other
/// purposes.
//
// TODO: Decide if the documentation currently written here is here to stay.
//       Maybe I should just nuke it once the associated code is written to
//       avoid having the same info in two places?
// TODO: Sync up with internal table if here to stay.
typedef enum future_type_e /* : _BitInt(4) */ {
    /// Invalid future type
    ///
    /// This placeholder type is only set on unallocated futures and should
    /// never be observed on a properly initialized future.
    TYPE_INVALID = 0,

    /// First network operation type
    ///
    /// All network operations have a type code between \ref TYPE_NETWORK_START
    /// inclusive and \ref TYPE_NETWORK_END exclusive, and share the following
    /// properties:
    ///
    /// - Single optional dependency.
    ///     - If it already has \ref OUTCOME_SUCCESS at command scheduling time,
    ///       command is scheduled as if no dependency was specified.
    ///     - If already has a failing outcome at command scheduling time,
    ///       future is created in the failure state without bothering the
    ///       network thread with this operation.
    ///     - If outcome isn't initially known, command is scheduled in a halted
    ///       state and network thread awaits the dependency via its fd before
    ///       command execution can start or be canceled.
    /// - Driven to completion by a udipe network thread processing a previously
    ///   submitted network request.
    /// - Produces a result type that depends on the network operation.
    /// - Supports futex-based status change notifications which must be enabled
    ///   via the `notify_address` flag of \ref future_status_word_t.
    /// - Output file descriptor is an eventfd whose notification must be
    ///   enabled via the `notify_fd` flag of \ref future_status_word_t.
    /// - Cancelation handled by switching to \ref STATE_CANCELING with \ref
    ///   OUTCOME_FAILURE_CANCELED, signaling this to the network thread via the
    ///   output eventfd which is monitored by the network thread along with
    ///   usual network sockets (by exception to usual rules this is done even
    ///   if `notify_fd` is not enabled), and waiting for the network thread to
    ///   acknowledge this signal by switching the future to \ref STATE_RESULT.
    /// - Liberation drains the eventfd if known to be signaled (can also be
    ///   done via `write(1)/read()` cycle if status unknown) then recycles it
    ///   along with the future.
    TYPE_NETWORK_START,

    /// Connection setup request from udipe_start_connect()
    ///
    /// See \ref TYPE_NETWORK_START for general info about network operations.
    TYPE_NETWORK_CONNECT = TYPE_NETWORK_START,

    /// Connection teardown request from udipe_start_disconnect()
    ///
    /// See \ref TYPE_NETWORK_START for general info about network operations.
    TYPE_NETWORK_DISCONNECT,

    /// Datagram send request from udipe_start_send()
    ///
    /// See \ref TYPE_NETWORK_START for general info about network operations.
    TYPE_NETWORK_SEND,

    /// Datagram receive request from udipe_start_recv()
    ///
    /// See \ref TYPE_NETWORK_START for general info about network operations.
    TYPE_NETWORK_RECV,

    /// First future type past the end of the list of network operations
    ///
    /// See \ref TYPE_NETWORK_START for general info about network operations.
    TYPE_NETWORK_END,

    /// Custom operation
    ///
    /// - No dependencies.
    /// - Driven to completion by a user thread submitting a result via
    ///   udipe_set_custom().
    /// - Produces a user-defined result type that is somehow fit into a small
    ///   udipe-provided bag of bytes.
    /// - Supports futex-based status change notifications which must be enabled
    ///   via the `notify_address` flag of \ref future_status_word_t.
    /// - Output file descriptor is an eventfd whose notifications must be
    ///   enabled via the `notify_fd` flag of \ref future_status_word_t.
    /// - Cancelation handled by switching to \ref STATE_RESULT with \ref
    ///   OUTCOME_FAILURE_CANCELED and providing the user thread that handles
    ///   the custom operation with a way to periodically check if its work has
    ///   been canceled.
    /// - Liberation drains the eventfd as described for \ref
    ///   TYPE_NETWORK_START.
    TYPE_CUSTOM = TYPE_NETWORK_END,

    /// Join (aka await all)
    ///
    /// - Multiple dependencies collected into an epollfd which serves double
    ///   duty as its output file descriptor, along with an eventfd used for
    ///   status word change notifications. Epoll identifier of a dependency is
    ///   its index in the upstream dependency array, except for the outcome
    ///   availability eventfd which has an easily identifiable special index
    ///   (`SIZE_MAX` ?).
    /// - Driven to completion by a thread that polls its epollfd, either
    ///   directly (if this future is passed to udipe_wait()/udipe_finish()) or
    ///   indirectly by virtue of being awaited by fd as a dependency of another
    ///   future. Contains a counter of dependencies that have not yet reached
    ///   \ref OUTCOME_SUCCESS, whenever a dependency reaches this outcome the
    ///   counter is decremented and the associated fd is removed from the
    ///   epollfd's interest list. Join future successful completion happens
    ///   once the pending dep counter reaches 0. When this happens, status word
    ///   moves to \ref STATE_RESULT with \ref OUTCOME_SUCCESS, and if
    ///   `downstream_count` indicates the presence of any waiters other than
    ///   the active one or the caller is not the final one (i.e. wait() not
    ///   finish() was used), outcome availability eventfd is signaled, marking
    ///   the output epollfd as permanently ready and thus ensuring that all
    ///   downstream clients will eventually take notice of the status change.
    /// - Does not produce a result.
    /// - Does not support futex-based status change notifications in general,
    ///   but uses them to synchronize concurrent access to output epollfd from
    ///   multiple threads. Synchronous wait performed via epoll_wait() under
    ///   locking protection.
    /// - Output file descriptor is an aforementioned epollfd that automatically
    ///   forwards input readiness notifications. Client threads that receive
    ///   this readiness notification proceed to lock the epollfd then read
    ///   notifications via epoll_wait() and update status word, internal
    ///   counter and epollfd interest list accordingly.
    /// - Cancelation handled by switching to \ref STATE_RESULT with \ref
    ///   OUTCOME_FAILURE_CANCELED then signaling clients with the outcome
    ///   availability eventfd if `downstream_count` + state indicates that at
    ///   least one downstream future monitors the epollfd.
    /// - Liberation drains the cancelation eventfd as described for \ref
    ///   TYPE_NETWORK_START, if canceled unregisters fds of all dependencies
    ///   via `epoll_ctl()` while ignoring errors (not needed in successful case
    ///   since all dependencies should already be unregistrered), then recycles
    ///   coupled (epoll, eventfd) pair and future.
    TYPE_JOIN,

    /// Unordered (aka await any)
    ///
    /// Implementation has several similarities to join, but with some obvious
    /// and less obvious differences:
    ///
    /// - Not based on a single epollfd that combines dependencies and an
    ///   outcome availability eventfd, instead two cascading epollfds are used:
    ///     - An "inner" epollfd that is attached to the fds of the upstream
    ///       futures that we depend on.
    ///     - An "outer" epollfd that is attached to the inner epollfd and to
    ///       the outcome availability eventfd.
    /// - epoll_wait() is only ever used to query one single input event, this
    ///   way we are never notified about more futures than we can report.
    /// - While remaining dependencies are still tracked, successful completion
    ///   is signaled when the **first** dependency reaches \ref
    ///   OUTCOME_SUCCESS.
    /// - Produces a result composed of the index of the input future that
    ///   completed, and another unordered future with remaining dependency
    ///   count decremented by 1 if the remaining dependency count of this
    ///   future indicates that there should be another future afterwards. This
    ///   other unordered future uses a different outer epollfd/eventfd pair,
    ///   but attaches it to the same inner epollfd that contains our remaining
    ///   dependencies, which is simultaneously unregistered from the outer
    ///   epollfd of the parent future. All this epollfd shuffling is obviously
    ///   done under lock protection, to ensure that from the perspective of
    ///   other threads also waiting for the initial unordered future, we switch
    ///   to a final state where the outer epollfd is only connected to the
    ///   outcome availability eventfd, which is signaled, ensuring that these
    ///   other threads can only get notifications about the eventfd.
    TYPE_UNORDERED,

    /// Single-shot/deadline timer
    ///
    /// - No dependency.
    /// - Driven to completion by the kernel signaling the underlying timerfd.
    /// - Does not produce a result.
    /// - Does not support futex-based status change notifications in general,
    ///   but uses them to synchronize concurrent access to output timerfd from
    ///   multiple threads. Synchronous wait is performed via read() under
    ///   locking protection.
    /// - Output file descriptor is an timerfd that automatically becomes ready
    ///   once specified deadline is reached. Client threads that receive this
    ///   readiness notification proceed to lock the epollfd then move the
    ///   future to \ref STATE_RESULT with \ref OUTCOME_SUCCESS.
    /// - Cancelation handled by switching to \ref STATE_RESULT with \ref
    ///   OUTCOME_FAILURE_CANCELED then setting the timerfd to a tiny relative
    ///   period so it gets instantly signaled.
    /// - Liberation disarms the timerfd, then recycles it and the future.
    TYPE_TIMER_ONCE,

    /// Multi-shot/repeating timer
    ///
    /// Much like unordered futures handle multi-shot signaling by using a
    /// cascading epoll structure that makes it possible to migrate the
    /// dependency eventfd to a different future, multi-shot timers also use
    /// epollfds as a way to decouple the output fd from the inner timerfd so
    /// that the latter can be reused by another future without disturbing the
    /// former.
    ///
    /// - Output fd is an epollfd that is connected to the timerfd of interest
    ///   and an eventfd that signals outcome availability.
    /// - Each timer shot leads the timerfd to be "unplugged" from the epollfd
    ///   and migrated to the next future emitted in the result, while on its
    ///   side the eventfd is signaled to make sure that the original future
    ///   remains in a perma-signaled state.
    TYPE_TIMER_REPEAT,

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
    /// This reference count is initialized to 1 at the time where a future
    /// is created. It is incremented when...
    ///
    /// - This future is registered as a dependency to another future,
    ///   either via an `after` option or the array parameter to a
    ///   collective operation.
    /// - A thread enters a udipe_wait() for this future.
    ///
    /// ...and it is decremented when it is guaranteed that the
    /// aforementioned waiter will not be accessing this future anymore. For
    /// example when...
    ///
    /// - A thread exists a udipe_wait() for this future.
    /// - A network operation scheduled after this future has read and
    ///   processed its final status, and either was canceled or started
    ///   executing as a result.
    /// - A joined future that awaited this future is liberated via
    ///   udipe_finish() or canceled via udipe_cancel().
    /// - The last future in an unordered chain is liberated via
    ///   udipe_finish() or any future in the chain is canceled via
    ///   udipe_cancel().
    /// - udipe_finish() is done reading out this future's final state and
    ///   will not touch its internal state again.
    ///
    /// The decrement that happens when this future is passed to
    /// udipe_finish() is expected to take its `downstream_count` from 1 to
    /// 0, and thus trigger the immediate liberation of the future. Any
    /// nonzero remainder indicates that some kind of use-after-free or
    /// reference counting bug is going on, and will therefore lead the
    /// application to terminate with a fatal error log.
    ///
    /// Futures that only support fd-based signaling can additionally use
    /// this counter in tandem with `state` to know if other futures or
    /// threads are awaiting their output fd besides the worker that is
    /// currently operating them. This can enabling eventfd signal elision
    /// optimizations when that is not the case (TODO decide if this
    /// optimization is worthwhile).
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

    /// Truth that changes to this status word should be notified through a
    /// call to wake_by_address_all()
    ///
    /// This flag is initially unset when a future is set up. It is set on
    /// the first time where a thread starts waiting for state changes via
    /// wait_on_address(), and cannot be unset afterwards until the future
    /// is liberated. From the point where this flag is set, all status word
    /// changes will be notified via wake_by_address_all().
    ///
    /// The reason why this is a sticky flag and not a counter of waiters is
    /// that we don't have enough bits in this status word to afford more
    /// than one counter of reasonable range... So we can afford to avoid
    /// futex syscalls on futures that never need it, but not on futures
    /// that intermittently need it.
    bool notify_address : 1;

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

    union {
        /// State lock for lazily updated futures
        ///
        /// "Lazy" future types are not eagerly updated by a thread which is
        /// in charge of performing the asynchronous work. Instead they get
        /// lazily updated, usually at the point where a user thread starts
        /// directly or indirectly waiting for their output epollfd to
        /// signal a status change.
        ///
        /// Because these future types may be concurrently awaited by
        /// multiple threads, access to their lazily updated internal state
        /// must be synchronized somehow. For collective and repeating timer
        /// futures, which have complex internal state that cannot be
        /// updated in a single atomic RMW operation, this is ensured by
        /// using this flag as a lock. When such a future's output fd
        /// becomes ready, indicating a possible state change, the thread
        /// that gets awakened as a result must...
        ///
        /// - Check if this locking flag is already set.
        ///     - If so, another thread is already in the process of
        ///       querying the file descriptor, and this thread can do
        ///       nothing but wait for the results. To do this set the
        ///       `notify_address` flag if it is not set yet, then use a
        ///       wait_on_address() loop to wait for the other thread that
        ///       arrived first to report the final state (or release the
        ///       lock in some other way).
        ///     - If not, attempt to set this flag, and if successful
        ///       perform any required state update operation finishing with
        ///       the status word (clearing this lock flag along the way),
        ///       then signal the status word change via
        ///       wake_by_address_all() if `notify_address` is set.
        bool lazy_update_lock : 1;

        /// Truth that changes to this status word should be signaled via
        /// the output `eventfd`
        ///
        /// "Eager" futures support address-based signaling, in contrast to
        /// "lazy" futures which only support file descriptor signaling.
        /// Therefore eager futures do not always need to signal changes
        /// through their output eventfd, and require a flag to enable this
        /// form of signaling.
        ///
        /// This flag works just like `notify_address`: initially unset, set
        /// the first time a thread expresses interest in receiving updates
        /// through the file descriptor path, cannot be unset afterwards
        /// until the future is destroyed.
        bool notify_fd : 1;
    };

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
    /// set to a completed state, as the user is allowed to liberate the
    /// associated memory after this point.
    ///
    /// Must have been checked to contain no duplicates and no `NULL`s at
    /// collective future construction time. As long as we do not expose an
    /// output fd accessor that lets users call `dup()`, `epoll_ctl()` should
    /// take care of the former at collective future construction time.
    udipe_future_t* const* array;

    // TODO: Consider adding a len field in debug builds, used
    //       for bound-checking accesses to `array`.
} collective_upstream_t;

/// eventfd used to mark a lazy future as permanently ready once it has reached
/// its final outcome, after its final status has been set
///
/// Most lazy futures (those whose status is set by the client thread that
/// awaits them, as opposed to eager future which are processed by a background
/// thread) must have an `output_fd` which is an epollfd. One limitation of
/// epollfds as an output file descriptor is that they stop being ready once its
/// readiness notifications have been processed, thus required an additional
/// eventfd in the epollfd interest list to ensure continued readiness after the
/// final status has becomes available.
///
/// The exception is \ref TYPE_TIMER_ONCE, which is simple enough to avoid the
/// need for an output epollfd, and can instead expose its internal timerfd
/// directly as its output file descriptor. This timerfd does not need to be
/// read by clients and can thus remain in the perpetually ready unread state,
/// which acts as that future type's output readiness notification.
///
/// These eventfds must be written to under `lazy_update_lock` protection, and
/// must be reset and recycled along with the associated output epollfd at the
/// time where the associated future is liberated.
//
// TODO: Windows version, based on NT semaphores?
typedef int outcome_eventfd_t;

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
        } eager_payload;

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

            /// Number of upstream futures that have not yet reached \ref
            /// OUTCOME_SUCCESS
            ///
            /// If this number reaches 0, then this future can switch to \ref
            /// STATE_RESULT with \ref OUTCOME_SUCCESS.
            ///
            /// Must be read and written under `lazy_update_lock` protection.
            size_t remaining;

            /// eventfd used to mark this future as permanently ready after it
            /// has reached its final outcome
            ///
            /// See \ref outcome_eventfd_t for more information.
            outcome_eventfd_t outcome_eventfd;
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
            /// Must be written under `lazy_update_lock` protection.
            udipe_unordered_payload_t payload;

            /// Inner epollfd that contains the set of upstream futures, which
            /// is awaited by the output epollfd
            ///
            /// This epollfd indirection is used to easily migrate the waiting
            /// set to successor unordered futures without affecting clients of
            /// this specific unordered future.
            ///
            /// Must be awaited under `lazy_update_lock` protection, and
            /// detached from the output epollfd and attached to the successor
            /// future (if any) once a result is ready. Must be reset by
            /// detaching all remaining upstream fds then recycled when the
            /// future is liberated.
            //
            // TODO: Windows version, based on NT service threads?
            int upstream_epollfd;

            /// eventfd used to mark this future as permanently ready after it
            /// has reached its final outcome
            ///
            /// See \ref outcome_eventfd_t for more information.
            outcome_eventfd_t outcome_eventfd;
        } unordered;

        /// Repeating timer state
        ///
        /// This union variant corresponds to \ref TYPE_TIMER_REPEAT. It tracks
        /// the state needed to report how many timer ticks were need and how to
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
            /// Must be read under `lazy_update_lock` protection, and detached
            /// from the output epollfd then attached to the successor future's
            /// output epollfd when a result is ready. Must be destroyed when
            /// the future is liberated, for now. May switch to disarming and
            /// recycling if timerfd creation/destruction ever becomes a
            /// bottleneck, but that seems unlikely under correct usage since
            /// recuring timerfds largely void the need for repeatedly creating
            /// and destroying lots of one-shot timerfds.
            //
            // TODO: Windows version, based on NT timer threads?
            int timerfd;

            /// eventfd used to mark this future as permanently ready after it
            /// has reached its final outcome
            ///
            /// See \ref outcome_eventfd_t for more information.
            outcome_eventfd_t outcome_eventfd;
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
    // TODO: Windows version, based on NT semaphores?
    union {
        /// eventfd in non-semaphore mode, used for eager futures
        ///
        /// This output file descriptor type is used for "eager" future types
        /// where the asynchronous operation is processed by a dedicated thread,
        /// namely network and custom operation futures.
        ///
        /// Because these futures can also signal completion via a futex or even
        /// via a mere atomic RMW operation when no one is waiting for
        /// completion yet, eventfd signaling is optional for these future types
        /// and must be explicitly requested by setting the `notify_fd` bit of
        /// `status_word` before registering interest in this eventfd.
        ///
        /// When the task's outcome has been filed into the status word, if
        /// `notify_fd` is set, the value `1` will be written into this eventfd,
        /// which will mark it as ready for all threads that are monitoring it.
        /// These threads will then proceed to read the outcome in the status
        /// word, completing the synchronization transaction.
        ///
        /// Must be reset via readout and recycled when the future is liberated.
        int event;

        /// epollfd with a cancelation eventfd, used for collective futures and
        /// repeating timers.
        ///
        /// This output file descriptor type is mainly used for "collective"
        /// future types that await several other futures. It is, as the name
        /// and previous description suggest, an epollfd that monitors the
        /// readiness of the output file descriptors of all upstream futures,
        /// along with an additional eventfd which indicates availability of
        /// this future's (successful or failing) outcome.
        ///
        /// How exactly dependencies are awaited depends on the kind of
        /// future that you are dealing with:
        ///
        /// - Join futures use a single epollfd that encompasses all fds of
        ///   interest. Dependency fds use their index in the array of
        ///   dependencies as epoll metadata, while the outcome availability
        ///   eventfd uses an easily identifiable invalid index.
        /// - Unordered futures use a cascaded pair of epollfds. Their
        ///   dependencies are attached to an "inner" epollfd with index-based
        ///   signaling as before, but this "inner" epollfd is in turn attached
        ///   to an "outer" epollfd which is additionally attached to the
        ///   outcome availability eventfd. This curious cascading epollfd
        ///   structure makes it easy to migrate the inner epollfd from an
        ///   unordered future to the next future in the unordered chain, while
        ///   leaving the original epollfd attached only to a signaled eventfd
        ///   which is left in a perpetually signaled state to advertise that
        ///   the outcome is now available.
        /// - Repeating timers handle multiple output futures using the same
        ///   trick, except instead of cascading epollfds they instead simply
        ///   have an output epollfd which is connected to a timerfd (that
        ///   performs time-based signaling and can be detached and migrated to
        ///   the next future in the chain) and an eventfd (that eventually
        ///   remains attached to the epollfd in a perpetually signaled state to
        ///   broadcast the information that the final outcome is available).
        ///
        /// Because epoll's API design is not very friendly to multi-threaded
        /// use, `epoll_wait()` on the inner epollfds requires
        /// `lazy_update_lock` protection, which effectively acts as a mutex to
        /// control access to the epollfd and associated future state.
        ///
        /// Whenever `epoll_wait()` output indicates that a particular upstream
        /// future has underwent a status change or this future has been
        /// canceled, the status word of the upstream future (if any) must be
        /// checked, then the fields of this collective future must be modified
        /// accordingly, and finally any other thread which registered to be
        /// notified of state changes while we were probing `epoll_wait()` must
        /// be notified via wake_by_address_all(). Once the future outcome is
        /// known, whether successful or unsuccessful, its availability must be
        /// signaled via the dedicated eventfd if at least one other future
        /// awaited this future, as indicated by `downstream_count`.
        ///
        /// Must be reset by detaching all remaining fds except for the outcome
        /// signaling eventfd, then recycled alongside said eventfd when the
        /// future is liberated.
        int epoll;

        /// timerfd, used for single-shot timer futures
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
        /// becomes a bottleneck, but that seems unlikely under correct usage
        /// since recuring timerfds are a thing.
        int timer;
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

/// Initialize a future's status word
///
/// This operation is not atomic and must never be called from multiple threads.
/// It is only meant to be used when a future is initially allocated, and must
/// never be called at a time where other threads might be accessing the future.
///
/// At any point of a future's lifetime after initialization, including when the
/// future is recycled into a thread-local cache and later recalled from said
/// cache, future_status_store() should be preferred to this function.
UDIPE_NON_NULL_ARGS
static inline
void future_status_initialize(udipe_future_t* future,
                              future_status_t status) {
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
    return result;
}

/// Wait for a future's status word to change away from a certain value, return
/// truth that it may have changed (otherwise the wakeup was spurious)
///
/// This function has the same semantics as wait_on_address(), but it is meant
/// to be called and operates on their status word in its decoded bitfield form.
///
/// It must be called within the scope of with_logger().
UDIPE_NON_NULL_ARGS
static inline
bool future_status_wait(udipe_future_t* future,
                        future_status_t expected,
                        udipe_duration_ns_t timeout) {
    return wait_on_address(
        &future->status_word,
        (future_status_word_t){ .as_bitfield = expected }.as_word,
        timeout
    );

}

/// Atomically increment a future's downstream count, returning its **new**
/// status
///
/// This should be done whenever a future is attached to a new downstream
/// entity, such as a join future, and no other change to the future status word
/// are required. If other changes are required, the downstream count increment
/// should be batched into a broaded `future_status_compare_exchange_`
/// operation.
///
/// But when doing so, you should bear in mind that this operation is faillible.
/// If the former status turned out to have a downstream count of \ref
/// MAX_DOWNSTREAM_COUNT, then the operation should be rolled back with
/// future_downstream_count_dec() and error out.
///
/// When this operation is successful, it returns the **final** status word
/// after the downstream count increment. This is to be contrasted with most
/// atomic operations, which return the **initial** status word. This unusual
/// convention was chosen because the initial status word is usually not useful
/// to callers of this function, and since this function is inline the
/// computation of the final status word can be optimized out when it is not
/// used.
///
/// This function must be called within the scope of with_logger().
UDIPE_NON_NULL_ARGS
static inline
future_status_t future_downstream_count_inc(udipe_future_t* future) {
    trace("Beginning downstream_count increment...");
    const future_status_t initial_status = (future_status_word_t){
        .as_word = atomic_fetch_add_explicit(&future->status_word,
                                             1,
                                             // No reordering before this increment
                                             memory_order_acquire)
    }.as_bitfield;
    if (initial_status.downstream_count == MAX_DOWNSTREAM_COUNT
        || initial_status.downstream_count_overflow)
    {
        errorf("Sorry, the current future implementation does not support "
               "attaching more than %zd waiters to a future",
               (size_t)MAX_DOWNSTREAM_COUNT);
        exit(EXIT_FAILURE);
    } else {
        trace("...which succeeded, let's compute the final status accordingly.");
        future_status_t final_status = initial_status;
        ++final_status.downstream_count;
        return final_status;
    }
}

/// Atomically decrement a future's downstream count
///
/// This should be done whenever a downstream entity, such as a join future, is
/// done inspecting the state of this future and will never touch it again.
///
/// As with future_downstream_count_inc(), if you need to modify other bits of
/// the future status word, you should batch these two updates into a single
/// `future_status_compare_exchange_` transaction.
UDIPE_NON_NULL_ARGS
static inline
void future_downstream_count_dec(udipe_future_t* future) {
    future_status_t result = (future_status_word_t){
        .as_word = atomic_fetch_sub_explicit(&future->status_word,
                                             1,
                                             // No reordering after this decrement
                                             memory_order_release)
    }.as_bitfield;
    assert(result.downstream_count >= 1);
}

/// \}


/// \name Type-specific branches of the future_wait() function
/// \{

/// Backend of udipe_wait() for all future types that get eagerly signaled by a
/// separate thread
///
/// Must be called within the scope of with_logger().
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
bool future_wait_eager(udipe_future_t* future,
                       future_status_t latest_status,
                       udipe_duration_ns_t timeout);

// TODO future_wait_join()
// TODO future_wait_unordered()

/// Backend of udipe_wait() for \ref TYPE_TIMER_ONCE
///
/// Must be called within the scope of with_logger().
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
bool future_wait_timer_once(udipe_future_t* future,
                            future_status_t latest_status,
                            udipe_duration_ns_t timeout);

// TODO future_wait_timer_repeat()

/// \}


#ifdef UDIPE_BUILD_TESTS
    /// Unit tests
    ///
    /// This function runs all the unit tests for this module. It must be called
    /// within the scope of with_logger().
    void future_unit_tests();
#endif
