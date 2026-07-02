#pragma once

//! \file
//! \brief Operations on the status word of \ref udipe_future_t
//!
//! In contrast with \ref status.h, which defines the \ref future_status_t of a
//! future object, this code module defines operations that target the status
//! word of a future, either directly or by manipulating the host future object.
//!
//! The two headers must be split because operations that target \ref
//! udipe_future_t need to be defined after the definition of the \ref
//! udipe_future_s struct, which itself depends on \ref future_status_t.

#include <udipe/duration.h>
#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "state.h"
#include "status.h"

#include "../address_wait.h"
#include "../error.h"
#include "../future.h"
#include "../log.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>


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
/// recycling a future from the thread-local cache, as it is not undefined
/// behavior to use it even if another thread is concurrently attempting to
/// access the same future.
///
/// But this function cannot be used for thread synchronization in general due
/// to the risk of overwriting status changes caused by other threads since the
/// last readout. You will usually need `compare_exchange` operations for that.
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

/// Atomically change a future's status and read its former status
///
/// This operation has the semantics of `atomic_exchange_explicit()`. It is
/// mostly used as an even safer alternative to future_status_store() during the
/// process of recycling a future into the thread-local cache, as it can help
/// you detect incorrect concurrent access of other threads to the same future.
///
/// But this function cannot be used for thread synchronization in general due
/// to the of overwriting status changes caused by other threads since the last
/// readout. You will usually need `compare_exchange` operations for that.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
static inline
future_status_t future_status_exchange(udipe_future_t* future,
                                       future_status_t status,
                                       memory_order order) {
    return (future_status_word_t){
        .as_word = atomic_exchange_explicit(
            &future->status_word,
            (future_status_word_t){ .as_bitfield = status }.as_word,
            order
        )
    }.as_bitfield;
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


#ifdef UDIPE_BUILD_TESTS
    /// Unit tests
    ///
    /// This function runs all the unit tests for this module. It must be called
    /// within the scope of with_logger().
    void future_status_unit_tests();
#endif
