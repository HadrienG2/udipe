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


// TODO code