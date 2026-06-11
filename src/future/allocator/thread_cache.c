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
#include <threads.h>


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

UDIPE_NON_NULL_ARGS
void future_thread_cache_finalize_from_thread(future_thread_cache_t** pcache) {
    // WARNING: This function is called by the TSS destructor at a time where
    //          the context may be partially finalized and the associated logger
    //          may not be available anymore.
    //
    //          Therefore, logging is only permitted if the THREAD_DYING flag is
    //          set before the CONTEXT_DYING flag, which results in context
    //          destruction waiting for the thread cache to spill its contents.
    //          And even then it is only permitted until the point where the
    //          THREAD_DONE flag is set, which resumes context destruction.
    //
    //          Outside of this hypothetical code span, normal events and
    //          non-fatal errors should not be signaled at all, while fatal
    //          errors should be signaled on stderr before exiting.
    future_thread_cache_t* cache = *pcache;
    assert(cache);

    // Mark this thread as dying
    uint32_t previous_flags = atomic_fetch_or_explicit(
        &cache->flags,
        THREAD_CACHE_THREAD_DYING,
        // - Need an acquire barrier to ensure that the following code is not
        //   reordered before this signal is sent.
        // - No release barrier needed because it is fine if previous writes to
        //   the cache are received after this signal, as this signal can only
        //   inhibit memory operations on non-flags members of the cache and not
        //   trigger extra operations.
        // - No sequential consistency needed because we're only using a single
        //   atomic variable to synchronize.
        memory_order_acquire
    );
    assert((previous_flags & (THREAD_CACHE_THREAD_DYING | THREAD_CACHE_THREAD_DONE)) == 0);

    // If we got there before the context destructor, spill and destroy the
    // thread cache's contents
    if ((previous_flags & THREAD_CACHE_CONTEXT_DYING) == 0) {
        // Here we can use the logger because if the context starts being
        // liberated it will notice that THREAD_DYING is set and wait for us.
        with_logger(&cache->context->logger, {
            debugf("Spilling thread cache %p into global cache of context %p "
                   "as the corresponding thread is exiting...",
                   (void*)cache, (void*)cache->context);
            exit_on_thread_error(
                mtx_lock(&cache->context->global_future_cache.mutex),
                "Failed to lock the context cache's mutex."
            );
            future_pointer_cache_spill(&cache->futures,
                                       &cache->context->global_future_cache.futures);
            exit_on_thread_error(
                mtx_unlock(&cache->context->global_future_cache.mutex),
                "Failed to lock the context cache's mutex."
            );

            debug("Liberating event objects...");
            event_cache_finalize(&cache->events);

            #ifdef __linux__
                debug("Liberating epollfd+eventfd pairs...");
                epoll_event_cache_finalize(&cache->epolls_with_events);
            #endif

            debug("Notifying the context destructor that we are done...");
            previous_flags = atomic_fetch_or_explicit(
                &cache->flags,
                THREAD_CACHE_EMPTIED,
                // - Need an acquire barrier because the wake_by_address signal
                //   must be sent after this flag is set.
                // - Need a release barrier because the signal must not be sent
                //   before previous cache operations complete, as 1/some of
                //   them rely on the global cache remaining untouched and 2/we
                //   want to minimize the time window between setting EMPTIED
                //   and setting THREAD_DONE as the context destructor will be
                //   busy-waiting for us during that time.
                // - No sequential consistency needed because we're only using a
                //   single atomic variable to synchronize.
                memory_order_acq_rel
            );
            assert((previous_flags & (THREAD_CACHE_EMPTIED | THREAD_CACHE_THREAD_DONE)) == 0);
            if (previous_flags & THREAD_CACHE_CONTEXT_DYING) {
                debug("Waking up possibly-asleep context destructor...");
                wake_by_address_all(&cache->flags);
            }
        });
    }
    // No need to await the context destructor if it came here first because the
    // following operations can safely happen in parallel with context
    // destruction as all dangerous operations are refcount-protected.

    // Decrement context refcount, deallocate context if this we held the last
    // reference to it
    if (refcounted_tss_release(&cache->context->thread_future_cache)) {
        free((void*)(cache->context));
    }
    cache->context = NULL;

    // Record that we are done with this cache, deallocate it if we held the
    // last reference to it.
    previous_flags = atomic_fetch_or_explicit(
        &cache->flags,
        THREAD_CACHE_THREAD_DONE,
        // - Acquire barrier is only needed on the liberation path below,
        //   otherwise the cache won't be accessed again so we don't need an up
        //   to date view of its contents.
        // - Need a release barrier because this signal should not be sent until
        //   we are done interacting with the cache, as the cache can be
        //   liberated by the context destructor at any point after it receives
        //   that signal.
        // - No sequential consistency needed because we're only using a single
        //   atomic variable to synchronize.
        memory_order_release
    );
    assert((previous_flags & THREAD_CACHE_THREAD_DONE) == 0);
    if (previous_flags & THREAD_CACHE_CONTEXT_DONE) {
        assert(previous_flags & THREAD_CACHE_CONTEXT_DYING);
        atomic_thread_fence(memory_order_acquire);
        free(cache);
    }

    *pcache = NULL;
}

UDIPE_NON_NULL_ARGS
void future_thread_cache_finalize_from_context(future_thread_cache_t** pcache) {
    future_thread_cache_t* cache = *pcache;
    assert(cache);

    debugf("Liberating thread cache %p from the udipe_finalize() code path...",
           cache);
    uint32_t previous_flags = atomic_fetch_or_explicit(
        &cache->flags,
        THREAD_CACHE_CONTEXT_DYING,
        // - Need an acquire barrier to ensure that the following code is not
        //   reordered before this signal is sent.
        // - No release barrier needed because it is fine if previous writes to
        //   the cache are received after this signal, as this signal can only
        //   inhibit memory operations on non-flags members of the cache and not
        //   trigger extra operations.
        // - No sequential consistency needed because we're only using a single
        //   atomic variable to synchronize.
        memory_order_acquire
    );
    assert((previous_flags & (THREAD_CACHE_CONTEXT_DYING | THREAD_CACHE_CONTEXT_DONE)) == 0);

    // If we got there before the TSS destructor, liberate the thread cache's
    // contents without going through the context cache as we're going to nuke
    // the latter anyway.
    if ((previous_flags & THREAD_CACHE_THREAD_DYING) == 0) {
        debug("Liberating thread cache contents...");
        future_pointer_cache_finalize(&cache->futures);

        debug("Liberating event objects...");
        event_cache_finalize(&cache->events);

        #ifdef __linux__
            debug("Liberating epollfd+eventfd pairs...");
            epoll_event_cache_finalize(&cache->epolls_with_events);
        #endif

        debug("Notifying the TSS destructor that we are done...");
        previous_flags = atomic_fetch_or_explicit(
            &cache->flags,
            THREAD_CACHE_EMPTIED,
            // - No acquire barrier necessary because we're not doing anything
            //   before the next access to the `flags` variable.
            // TODO: Review all the code and check if the release is truly needed here, I'm suspicious
            // - Need a release barrier because the signal must not be sent
            //   before previous cache operations complete, as 1/some of
            //   them rely on the global cache remaining untouched and 2/we
            //   want to minimize the time window between setting EMPTIED
            //   and setting THREAD_DONE as the context destructor will be
            //   busy-waiting during that time.
            // - No sequential consistency needed because we're only using a
            //   single atomic variable to synchronize.
            memory_order_acq_rel
        );
        assert((previous_flags & (THREAD_CACHE_EMPTIED | THREAD_CACHE_CONTEXT_DONE)) == 0);
    } else {
        debug("Waiting for the TSS destructor to finish spilling "
              "the thread cache's contents...");
        while ((previous_flags & THREAD_CACHE_EMPTIED) == 0) {
            if (wait_on_address(&cache->flags,
                                previous_flags,
                                UDIPE_DURATION_MAX)) {
                previous_flags = atomic_load_explicit(
                    &cache->flags,
                    // - Need an acquire barrier to ensure that the next
                    //   wait_on_address call is not reordered before this load.
                    // - No release barrier possible on a load.
                    // - No sequential consistency needed because we're only
                    //   using a single atomic variable to synchronize.
                    memory_order_acquire
                );
            }
        }

        debug("Busy-waiting for the TSS destructor to finish accessing "
              "the underlying udipe_context_t so we can destroy it...");
        while ((previous_flags & THREAD_CACHE_THREAD_DONE) == 0) {
            previous_flags = atomic_load_explicit(
                &cache->flags,
                // - Neither acquire nor sequential consistency are needed
                //   because we're not accessing any other data until the end of
                //   the loop and the next operation after the loop is an atomic
                //   operation on the same variable.
                // - No release barrier possible on a loop.
                memory_order_relaxed
            );
        }
    }

    // Record that we are done with this cache, deallocate it if we held the
    // last reference to it.
    previous_flags = atomic_fetch_or_explicit(
        &cache->flags,
        THREAD_CACHE_CONTEXT_DONE,
        // - Acquire barrier is only needed on the liberation path below,
        //   otherwise the thread cache won't be accessed again so we don't need
        //   an up to date view of its contents.
        // - Need a release barrier because this signal should not be sent until
        //   we are done interacting with the cache, as the cache can be
        //   liberated by the TSS destructor at any point after it receives
        //   that signal.
        // - No sequential consistency needed because we're only using a single
        //   atomic variable to synchronize.
        memory_order_release
    );
    assert(previous_flags & THREAD_CACHE_EMPTIED);
    assert((previous_flags & THREAD_CACHE_CONTEXT_DONE) == 0);
    if (previous_flags & THREAD_CACHE_THREAD_DONE) {
        assert(previous_flags & THREAD_CACHE_THREAD_DYING);
        atomic_thread_fence(memory_order_acquire);
        free(cache);
    }

    *pcache = NULL;
}
