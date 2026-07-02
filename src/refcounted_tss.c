#include "refcounted_tss.h"

#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>


UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
refcounted_tss_t refcounted_tss_initialize(tss_dtor_t destructor) {
    // WARNING: This function may be called by the logger implementation and
    //          must therefore not perform any logging. Normal events and
    //          non-fatal errors should not be signaled at all, fatal errors
    //          should be signalled on stderr before exiting.

    refcounted_tss_t result = { 0 };
    if (tss_create(&result.key, destructor) != thrd_success) {
        fprintf(stderr, "libudipe: Failed to set up a TLS key!\n");
        exit(EXIT_FAILURE);
    }
    atomic_init(&result.refcount_and_reachability,
                REFCOUNTED_TSS_REACHABLE);
    return result;
}

UDIPE_NODISCARD
UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2)
void* refcounted_tss_create_slot(refcounted_tss_t* tss,
                                 refcounted_tss_ctor_t constructor,
                                 void* constructor_context) {
    // WARNING: This function is called by the logger implementation and must
    //          therefore not perform any logging. Normal events and non-fatal
    //          errors should not be signaled at all, fatal errors should be
    //          signalled on stderr before exiting.

    // Increment tss refcount
    size_t old = atomic_fetch_add_explicit(&tss->refcount_and_reachability,
                                           1,
                                           memory_order_acquire);
    assert(("Should not be called on an unreachable refounted_tss_t",
            old & REFCOUNTED_TSS_REACHABLE));
    assert(("Overflow occured while incrementing refounted_tss_t refcount",
            (old + 1) & REFCOUNTED_TSS_REACHABLE));

    // Call the user-specified constructor
    void* const result = constructor(constructor_context);
    assert(("User constructor must return a valid object", result));

    // Set up TLS accordingly
    assert(("Must not be called if a slot already exists",
            tss_get(tss->key) == NULL));
    if (tss_set(tss->key, result) != thrd_success) {
        fprintf(stderr,
                "libudipe: Failed to set a TLS slot for this thread!\n");
        exit(EXIT_FAILURE);
    }
    return result;
}

UDIPE_NON_NULL_ARGS
bool refcounted_tss_release(refcounted_tss_t* tss) {
    // WARNING: This function is called on thread exit, where a logger may not
    //          be available, and must therefore not perform any logging. Normal
    //          events and non-fatal errors should not be signaled at all, fatal
    //          errors should be signalled on stderr before exiting.

    assert(("Meant to be called by the TLS destructor, which auto-clears TLS",
            tss_get(tss->key) == NULL));

    // Decrement tss refcount
    size_t old = atomic_fetch_sub_explicit(&tss->refcount_and_reachability,
                                           1,
                                           memory_order_release);
    assert(old);
    if (old > 1) return false;  // There are more thread-local slots remaining

    // This was the last thread-local slot and no other slot can be created.
    // Synchronize with the previously exiting threads...
    atomic_thread_fence(memory_order_acquire);

    // ...then destroy the TLS key and let the caller liberate the struct
    tss_delete(tss->key);
    return true;
}

UDIPE_NON_NULL_ARGS
bool refcounted_tss_discard(refcounted_tss_t* tss) {
    // Mark as unreachable
    size_t old = atomic_fetch_and_explicit(&tss->refcount_and_reachability,
                                           ~REFCOUNTED_TSS_REACHABLE,
                                           memory_order_release);
    assert(("Must not be called twice on the same refounted_tss_t",
            old & REFCOUNTED_TSS_REACHABLE));

    // Check if some references remain
    if (old != REFCOUNTED_TSS_REACHABLE) return false;

    // Otherwise destroy the tss_t right away
    atomic_thread_fence(memory_order_acquire);
    tss_delete(tss->key);
    return true;
}
