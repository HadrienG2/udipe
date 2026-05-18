#include "pointer_cache.h"

#include <udipe/pointer.h>
#include <udipe/nodiscard.h>

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
    assert(page);
    memset(page, 0, get_page_size());
    return (future_pointer_page_t*)page;
}

// FIXME: Adjust implementations of the following methods according to the new
//        context-local design described in their documentation.

UDIPE_NODISCARD
future_cache_t future_cache_initialize(bool global) {
    future_cache_t result = { 0 };

    // The global cache starts empty, in contrast with thread-local caches which
    // have a fixed set of pointer pages + some preallocated futures inside
    if (global) return result;

    // Allocate future pointer pages in reverse order, this way the "current"
    // pointer will point to the first page of the list at the end.
    future_pointer_page_t* next = NULL;
    future_pointer_page_t* current = NULL;
    static_assert(LOCAL_FUTURE_POINTER_PAGES >= (size_t)1,
                  "Thread-local future cache should have some capacity");
    for (size_t i = 0; i < LOCAL_FUTURE_POINTER_PAGES; ++i) {
        next = current;
        current = future_pointer_page_initialize();
        current->next = next;
        if (next) next->previous = current;
    }
    result.bottom = current;
    result.top = current;
    return result;
}

// TODO: future_cache_local_allocate
// TODO: future_cache_local_liberate
// TODO: future_cache_extract_futures
// TODO: future_cache_obtain_empty
// TODO: future_cache_insert_futures
// TODO: future_cache_insert_empty
// TODO: future_cache_local_refill

UDIPE_NON_NULL_ARGS
void future_cache_local_recycle(future_cache_t* local, future_cache_t* global) {
    // NOTE: This function is called at thread exit time and therefore cannot
    //       use logging as no logger should be set up at this point.

    // TODO: Consider extracting some reusable operations out of this, but make
    //       sure these operations have no logging.

    // Migrate future pointers from the top page of the thread-local cache,
    // which is special because it will usually not be full of futures.
    for (size_t src = 0; src < local->num_top_futures; ++src) {
        // Extract a future from the top pointer page of the local cache
        assert(local->top);
        udipe_future_t* const future = local->top->futures[src];
        local->top->futures[src] = NULL;
        assert(future);
        if (global->top == NULL) {
            // Allocate the first pointer storage of the global cache if
            // currently holds no pointer storage page.
            assert(global->bottom == NULL);
            global->bottom = future_pointer_page_initialize();
            global->top = global->bottom;
        } else if(global->num_top_futures == future_pointer_page_len()) {
            // Allocate a new pointer storage page if the last storage page of
            // the global cache is full and can't host the new future.
            future_pointer_page_t* const new = future_pointer_page_initialize();
            assert(new);
            global->top->next = new;
            new->previous = global->top;
            global->top = new;
            global->num_top_futures = 0;
        }
        // Migrate the future from the local cache to the top page of the global
        // cache, which is now ready to hold it.
        assert(global->top && global->num_top_futures < future_pointer_page_len());
        assert(global->top->futures[global->num_top_futures] == NULL);
        global->top->futures[(global->num_top_futures)++] = future;
    }
    local->num_top_futures = 0;

    // Migrate future pointers from the previous pages of the thread-local
    // cache, if any. These should be full of futures and can thus be migrated
    // to the beginning of the global cache's pointer page list via simpler
    // doubly linked list operations.
    future_pointer_page_t* last_full_page = local->top->previous;
    if (last_full_page) {
        assert(last_full_page->futures[future_pointer_page_len() - 1]);
        last_full_page->next = global->bottom;
        if (global->bottom) global->bottom->previous = last_full_page;
        global->bottom = local->bottom;
    }
    local->bottom = NULL;

    // Migrate future pointer pages after the top one, which should be empty, to
    // the end of the global cache's pointer page list.
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

    // Migrate future storage pages from the local cache to the global cache
    if (local->storage) {
        future_storage_page_t* last_storage_page = local->storage;
        while (last_storage_page->next) last_storage_page = last_storage_page->next;
        last_storage_page->next = global->storage;
        global->storage = local->storage;
        local->storage = NULL;
    }
}

// TODO: future_cache_finalize + use in global_cache_finalize()
