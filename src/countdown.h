#pragma once

//! \file
//! \brief Lock-free countdown
//!
//! This header is the home of \ref countdown_t and supporting functions, which
//! are used in circumstances where N worker threads are doing something and the
//! last one to finish its task must do something else (e.g. liberate resources
//! associated with the parallel job and signal completion to a client thread).

#include <udipe/pointer.h>

#include "log.h"

#include <assert.h>
#include <stdatomic.h>


/// Lock-free countdown
///
/// A \ref countdown_t is basically a lock-free reference count that comes with
/// the additional constraint that it cannot increase, only decrease.
///
/// It is used in circumstances where all of the following is true:
///
/// - At least two worker threads are working on some task.
/// - Once the last worker is done, something must happen no matter the outcome
///   (typically liberating resources and signaling completion to the client
///   thread that initiated the parallel task).
/// - It is not necessary to track which threads completed the task so far and
///   which threads didn't (which required fancier atomic bit arrays/trees).
///
/// An example of a task for which \ref countdown_t is a good fit is the
/// cancelation of a parallel job which has failed: cancelation is considered
/// infaillible in `libudipe` as failure to cancel something is either ignored
/// with a warning or handled by crashing the application with exit(), so there
/// is no need to track which thread is done canceling and which is not done, we
/// just need to liberate task resources and send a failure notification to the
/// client thread that initiated the task once we are done.
///
/// Like all heavily mutated shared state, a \ref countdown_t should be isolated
/// into its own false sharing granule using something like
/// `alignas(FALSE_SHARING_GRANULARITY)`, away from any read-only state used for
/// the same task or mutable state used for an unrelated parallel task. But this
/// alignment is not enforced at the \ref countdown_t level because there are
/// cases where a \ref countdown_t must be grouped with other state that is used
/// to synchronize the same threads, and in that case it is fine to put all this
/// state inside of the same false sharing granule.
///
/// A \ref countdown_t should be initialized with countdown_initialize() once.
/// Then whenever you need to use it, set it to some initial value with
/// countdown_set() and decrement it with countdown_dec_and_check() until it
/// reaches zero again, at which point it can be set to another value and
/// decremented again.
typedef atomic_size_t countdown_t;

/// Initialize a \ref countdown_t
///
/// The in-place initialization pattern is unfortunately forced upon us by the
/// C11 atomics API.
///
/// After initializing a \ref countdown_t, you can use it as many times as you
/// like by giving it an initial value with countdown_set() then decrementing it
/// with countdown_dec_and_check() until it reaches zero.
static inline
UDIPE_NON_NULL_ARGS
void countdown_initialize(countdown_t* countdown) {
    atomic_init(countdown, 0);
}

/// (Re)set a \ref countdown_t to an initial value >= 2
///
/// This operation is only valid when a countdown is not in active use from
/// multiple threads, i.e. when it has just been initialized or has been taken
/// back to zero via countdown_dec_and_check() since the last time where it has
/// been set.
///
/// \param countdown must be a \ref countdown_t that has previously been
///                  initialized using countdown_initialize() and is currently
///                  in the zeroed state.
/// \param initial should be greater than or equal to 2
static inline
UDIPE_NON_NULL_ARGS
void countdown_set(countdown_t* countdown, size_t initial) {
    debugf("Initializing countdown %p to %zu...", countdown, initial);
    assert(initial >= 2);
    assert((
        "Countdown must be done with its previous task before being reset",
        atomic_load_explicit(countdown, memory_order_relaxed) == 0
    ));
    atomic_store_explicit(countdown, initial, memory_order_relaxed);
}

/// Decrement a \ref countdown_t and tell if it reached zero
///
/// This atomic operation has `memory_order_release` ordering when the counter
/// has not yet reached zero and `memory_order_acquire` when it reaches zero.
/// Together, these memory ordering constraints ensure that the thread which
/// decrements the countdown for the last time by taking it to zero also
/// observes the decrements from other threads as well as any other work that
/// these threads did before performing the countdown decrement.
static inline
UDIPE_NON_NULL_ARGS
bool countdown_dec_and_check(countdown_t* countdown) {
    tracef("Decrementing countdown %p...", countdown);
    // Need release ordering here so that the thread that will perform the last
    // decrement can synchronize-with this thread and correctly observe the
    // effect of its work on shared program state.
    const size_t previous_count =
        atomic_fetch_sub_explicit(countdown, 1, memory_order_release);
    assert(("Decremented a countdown too many times", previous_count != 0));

    if (previous_count > 1) {
        tracef("%zu more thread(s) must decrement this countdown before it reaches 0.",
               previous_count - 1);
        return false;
    }

    debugf("Countdown %p has reached zero.", countdown);
    // With this fence, which applies to the load from the fetch_sub load above,
    // we synchronize-with every other thread that decremented the counter with
    // release ordering previously, which ensures that any code after this
    // function returns happens-after every other countdown decrement and
    // correctly observes the effect of its work on shared program state.
    atomic_thread_fence(memory_order_acquire);
    return true;
}

#ifdef UDIPE_BUILD_TESTS
    /// Unit tests for \ref countdown_t
    ///
    /// This function runs all the unit tests for \ref countdown_t. It must be
    /// called within the scope of with_logger().
    void countdown_unit_tests();
#endif
