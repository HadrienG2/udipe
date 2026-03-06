#pragma once

//! \file
//! \brief Implementation of \ref udipe_future_t
//!
//! Under the hood, futures are implemented using a type that is isomorphic to
//! \ref udipe_result_t, but uses a futex instead of a dumb enum value. This
//! futex leverages the existence of the \ref UDIPE_COMMAND_PENDING sentinel
//! value of the \ref udipe_command_id_t result tag in order to let threads
//! efficiently wait for the result to come up.

#include <udipe/future.h>
#include <udipe/result.h>

#include "arch.h"

#include <assert.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>


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

    /// Liberation-in-progress state
    ///
    /// A future reaches this state after udipe_finish() has been called, if its
    /// downstream_count is not zero at that time. This means that other futures
    /// still have this future registered as a dependency and have not looked up
    /// its final status yet, and will therefore come back to check its status.
    /// In this case, liberation is deferred until all downstream futures are
    /// done observing this future's final state and guaranteed to never access
    /// this future again.
    ///
    /// Any user attempt to use a future that is in \ref STATE_LIBERATING in any
    /// manner is a use-after-free error that should trigger a program abort in
    /// debug builds.
    STATE_LIBERATING,

    // NOTE: If this enum gets more than 8 variants, reallocate the bit budget
    //       of the `state` field of the future_status_word_t accordingly.
} future_state_t;

/// Future execution outcome
///
/// After a future enters \ref STATE_CANCELING, \ref STATE_RESULT or \ref
/// STATE_LIBERATING, this enum tracks whether the associated asynchronous
/// operation completed successfully or errored out in some fashion.
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
    /// STATE_CANCELING, \ref STATE_RESULT or \ref STATE_LIBERATING where the
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
typedef enum future_type_e /* : _BitInt(3) */ {
    /// Invalid future type
    ///
    /// This placeholder type is only set on unallocated futures and should
    /// never be observed on a properly initialized future.
    TYPE_INVALID = 0,

    /// Network operation (send, recv)
    ///
    /// - Single dependency awaited by the network thread via its output file
    ///   descriptor.
    /// - Driven to completion by a udipe network thread processing a previously
    ///   submitted network request.
    /// - Produces a result type that depends on the network operation.
    /// - Supports futex-based status change notifications which must be enabled
    ///   via the `notify_address` flag of \ref future_status_word_t.
    /// - Output file descriptor is an eventfd whose notifications must be
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
    //
    // TODO: Figure out if we need finer-grained operation types, e.g. SEND
    //       and RECV. Probably not, the future type doesn't need to know and
    //       the network thread doing the work and setting the result can be
    //       told the operation type via another channel than the future.
    // TODO: Figure out if connect and disconnect truly need to be asynchronous.
    //       Probably not. Just make the client thread set up the socket and
    //       send it to the appropriate network thread.
    TYPE_NETWORK,

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
    /// - Liberation drains the eventfd as described for \ref TYPE_NETWORK.
    TYPE_CUSTOM,

    /// Join (wait-for-all)
    ///
    /// - Multiple dependencies collected into an epollfd which serves double
    ///   duty as its output file descriptor, along with an eventfd used for
    ///   cancelation. Epoll identifier of a dependency is its index in the
    ///   upstream dependency array, except for the cancelation eventfd which
    ///   has an easily identifiable special index (`SIZE_MAX` ?).
    /// - Driven to completion by a thread that polls its epollfd, either
    ///   directly (if this future is passed to udipe_wait()/udipe_finish()) or
    ///   indirectly by virtue of being awaited by fd as a dependency of another
    ///   future. Contains a counter of dependencies that have not yet reached
    ///   \ref OUTCOME_SUCCESS, successful completion happens once this counter
    ///   reaches 0.
    /// - Does not produce a result.
    /// - Does not support futex-based status change notifications in general,
    ///   but uses them to synchronize concurrent access to output epollfd from
    ///   multiple threads.
    /// - Output file descriptor is an aforementioned epollfd that automatically
    ///   forwards input readiness notifications. Client threads that receive
    ///   this readiness notification proceed to lock the epollfd then read
    ///   notifications via epoll_wait() and update status word, internal
    ///   counter and epollfd interest list accordingly.
    /// - Cancelation handled by switching to \ref STATE_RESULT with \ref
    ///   OUTCOME_FAILURE_CANCELED then signaling clients with the eventfd that
    ///   was registered into the output epollfd for this very purpose.
    /// - Liberation drains the cancelation eventfd as described for \ref
    ///   TYPE_NETWORK, if canceled unregisters fds of all dependencies via
    ///   `epoll_ctl()` while ignoring errors (not needed in successful case
    ///   since all dependencies should already be unregistrered), then recycles
    ///   coupled (epoll, eventfd) pair and future.
    TYPE_JOIN,

    /// Unordered (wait-for-any)
    ///
    /// Works a lot like join, but with some important differences:
    ///
    /// - No remaining dependency counter, instead successful completion is
    ///   signaled when the **first** dependency reaches \ref OUTCOME_SUCCESS.
    /// - Produces a result composed of the index of the input future that
    ///   completed and a future that reuses the same epollfd but with the
    ///   successful dependency removed (TODO figure out how to make that safe
    ///   in the presence of other threads potentially listening to the epollfd,
    ///   if that's not possible just rebuild the epollfd in the presence of
    ///   any other listener signaled by `downstream_count`).
    TYPE_UNORDERED,

    /// Single-shot timer
    ///
    /// - No dependency.
    /// - Driven to completion by the kernel signaling the underlying timerfd.
    /// - Does not produce a result.
    /// - Does not support futex-based status change notifications in general,
    ///   but uses them to synchronize concurrent access to output timerfd from
    ///   multiple threads.
    /// - Output file descriptor is an timerfd that automatically becomes ready
    ///   once specified deadline is reached. Client threads that receive this
    ///   readiness notification proceed to lock the epollfd then move the
    ///   future to \ref STATE_RESULT with \ref OUTCOME_SUCCESS.
    /// - Cancelation handled by switching to \ref STATE_RESULT with \ref
    ///   OUTCOME_FAILURE_CANCELED then setting the timerfd to a tiny relative
    ///   period so it gets instantly signaled.
    /// - Liberation disarms the timerfd, then recycles it and the future.
    TYPE_TIMER_ONCE,

    /// Multi-shot timer
    ///
    /// Works a lot like single-shot, but with some important differences:
    ///
    /// - Produces a result composed of the number of missed timer ticks and
    ///   another future that can be used to keep awaiting this timerfd. (TODO
    ///   figure out how to make that safe in the presence of other threads
    ///   potentially listening to the timerfd, if that's not possible just
    ///   rebuild the timerfd in the presence of any other listener signaled by
    ///   `downstream_count`).
    TYPE_TIMER_REPEAT,

    // NOTE: If this enum gets more than 8 variants, reallocate the bit budget
    //       of the `type` field of the future_status_word_t accordingly.
} future_type_t;

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
    /// Fine-grained interpretation of a future status word
    ///
    /// Used for any kind of logical status word readout or manipulation.
    struct {
        /// Number of threads or downstream futures that have expressed interest
        /// in this future's final state and have not processed it yet
        ///
        /// This reference count is used to enforce deferred liberation in
        /// scenarios where udipe_finish() is called on a future that still has
        /// dependents that have not checked its final state yet.
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
        /// wait_on_address(), and will not be unset afterwards until the future
        /// is liberated. From the point where this flag is set, all status word
        /// changes will be notified via wake_by_address_all().
        ///
        /// The reason why this is a sticky flag and not a counter of waiters is
        /// that we don't have enough space in this status word to afford a
        /// counter of reasonable dynamic range...
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
        unsigned type : 3;

        union {
            /// File descriptor lock
            ///
            /// Some future types are not eagerly updated by a thread which is
            /// in charge of performing the asynchronous work. Instead they get
            /// lazily updated at the point where a user thread starts directly
            /// or indirectly waiting for their output file descriptor to signal
            /// a status change. At time of writing, this is true of collective
            /// futures based on epollfd and of timer futures based on timerfd.
            ///
            /// For all of these future types, at the time of writing again,
            /// file descriptor queries beyond simple readiness checking via
            /// epoll are thread unsafe and must be carried out by one thread at
            /// a time. Therefore, a thread that wishes to wait for such a
            /// future to transition to \ref STATE_RESULT must follow the
            /// following lock-based protocol:
            ///
            /// - Check if this locking flag is already set.
            ///     - If so, another thread is already in the process of
            ///       querying the file descriptor, and this thread can do
            ///       nothing but wait for the results. To do this we set the
            ///       `notify_address` flag if it is not set yet, then we use
            ///       a wait_on_address() loop to wait for the other thread that
            ///       arrived first to report the final state (or release the
            ///       lock in some other way).
            ///     - If not, attempt to set this flag, and if successful query
            ///       the fd, adjust the status word accordingly and clear this
            ///       lock flag along the way, and signal the status word change
            ///       via wake_by_address_all() if `notify_address` is set.
            bool output_fd_locked : 1;

            /// Truth that changes to this status word should be signaled via
            /// its output file descriptor
            ///
            /// This flag only applies to "eager" futures which support
            /// address-based signaling, as opposed to "lazy" futures which only
            /// support file descriptor signaling. The latter have no way to
            /// signal status changes other than their file descriptor, so they
            /// will always signal changes through their file descriptor and
            /// therefore don't need a flag to turn such signaling on.
            ///
            /// For eager futures, this flag works just like `notify_address`:
            /// initially unset, set the first time a thread expresses interest
            /// in receiving updates through the file descriptor path, cannot be
            /// unset afterwards until the future is destroyed.
            bool notify_fd : 1;
        };

        // NOTE: This bitfield cannot grow beyond the end of the above byte.
    } as_bitfield;

    /// Integral representation of a future status word
    ///
    /// Used to encode the `as_bitfield` state into an integer for the purpose
    /// of later injecting it into \ref udipe_future_t::status_word via atomic
    /// read-modify-write operations.
    uint32_t as_word;
} future_status_word_t;

/// \copydoc udipe_future_t
struct udipe_future_s {
    /// Result of the command, if any
    ///
    /// Once the underlying command is done running to completion, its result
    /// will be written down to this field.
    //
    // TODO: should be a union with other state for collective futures that
    //       don't produce results like join().
    alignas(FALSE_SHARING_GRANULARITY) udipe_result_payload_t payload;

    // TODO: more members which haven't been added yet

    /// Status word
    ///
    /// This innocent-looking machine word actually contains most of the
    /// synchronization-critical state of a future, bitpacked via the `as_word`
    /// variant of a \ref future_status_word_t so that it can be used for atomic
    /// read-modify-write operations. See that union's `as_bitfield` variant for
    /// more information about what information is stored there.
    ///
    /// A future's status word does double duty as a futex that can sometimes
    /// (but not always) be awaited with wait_for_address() to await
    /// `status_word` changes. When a future supports this signaling protocol,
    /// it must be requested first by setting the `notify_address` field of the
    /// status word, before beginning the wait for status changes via
    /// wait_for_address().
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
    union {
        /// eventfd in non-semaphore mode, used for eager futures
        ///
        /// This output file descriptor type is used for "eager" future types
        /// where the asynchronous operation is processed by a dedicated thread,
        /// currently \ref TYPE_NETWORK and \ref TYPE_CUSTOM.
        ///
        /// Because these futures can also signal completion via a futex or even
        /// via a mere atomic RMW operation when no one is waiting for
        /// completion yet, eventfd signaling is optional for these future types
        /// and must be explicitly enabled by setting the `notify_fd` bit of
        /// `status_word` before registering interest in this eventfd.
        ///
        /// When the task's outcome has been filed into the status word, if
        /// `notify_fd` is set, the value `1` will be written into this eventfd,
        /// which will mark it as ready for all threads that are waiting for it.
        /// These threads will then proceed to read the outcome in the status
        /// word, completing the synchronization transaction.
        int event;

        /// epollfd with a cancelation eventfd, used for collective futures
        ///
        /// This output file descriptor type is used for "collective" future
        /// types that await several other futures. It is, as the name and
        /// previous description suggest, an epollfd that monitors the readiness
        /// of the output file descriptors of all upstream futures that this
        /// collective future depends on, with user data set to the index of the
        /// upstream future from which the signal came.
        ///
        /// An additional eventfd is also registered into the epollfd with an
        /// easily identifiable invalid "index", and will be used to signal
        /// cancelation of this collective future.
        ///
        /// Because epoll is not designed for thread-safety, `epoll_wait()`
        /// transactions on this epollfd must be guarded by the `output_fd_lock`
        /// bit of `status_word`, which effectively acts as a mutex to control
        /// access to the epollfd.
        ///
        /// Whenever `epoll_wait()` output indicates that a particular upstream
        /// future has underwent a status change or this future has been
        /// canceled, the status word of the upstream future (if any) must be
        /// checked, then the fields of this collective future must be modified
        /// accordingly, and finally any other thread which registered to be
        /// notified of state changes while we were probing `epoll_wait()` must
        /// be notified via wake_by_address_all().
        int epoll;

        /// timerfd, used for timer futures
        ///
        /// This output file descriptor type is used for "timer" futures that
        /// become ready once the system clock reaches a certain time point, and
        /// may be triggered multiple times via `timerfd` logic after that.
        ///
        /// When this file descriptor becomes ready, the `output_fd_lock` lock
        /// of `status_word` must be taken, then the timerfd must be read, the
        /// fields of this collective future must be updated according to the
        /// result of the read, and finally state change waiters must be
        /// notified via wake_by_address_all().
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
    offsetof(udipe_future_t, fd) + sizeof(uint32_t) <= CACHE_LINE_SIZE,
    "Should fit on a single cache line for optimal memory access performance "
    "on CPUs where the FALSE_SHARING_GRANULARITY upper bound is pessimistic"
);
static_assert(sizeof(udipe_result_t) <= CACHE_LINE_SIZE,
              "Should always be true because future is a superset of result");

// TODO: Implement operations
