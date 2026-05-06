#pragma once

//! \file
//! \brief Core state machine of \ref udipe_future_t
//!
//! This header is not to be confused with its neighbor status.h, which contains
//! the full future status word that features this state machine along with
//! several other fields.


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

    // NOTE: If this enum gets more than 8 variants, excluding NUM_STATES which
    //       is never assigned to it, reallocate the bit budget of the state
    //       field of the future_status_word_t then adjust the _BitInt comment
    //       above for future C23 migration accordingly.
} future_state_t;
