#include "pointer_cache.h"

#include <udipe/pointer.h>
#include <udipe/nodiscard.h>

#include "../../error.h"
#include "../../log.h"
#include "../../memory.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>


// === Tuning parameters ===

/// Number of \ref future_pointer_page_t in a thread-local \ref future_cache_t
///
/// A \ref future_cache_t should contain at least two \ref
/// future_pointer_page_t. Why? Because as futures get liberated, the current
/// thread's local cache fills up. And when it becomes full, we want to be able
/// to transfer one full page of futures to the global cache and replace it with
/// an empty page. And we don't want this bulk transfer process to fully empty
/// of the thread's local future cache.
//
// TODO: Tune based on benchmarking on realistic use cases
#define LOCAL_FUTURE_POINTER_PAGES ((size_t)2)


// === Function definitions ===

UDIPE_NODISCARD
UDIPE_NON_NULL_RESULT
future_pointer_page_t* future_pointer_page_initialize() {
    void* const page = malloc(get_page_size());
    exit_on_null(page, "Failed to allocate future pointer page");
    debugf("Allocated new pointer page at %p.", page);
    memset(page, 0, get_page_size());
    return (future_pointer_page_t*)page;
}

UDIPE_NODISCARD
future_pointer_cache_t future_pointer_cache_initialize(bool global) {
    future_pointer_cache_t result = { 0 };

    // The global cache starts empty, in contrast with thread-local caches which
    // have a fixed set of pointer pages + some preallocated futures inside
    if (global) {
        debug("Set up a context's global pointer cache.");
        return result;
    } else {
        debug("Setting up a thread-local pointer cache...");
    }

    // Allocate future pointer pages in reverse order, this way the "current"
    // pointer will point to the first page of the list at the end.
    future_pointer_page_t* next = NULL;
    future_pointer_page_t* current = NULL;
    static_assert(LOCAL_FUTURE_POINTER_PAGES >= (size_t)1,
                  "Thread-local future cache should have some capacity");
    for (size_t i = 0; i < LOCAL_FUTURE_POINTER_PAGES; ++i) {
        debugf("Setting up pointer page #%zu from back...", i);
        next = current;
        current = future_pointer_page_initialize();
        current->next = next;
        if (next) next->previous = current;
    }
    result.bottom = current;
    result.top = current;
    return result;
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
udipe_future_t*
future_pointer_cache_allocate_local(future_pointer_cache_t* local_cache) {
    debug("Trying to allocate a future from the thread-local cache...");
    if (local_cache->num_top_futures == 0) {
        debug("No future available. Must fall back to the global cache!");
        return NULL;
    }
    assert(local_cache->num_top_futures <= future_pointer_page_len());
    const size_t last_idx = local_cache->num_top_futures - 1;
    debugf("Can allocate future from slot #%zu of the current top page.",
           last_idx);

    assert(local_cache->top);
    udipe_future_t* const result = local_cache->top->futures[last_idx];
    assert(result);
    local_cache->top->futures[last_idx] = NULL;
    debugf("Allocated future %p, decrement num_top_futures accordingly...",
           result);

    if (--(local_cache->num_top_futures) == 0) {
        debugf("This operation emptied the current top page %p...",
               local_cache->top);
        if (local_cache->top->previous) {
            local_cache->top = local_cache->top->previous;
            local_cache->num_top_futures = future_pointer_page_len();
            debugf("...so we switched to its predecessor %p.", local_cache->top);
        } else {
            debug("...but all pages are now empty, so that's fine.");
        }
    }

    return result;
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
bool future_pointer_cache_liberate(bool global,
                                   future_pointer_cache_t* cache,
                                   udipe_future_t* future) {
    debugf("Trying to liberate future %p into cache %p...",
           future, cache);

    if (global && cache->top == NULL) {
        debug("Global cache has no pointer pages left, add one...");
        cache->top = future_pointer_page_initialize();
        cache->bottom = cache->bottom;
    }
    assert(cache->top);

    if (cache->num_top_futures == future_pointer_page_len()) {
        debugf("The current top page %p is full...", cache->top);
        if(cache->top->next) {
            cache->top = cache->top->next;
            debug("...but it has a successor that we can switch to.");
        } else if(global) {
            debug("...but this is a global cache, so we can just add one.");
            cache->top->next = future_pointer_page_initialize();
            cache->top->next->previous = cache->top->next;
            cache->top = cache->top->next;
        } else {
            debug("...and it was the last page from a bounded local cache. "
                  "Must spill into the global cache!");
            return false;
        }
        cache->num_top_futures = 0;
    }
    assert(cache->num_top_futures < future_pointer_page_len());
    const size_t next_idx = cache->num_top_futures;
    debugf("Found room in slot #%zu of top page %p. Liberating future...",
           next_idx, cache->top);

    *future = (udipe_future_t){ 0 };
    #ifdef __linux__
        future->status_sync.any = -1;
    #endif
    assert(cache->top->futures[next_idx] == NULL);
    cache->top->futures[next_idx] = future;
    ++(cache->num_top_futures);
    return true;
}

// TODO: future_pointer_cache_extract_futures
// TODO: future_pointer_cache_obtain_empty
// TODO: future_pointer_cache_insert_futures
// TODO: future_pointer_cache_insert_empty
// TODO: future_pointer_cache_refill_local

UDIPE_NON_NULL_ARGS
void future_pointer_cache_recycle_local(future_pointer_cache_t* local,
                                        future_pointer_cache_t* global) {
    // TODO: Add some logging.

    // Migrate future pointers from the top page of the thread-local cache,
    // which is special because it will usually not be full of futures.
    const size_t initial_top_futures = local->num_top_futures;
    for (size_t src = 0; src < initial_top_futures; ++src) {
        udipe_future_t* const future = future_pointer_cache_allocate_local(local);
        assert(future);
        const bool success = future_pointer_cache_liberate(true, global, future);
        assert(success);
    }
    assert((local->num_top_futures == 0 && local->top->previous == NULL)
           || (local->num_top_futures == future_pointer_page_len()));

    // TODO: Reimplement rest based on other operations. It will be less
    //       efficient, but we don't care because this operation only happens
    //       when a thread stops, which is a rare event.

    // Migrate future pointers from the previous pages of the thread-local
    // cache, if any. These should be full of futures and can thus be migrated
    // to the beginning of the global cache's pointer page list via simpler
    // doubly linked list operations.
    if (local->num_top_futures) {
        future_pointer_page_t* last_full_page = local->top;
        if (last_full_page) {
            assert(last_full_page->futures[future_pointer_page_len() - 1]);
            last_full_page->next = global->bottom;
            if (global->bottom) global->bottom->previous = last_full_page;
            global->bottom = local->bottom;
        }
        local->bottom = NULL;
    }

    // Migrate future pointer pages after the top one, which should be empty, to
    // the end of the global cache's pointer page list.
    // FIXME: Must liberate the top page too if it's empty
    future_pointer_page_t* const first_empty_page = local->top->next;
    if (first_empty_page) {
        assert(first_empty_page->futures[0] == NULL);
        future_pointer_page_t* last_empty_page = first_empty_page;
        while (last_empty_page->next) last_empty_page = last_empty_page->next;
        last_empty_page->next = global->top->next;
        if (global->top->next) global->top->next->previous = last_empty_page;
        global->top->next = first_empty_page;
    }
    local->top = NULL;
}

// TODO: future_pointer_cache_finalize + use in global_cache_finalize()
