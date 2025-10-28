#pragma once

//! \file
//! \brief Lock-free countdown
//!
//! This header is the home of \ref countdown_t and supporting functions, which
//! are used to detect the end of parallel work from N worker threads.

#include <udipe/pointer.h>

#include "arch.h"

#include <assert.h>
#include <stdalign.h>
#include <stdatomic.h>


/// Lock-free countdown
///
/// The \ref countdown_t type is basically a lock-free reference count that
/// comes with the additional constraint that it cannot go up, only down.
///
/// It is used in circumstances where N worker threads must do the same thing in
/// parallel then take a final action, such as freeing up resources that were
/// allocated to a task or signaling a \ref future_t that tells a client thread
/// that the task has completed. From this perspective, you can think of it as
/// the lock-free equivalent of a pthread barrier.
///
/// Its associated operations are also reasonably optimized for the common
/// special case where only one worker thread happens to be performing a
/// particular action that also supports being executed by multiple threads.
///
/// It should be initialized with countdown_initialize() once. Then whenever you
/// need to use it, set it to some initial value with countdown_set() and
/// decrement it with countdown_dec_and_check() until it reaches zero again, at
/// which point it can be set to another value and decremented again.
typedef struct countdown_s {
    alignas(FALSE_SHARING_GRANULARITY) atomic_size_t counter;
} countdown_t;
static_assert(alignof(countdown_t) == FALSE_SHARING_GRANULARITY,
              "As a synchronization variable, countdown_t must be aligned to "
              "avoid false sharing with neighboring non-synchronization state");
static_assert(sizeof(countdown_t) == FALSE_SHARING_GRANULARITY
              "Multiple countdown_t are associated with unrelated tasks and "
              "should therefore not reside on the same false sharing granule");

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
    atomic_init(&countdown->counter, 0);
}

/// (Re)set a \ref countdown_t to a nonzero value
///
/// This operation is only valid when a countdown is not in active use from
/// multiple threads, i.e. when it has just been initialized or has been taken
/// back to zero using decrements since the last time it has been set.
///
/// Debug builds will (faillibly) attempt to check for this.
///
/// \param countdown must be a \ref countdown_t that has previously been
///                  initialized using countdown_initialize_inplace() and is
///                  currently in the zeroed state.
/// \param initial must not be zero
static inline
UDIPE_NON_NULL_ARGS
void countdown_set(countdown_t* countdown, size_t initial) {
    debugf("Initializing countdown %p to %zu...", countdown, initial);
    assert(("Countdown must have a nonzero initial value", initial > 0));
    assert((
        "Countdown must be done with its previous task before being reset",
        atomic_load_explicit(&countdown->counter, memory_order_relaxed) == 0
    ));
    atomic_store_explicit(&countdown->counter, initial, memory_order_relaxed);
}

/// Decrement a \ref countdown_t and tell whether it reached zero
///
/// This atomic operation has `memory_order_release` when the counter has not
/// yet reached zero and `memory_order_acquire` when it reaches zero. Together,
/// these memory ordering constraints ensure that the thread which decrements
/// the countdown for the last time by taking it to zero also observes the
/// decrements from other threads as well as any other work that these threads
/// did before performing the countdown decrement.
static inline
UDIPE_NON_NULL_ARGS
bool countdown_dec_and_check(countdown_t* countdown) {
    tracef("Decrementing countdown %p...", countdown);
    const size_t initial_count = atomic_load_explicit(&options->counter,
                                                      memory_order_relaxed);
    assert(("Countdown has been decremented too many times",
            initial_count > 0));

    if (initial_count == 1) {
        // While this fast path may seem oddly specific, it is actually always
        // taken in the common scenario where an operation that _can_ happen in
        // parallel is scheduled in a sequential manner.
        debugf("Countdown %p is only visible by this thread, "
               "can go to 0 via the single-threaded fast path...",
               countdown);
        // With this fence, which applies to the counter load above, we
        // synchronize with every other thread that decremented the counter with
        // release ordering previously, so that any code after this function
        // returns happens after every other countdown decrement and the work
        // that preceded it in each thread.
        atomic_thread_fence(memory_order_acquire);
        // Release ordering is not needed here because we're not synchronizing
        // with any other thread by setting this counter to zero, only making
        // sure that whoever reuses this countdown later on will observe it in
        // the expected initial unused/zeroed state.
        atomic_store_explicit(&options->counter,
                              0,
                              memory_order_relaxed);
        return true;
    }

    tracef("Countdown is still visible by %zu other threads, "
           "must decrement it via the slow RMW path...",
           initial_count - 1);
    // Need release ordering here so that the thread that will perform the last
    // decrement sees everything this thread did before calling this function.
    const size_t previous_count =
        atomic_fetch_sub_explicit(&options->counter, 1, memory_order_release);

    if (previous_count == 1) {
        debugf("Countdown %p reached zero via the slow RMW path.", countdown);
        // Need this acquire fence for the same reason as the one above, but
        // this time it pairs with the fetch_sub above, which is the one that
        // observed the countdown as going from 1 to 0.
        atomic_thread_fence(memory_order_acquire);
        return true;
    }
    return false;
}


// TODO: Add unit tests
//
// TODO: Set up a pool of countdown_t in udipe_context_t which works much like
//       the existing connection pool, or just share the same bitmap allocator
//       for both and call that a parallel command slot or something? I think I
//       actually prefer that second idea.
//
// TODO: Modify connect.[ch] to use an externally provided countdown_t, which
//       will become a pointer member of shared_connect_options_t instead of an
//       inline allocation, thus taking the minimal struct size down to 128B.
