#include "thread_cache.h"

#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "pointer_cache.h"
#include "sync_caches.h"

#include "../../address_wait.h"
#include "../../context.h"
#include "../../error.h"
#include "../../log.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
future_thread_cache_t* future_thread_cache_initialize(udipe_context_t* context) {
    debugf("Setting up a new thread-local future cache in context %p...",
           (void*)context);

    debug("- Allocating the shared struct...");
    future_thread_cache_t* const cache = (future_thread_cache_t*)malloc(
        sizeof(future_thread_cache_t)
    );
    exit_on_null(cache, "Failed to allocate cache");
    memset(cache, 0, sizeof(future_thread_cache_t));
    debugf(" ...done, it will reside at address %p.", (void*)cache);

    debug("- Setting up the future pointer cache...");
    cache->futures = future_pointer_cache_initialize(false);

    debug("- Setting up the event object cache...");
    cache->events = event_cache_initialize();

    #ifdef __linux__
        debug("- Setting up the epollfd+eventfd cache...");
        cache->epolls_with_events = epoll_event_cache_initialize();
    #endif

    debug("- Setting up flags...");
    atomic_init(&cache->flags, 0);

    cache->context = context;
    return cache;
}

// TODO: Rework this using the new flags-based synchronization design
/*UDIPE_NON_NULL_ARGS
void future_thread_cache_finalize_from_thread(future_thread_cache_t** pcache) {
    // WARNING: This function is called by the TSS destructor at a time where
    //          the context may be finalized and the associated logger may not
    //          be available anymore.
    //
    //          It must therefore not perform any logging before establishing
    //          that it is on the code path where it wins the race and switches
    //          the state machine to the SPILLING state (which will make context
    //          liberation wait for us). And even if it gets there it must stop
    //          logging before switching to the final EMPTIED state which
    //          enables context finalization to run again.
    //
    //          Outside of the SPILLING -> EMPTIED region, normal events and
    //          non-fatal errors should not be signaled at all, while fatal
    //          errors should be signalled on stderr before exiting.

    future_thread_cache_t* cache = *pcache;
    assert(cache);

    // Race to perfom the READY -> SPILLING state transition
    // TODO: Generalize into a function that
    //       future_thread_cache_finalize_from_context() can call too
    bool won_race = false;
    uint32_t expected = atomic_load_explicit(&cache->futex,
                                             memory_order_relaxed);
    while ((expected & THREAD_CACHE_STATE_BITS) == THREAD_CACHE_READY) {
        const uint32_t desired = (expected & ~THREAD_CACHE_STATE_BITS)
                               | THREAD_CACHE_SPILLING;
        const bool result = atomic_compare_exchange_weak_explicit(
            &cache->futex,
            &expected,
            desired_state,
            memory_order_acquire,
            memory_order_relaxed
        );
        if (result) {
            expected = desired;
            won_race = true;
            break;
        }
    }
    if (!won_race) atomic_thread_fence(memory_order_acquire);

    if (won_race) {
        // TODO implement depending on won_race, if won remember to enable logging
        fprintf(stderr, "Not implemented yet!\n");
        exit(EXIT_FAILURE);

        // Switch to the EMPTIED state and wake up the context liberation code
        atomic_fetch_or_explicit(&cache->futex,
                                 THREAD_CACHE_EMPTIED,
                                 memory_order_release);
        // FIXME: Logging should still be enabled at this point
        wake_by_address_all(&cache->futex);
    }

    // Notify the context liberation code that we are done using the context
    const uint32_t pre_final_state = atomic_fetch_or_explicit(
        &cache->futex,
        THREAD_CACHE_THREAD_DONE,
        memory_order_release
    );
    const bool context_done = (pre_final_state & THREAD_CACHE_CONTEXT_DONE) != 0;

    // Release the TLS slot associated with this thread cache and liberate the
    // udipe_context_t if this was the last reference to it
    if (refcounted_tss_release(&cache->context->thread_future_cache)) {
        free((void*)(cache->context));
    }
    cache->context = NULL;

    // Liberate the thread cache if we were its last user
    if (context_done) {
        atomic_thread_fence(memory_order_acquire);
        free((void*)cache);
    }

    *pcache = NULL;
}

UDIPE_NON_NULL_ARGS
void future_thread_cache_finalize_from_context(future_thread_cache_t** pcache) {
    future_thread_cache_t* cache = *pcache;
    assert(cache);

    debugf("Liberating thread cache %p from the udipe_finalize() path...",
           cache);

    debug("- Racing to perform the READY -> LIBERATING state transition...");
    // TODO: Replace with extracted utility from above
    bool won_race = false;
    uint32_t expected = atomic_load_explicit(&cache->futex,
                                             memory_order_relaxed);
    while ((expected & THREAD_CACHE_STATE_BITS) == THREAD_CACHE_READY) {
        const uint32_t desired = (expected & ~THREAD_CACHE_STATE_BITS)
                               | THREAD_CACHE_LIBERATING;
        const bool result = atomic_compare_exchange_weak_explicit(
            &cache->futex,
            &expected,
            desired_state,
            memory_order_acquire,
            memory_order_relaxed
        );
        if (result) {
            expected = desired;
            won_race = true;
            break;
        }
    }
    if (!won_race) atomic_thread_fence(memory_order_acquire);

    // TODO implement depending on won_race
    exit_with_error("Not implemented yet!");

    *pcache = NULL;
} */
