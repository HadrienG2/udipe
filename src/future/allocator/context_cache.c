#include "context_cache.h"

#include <udipe/nodiscard.h>

#include "pointer_cache.h"

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

    trace("- Setting up the mutex...");
    int result = mtx_init(&cache.mutex, mtx_plain);
    if (result != thrd_success) {
        exit_after_c_error("Failed to set up the mutex of the context-global "
                           "futures allocator cache.");
    }

    trace("- Setting up the pointer cache...");
    cache.futures = future_pointer_cache_initialize(true);

    assert(cache.first_storage_page == NULL);

    trace("- Setting up the thread cache array...");
    const size_t ptr_size = sizeof(future_thread_cache_t*);
    cache.thread_caches_capacity = get_page_size() / ptr_size;
    cache.thread_caches = (future_thread_cache_t**)malloc(
        cache.thread_caches_capacity * ptr_size
    );
    exit_on_null(cache.thread_caches,
                 "Failed to allocate thread caches array");
    assert(cache.thread_caches_length == (size_t)0);

    return cache;
}

UDIPE_NON_NULL_ARGS
void future_context_cache_register_thread(future_context_cache_t* context_cache,
                                          future_thread_cache_t* thread_cache) {
    debugf("Registering thread cache %p into context cache %p...",
           thread_cache, context_cache);

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
        exit_on_null(context_cache->thread_caches,
                     "Failed to grow thread caches array");
        debugf("- Thread cache list is now at address %p with capacity %zu.",
               (void*)context_cache->thread_caches,
               context_cache->thread_caches_capacity);

    }

    const size_t index = (context_cache->thread_caches_length)++;
    debugf("- Will insert thread cache at index %zu of the context cache's list.",
           index);
    context_cache->thread_caches[index] = thread_cache;
}


// TODO code