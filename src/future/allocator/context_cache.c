#include "context_cache.h"

#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "pointer_cache.h"
#include "storage_page.h"
#include "thread_cache.h"

#include "../../error.h"
#include "../../log.h"
#include "../../memory.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <threads.h>


UDIPE_NODISCARD
future_context_cache_t future_context_cache_initialize() {
    debug("Setting up a future context cache...");
    future_context_cache_t cache = { 0 };

    debug("- Setting up the mutex...");
    exit_on_thread_error(
        mtx_init(&cache.mutex, mtx_plain),
        "Failed to set up the context cache's mutex."
    );

    debug("- Setting up the pointer cache...");
    cache.futures = future_pointer_cache_initialize(true);

    assert(cache.first_storage_page == NULL);

    debug("- Setting up the thread cache array...");
    const size_t ptr_size = sizeof(future_thread_cache_t*);
    cache.thread_caches_capacity = get_page_size() / ptr_size;
    cache.thread_caches = (future_thread_cache_t**)malloc(
        cache.thread_caches_capacity * ptr_size
    );
    exit_on_null(cache.thread_caches,
                 "Failed to allocate thread caches array");
    assert(cache.thread_caches_length == (size_t)0);
    debugf("  ...done, thread cache array is located at %p with capacity %zu.",
           (void*)cache.thread_caches, cache.thread_caches_capacity);

    return cache;
}

UDIPE_NON_NULL_ARGS
void future_context_cache_register_thread(future_context_cache_t* context_cache,
                                          future_thread_cache_t* thread_cache) {
    debugf("Registering thread cache %p into context cache %p...",
           thread_cache, context_cache);

    exit_on_thread_error(mtx_lock(&context_cache->mutex),
                         "Failed to lock the context cache's mutex.");

    assert(context_cache->thread_caches);
    assert(context_cache->thread_caches_length
           <= context_cache->thread_caches_capacity);
    if (
        context_cache->thread_caches_length
        == context_cache->thread_caches_capacity
    ) {
        debugf("- Growing thread cache list %p which has outgrown its current "
               "capacity %zu...",
               (void*)context_cache->thread_caches,
               context_cache->thread_caches_capacity);
        context_cache->thread_caches_capacity *= 2;
        context_cache->thread_caches = (future_thread_cache_t**)realloc(
            (void*)context_cache->thread_caches,
            context_cache->thread_caches_capacity * sizeof(future_thread_cache_t*)
        );
        if (context_cache->thread_caches == NULL) {
            exit_on_thread_error(
                mtx_unlock(&context_cache->mutex),
                "Failed to unlock the context cache's mutex."
            );
            exit_after_c_error("Failed to grow thread caches array");
        }
        debugf("- Thread cache list is now at address %p with capacity %zu.",
               (void*)context_cache->thread_caches,
               context_cache->thread_caches_capacity);

    }

    const size_t index = (context_cache->thread_caches_length)++;
    debugf("- Will insert thread cache at index %zu of the context cache's list.",
           index);
    context_cache->thread_caches[index] = thread_cache;

    exit_on_thread_error(mtx_unlock(&context_cache->mutex),
                         "Failed to unlock the context cache's mutex.");
}

UDIPE_NON_NULL_ARGS
void future_context_cache_finalize_threads(future_context_cache_t* cache) {
    // Notice that cache->mutex is not taken here. This is done deliberately in
    // order to avoid the deadlock scenario discussed in the thread_caches
    // member's documentation. Associated race conditions are handled in the
    // manner that is described in said documentation.

    debugf("Finalizing thread cache array at %p...", cache->thread_caches);
    assert(cache->thread_caches);
    assert(cache->thread_caches_length <= cache->thread_caches_capacity);
    for (size_t i = 0; i < cache->thread_caches_length; ++i) {
        tracef("Finalizing thread cache #%zu...", i);
        future_thread_cache_finalize_from_context(&(cache->thread_caches[i]));
    }

    free((void*)cache->thread_caches);
    cache->thread_caches = NULL;
    cache->thread_caches_length = 0;
    cache->thread_caches_capacity = 0;
}

UDIPE_NON_NULL_ARGS
void future_context_cache_finalize(future_context_cache_t* cache) {
    debugf("Finalizing context cache %p...", cache);

    debug("- Finalizing the thread-local caches...");
    future_context_cache_finalize_threads(cache);

    exit_on_thread_error(mtx_lock(&cache->mutex),
                         "Failed to lock the context cache's mutex.");

    debug("- Finalizing the global pointer cache...");
    future_pointer_cache_finalize(&cache->futures);

    debug("- Finalizing the storage cache...");
    future_storage_liberate_all(&cache->first_storage_page);

    exit_on_thread_error(mtx_unlock(&cache->mutex),
                         "Failed to unlock the context cache's mutex.");
}
