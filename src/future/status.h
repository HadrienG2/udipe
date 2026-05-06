#pragma once

//! \file
//! \brief Status word of \ref udipe_future_t
//!
//! This header is not to be confused with state.h, which contains possible
//! values for the state machine field of this status word. The status word is a
//! big bag of information, the state machine is just

#include <stdbool.h>
#include <stdint.h>


/// Maximal value of a future's `downstream_count`
///
/// Attempts to increase a future's downstream count above this should fail and
/// either be rolled back or crash the process.
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
    /// endian CPUs, which are the vast majority of today's CPUs, though special
    /// care will be needed if said increment overflows. Handling this is the
    /// job of the `downstream_count_overflow` bit below.
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

    /// Truth that changes to this status word should be notified by calling the
    /// `wake_by_address` function.
    ///
    /// This flag is initially unset when a future is set up. It is set on the
    /// first time where a thread starts waiting for state changes via
    /// wait_on_address(), and cannot be unset afterwards until the future is
    /// liberated. From the point where this flag is set, all status word
    /// changes must be notified via a variant of `wake_by_address`. Typically
    /// wake_by_address_all() is used, but lazy futures also use
    /// wake_by_address_single() to avoid thundering herds when they transfer
    /// the lazy update lock from one waiter to another.
    ///
    /// The reason why this is a sticky flag and not a counter of waiters is
    /// that we don't have enough bits in this status word to afford more than
    /// one counter of reasonable range... So we can afford to avoid unnecessary
    /// futex syscalls on futures that never need a futex syscall, but not on
    /// futures that only intermittently need such syscalls.
    bool notify_address : 1;

    /// Request for `output_sync.event` signaling or lazy future state locking
    ///
    /// The meaning of this field depends on whether you are dealing with an
    /// "eager" future, whose status is automatically changed by a dedicated
    /// thread, or with a "lazy" future, whose status is changed as a result of
    /// polling a file descriptor.
    ///
    /// # Eager future: Request for `output_sync.event` signaling
    ///
    /// "Eager" futures support address-based signaling, in contrast to "lazy"
    /// futures which only support file descriptor signaling. Therefore eager
    /// futures do not always need to signal changes through their output event
    /// object, and require a flag to be set before they start engaging in this
    /// form of signaling.
    ///
    /// For these futures, this flag works just like `notify_address`: initially
    /// unset, set the first time a thread expresses interest in receiving
    /// updates through the `output_sync.event` path, and cannot be unset
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
    ///     final state of the future (or release the lock in some other way).
    ///   - If not, attempt to set this flag, and if successful perform any
    ///     required state update operation finishing with the status word
    ///     (clearing this lock flag along the way), then signal the status word
    ///     change via wake_by_address_all() if `notify_address` is set. If you
    ///     need to exit before the outcome is available due to a timeout, then
    ///     pass on the lock to the next waiter with wake_by_address_single().
    bool notify_event_or_lazy_lock : 1;

    /// Those spare bits are reserved and must be set to 0 for now
    ///
    unsigned reserved : 2;

    // NOTE: This bitfield cannot grow beyond the end of the above byte.
} future_status_t;

/// Future status bitcasting helper
///
/// This union enables bitcasting the \ref future_status_t bitfield into a
/// 32-bit unsigned integer representation for compatibility with C11 atomic
/// operations and OS address_wait operations.
///
/// Use the `as_bitfield` variant to access its logical contents, and the
/// `as_word` variant to translate back and forth between this detailed view and
/// the 32-bit integer representation that is required to be able to use C11
/// `_Atomic` and Linux futex operations.
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
