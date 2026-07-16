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
    LOGGED_FUNCTION_START_NO_PARAMS
        void* const page = malloc(get_page_size());
        exit_on_null(page, "Failed to allocate future pointer page");
        debugf("Allocated new pointer page at %p.", page);
        memset(page, 0, get_page_size());
        return (future_pointer_page_t*)page;
    LOGGED_FUNCTION_END
}

UDIPE_NODISCARD
future_pointer_cache_t future_pointer_cache_initialize(bool global) {
    LOGGED_FUNCTION_START("%u", global)
        future_pointer_cache_t result = { 0 };

        // The global cache starts empty, in contrast with thread-local caches
        // which have a fixed set of preallocated pointer pages
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
            debugf("- Setting up pointer page #%zu from back...", i);
            next = current;
            current = future_pointer_page_initialize();
            current->next = next;
            if (next) next->previous = current;
        }
        result.bottom = current;
        result.top = current;
        return result;
    LOGGED_FUNCTION_END
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
udipe_future_t*
future_pointer_cache_allocate_local(future_pointer_cache_t* local_cache) {
    LOGGED_FUNCTION_START("%p", local_cache)
        if (local_cache->num_top_futures == 0) {
            debug("No future available. Time to steal from the global cache!");
            return NULL;
        }
        assert(local_cache->num_top_futures <= future_pointer_page_capacity());
        const size_t last_idx = local_cache->num_top_futures - 1;
        debugf("Can allocate future from slot #%zu of the current top page.",
               last_idx);

        assert(local_cache->top);
        udipe_future_t* const future = local_cache->top->futures[last_idx];
        assert(future);
        local_cache->top->futures[last_idx] = NULL;
        debugf("Allocated future %p, decrement num_top_futures accordingly...",
               future);

        if (--(local_cache->num_top_futures) == 0) {
            debugf("This operation emptied the current top page %p...",
                   local_cache->top);
            if (local_cache->top->previous) {
                local_cache->top = local_cache->top->previous;
                local_cache->num_top_futures = future_pointer_page_capacity();
                debugf("...so we switched to its predecessor %p.", local_cache->top);
            } else {
                debug("...which has no predecessor, so this cache is now empty.");
            }
        }
        return future;
    LOGGED_FUNCTION_END
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
bool future_pointer_cache_liberate_local(future_pointer_cache_t* local_cache,
                                         udipe_future_t* future) {
    LOGGED_FUNCTION_START("%p, %p", local_cache, future)
        assert(local_cache->top);
        if (local_cache->num_top_futures == future_pointer_page_capacity()) {
            debugf("The current top page %p is full...", local_cache->top);
            if (local_cache->top->next == NULL) {
                debug("...and it was the last page from this cache. "
                      "Time to spill into the global cache!");
                return false;
            }
            local_cache->top = local_cache->top->next;
            local_cache->num_top_futures = 0;
            debug("...but it has a successor page that we could switch to.");
        }
        assert(local_cache->num_top_futures < future_pointer_page_capacity());
        const size_t next_idx = local_cache->num_top_futures;

        debugf("Found room in slot #%zu of top page %p. Liberating future...",
               next_idx, local_cache->top);
        *future = (udipe_future_t){ 0 };
        #ifdef __linux__
            future->status_sync.any = -1;
        #endif
        assert(local_cache->top->futures[next_idx] == NULL);
        local_cache->top->futures[next_idx] = future;
        ++(local_cache->num_top_futures);
        return true;
    LOGGED_FUNCTION_END
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_pointer_page_t*
future_pointer_cache_extract_futures(future_pointer_cache_t* cache) {
    LOGGED_FUNCTION_START("%p", cache)
        debugf("Looking for an non-empty pointer page in cache %p...", cache);
        if (cache->num_top_futures == 0 || cache->bottom == NULL) {
            debug("No luck, there are no futures in this cache.");
            return NULL;
        }

        future_pointer_page_t* const extracted = cache->bottom;
        cache->bottom = extracted->next;
        debugf("Extracting bottom future page %p, new bottom page is %p.",
               extracted, cache->bottom);
        if (cache->top == extracted) {
            debug("That was also the top page, so we emptied this cache.");
            cache->top = cache->bottom;
            cache->num_top_futures = 0;
        }

        debug("Unlinking the extracted page from its successors...");
        assert(extracted->previous == NULL);  // Because it was the bottom page
        future_pointer_page_unlink(extracted);
        return extracted;
    LOGGED_FUNCTION_END
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
future_pointer_page_t*
future_pointer_cache_obtain_empty(future_pointer_cache_t* cache) {
    LOGGED_FUNCTION_START("%p", cache)
        debugf("Looking for an empty pointer page in cache %p...", cache);

        if (cache->top == NULL) {
            debug("There is no page in this cache, must allocate.");
            return future_pointer_page_initialize();
        }

        if (cache->top->next) {
            future_pointer_page_t* const extracted = cache->top->next;
            debugf("Extracting after-top page %p as it is easiest...",
                   extracted);
            future_pointer_page_unlink(extracted);
            return extracted;
        }

        if (cache->num_top_futures >= 1) {
            debug("All pages have futures in them, must allocate.");
            return future_pointer_page_initialize();
        }

        future_pointer_page_t* const extracted = cache->top;
        assert(extracted->next == NULL);
        assert(extracted->previous == NULL);
        debugf("Extracted last page from cache %p.", extracted);
        cache->top = NULL;
        cache->bottom = NULL;
        return extracted;
    LOGGED_FUNCTION_END
}

UDIPE_NON_NULL_ARGS
void future_pointer_cache_insert_futures(future_pointer_cache_t* cache,
                                         future_pointer_page_t* non_empty) {
    LOGGED_FUNCTION_START("%p, %p", cache, non_empty)
        assert(non_empty->next == NULL);
        assert(non_empty->previous == NULL);
        const size_t num_futures = future_pointer_page_occupancy(non_empty);
        assert(num_futures >= 1);
        debugf("Transferring page %p with %zu future pointers into cache %p...",
               non_empty, num_futures, cache);

        if (cache->num_top_futures == 0  // If the target cache is empty...
            || num_futures == future_pointer_page_capacity()  // ...or the source page is full.
        ) {
            debug("This page is safe to insert at the bottom of the cache...");
            non_empty->next = cache->bottom;
            if (cache->bottom) {
                debugf("...replacing the former bottom page %p...",
                       cache->bottom);
                assert(cache->bottom->previous == NULL);
                cache->bottom->previous = non_empty;
            }
            if (cache->num_top_futures == 0) {
                assert(cache->top == cache->bottom);
                debug("...and becoming the top page in the process.");
                cache->top = non_empty;
                cache->num_top_futures = num_futures;
            }
            cache->bottom = non_empty;
            return;
        }
        assert(cache->top
               && cache->num_top_futures != 0
               && num_futures != future_pointer_page_capacity());

        const size_t top_capacity =
            future_pointer_page_capacity() - cache->num_top_futures;
        debugf(
            "Must merge partial input into non-empty top page %p, "
            "which has room for %zu futures.",
            cache->top, top_capacity
        );

        const size_t moved_futures =
            (num_futures <= top_capacity) ? num_futures : top_capacity;
        debugf("Moving the top %zu futures into the top page...",
               moved_futures);
        future_pointer_page_t* const input = non_empty;
        udipe_future_t** const move_source =
            input->futures + (num_futures - moved_futures);
        udipe_future_t** const move_destination =
            cache->top->futures + cache->num_top_futures;
        const size_t move_size = moved_futures * sizeof(udipe_future_t*);
        memcpy(move_destination, move_source, move_size);
        memset(move_source, 0, move_size);
        cache->num_top_futures += moved_futures;
        const size_t remaining_futures = num_futures - moved_futures;

        if (remaining_futures == 0) {
            debug("This emptied the input, so we insert it as an empty page.");
            future_pointer_cache_insert_empty(cache, input);
            return;
        }
        assert(remaining_futures >= 1 && cache->num_top_futures == top_capacity);

        debug("Now the top page is full but the input page still has futures. "
              "This remainder will become the new top page.");
        input->next = cache->top->next;
        input->previous = cache->top;
        cache->top->next = input;
        if (input->next) input->next->previous = input;
        cache->top = input;
        cache->num_top_futures = remaining_futures;
    LOGGED_FUNCTION_END
}

UDIPE_NON_NULL_ARGS
void future_pointer_cache_insert_empty(future_pointer_cache_t* cache,
                                       future_pointer_page_t* empty) {
    LOGGED_FUNCTION_START("%p, %p", cache, empty)
        assert(empty->next == NULL);
        assert(empty->previous == NULL);
        assert(empty->futures[0] == NULL);
        debugf("Transferring empty pointer page %p into cache %p...",
               empty, cache);

        if (cache->top == NULL) {
            assert(cache->bottom == NULL);
            assert(cache->num_top_futures == 0);
            debug("There was no page before, "
                  "so this becomes the top/bottom page.");
            cache->top = empty;
            cache->bottom = empty;
            return;
        }
        assert(cache->top);

        debugf("It will be inserted after the top page %p.", cache->top);
        empty->previous = cache->top;
        empty->next = cache->top->next;
        cache->top->next = empty;
        if (empty->next) empty->next->previous = empty;
    LOGGED_FUNCTION_END
}

UDIPE_NON_NULL_ARGS
void future_pointer_cache_refill_local(future_pointer_cache_t* local_cache,
                                       future_storage_page_t* new_futures) {
    LOGGED_FUNCTION_START("%p, %p", local_cache, new_futures)
        assert(local_cache->num_top_futures == 0);
        assert(local_cache->top);
        assert(future_storage_page_length() <= future_pointer_page_capacity());
        for (size_t i = 0; i < future_storage_page_length(); ++i) {
            local_cache->top->futures[i] = &new_futures->futures[i];
        }
        local_cache->num_top_futures = future_storage_page_length();
    LOGGED_FUNCTION_END
}

UDIPE_NON_NULL_ARGS
void future_pointer_cache_spill(future_pointer_cache_t* local,
                                future_pointer_cache_t* global) {
    LOGGED_FUNCTION_START("%p, %p", local, global)
        debug("Transferring non-empty pages...");
        while (local->num_top_futures != 0) {
            future_pointer_page_t* const futures =
                future_pointer_cache_extract_futures(local);
            future_pointer_cache_insert_futures(global, futures);
        }

        debug("Transferring empty pages...");
        while (local->top) {
            future_pointer_page_t* const empty =
                future_pointer_cache_obtain_empty(local);
            future_pointer_cache_insert_empty(global, empty);
        }

        debug("Checking final local state in debug builds...");
        assert(local->bottom == NULL);
        assert(local->top == NULL);
        assert(local->num_top_futures == 0);
    LOGGED_FUNCTION_END
}

UDIPE_NON_NULL_ARGS
void future_pointer_cache_finalize(future_pointer_cache_t* cache) {
    LOGGED_FUNCTION_START("%p", cache)
        future_pointer_page_t* page = cache->bottom;
        assert(page == NULL || page->previous == NULL);
        while (page) {
            tracef("- Liberating pointer page %p...", page);
            future_pointer_page_t* const next_page = page->next;
            free((void*)page);
            page = next_page;
        }

        debug("Poisoning remaining pointer cache state...");
        cache->bottom = NULL;
        cache->top = NULL;
        cache->num_top_futures = 0;
    LOGGED_FUNCTION_END
}
