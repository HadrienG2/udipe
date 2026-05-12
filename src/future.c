#include "future.h"

#include <udipe/context.h>
#include <udipe/duration.h>
#include <udipe/nodiscard.h>
#include <udipe/pointer.h>
#include <udipe/result.h>

#include "future/outcome.h"
#include "future/state.h"
#include "future/status_ops.h"
#include "future/type.h"
#include "future/wait.h"

#include "context.h"
#include "error.h"
#include "event.h"
#include "log.h"
#include "memory.h"
#include "visibility.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <threads.h>
#include <time.h>


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


// === Basic future lifecycle ===

UDIPE_NON_NULL_ARGS
void future_storage_allocate(future_storage_page_t** next) {
    void* const page = realtime_allocate(get_page_size());
    assert(page);

    memset(page, 0, get_page_size());
    future_storage_page_t* const new = (future_storage_page_t*)page;
    for (size_t i = 0; i < future_storage_page_len(); ++i) {
        // Must do this in addition to the memset because C11 specifies that
        // atomic_initialize() is the only valid way to initialize an atomic
        // variable. With a bit of luck, it should be optimized out.
        future_status_initialize(&(new->futures[i]),
                                 (future_status_t) { 0 });
        #ifdef __linux__
            // 0 is a valid fd number (stdin), so -1 is a better placeholder
            new->futures[i].status_sync.any = -1;
        #endif
    }

    new->next = *next;
    *next = new;
}

UDIPE_NON_NULL_ARGS
void future_storage_liberate_all(future_storage_page_t** first) {
    future_storage_page_t* next = *first;
    while (next) {
        void* const current = (void*)next;
        next = next->next;
        realtime_liberate(current, get_page_size());
    }
    *first = NULL;
}

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

// FIXME: Adjust implementations of the following once they have been redesigned
//        according to the design of the new context-local future cache.

/// Global future resource cache implementation
///
/// This cache must not be accessed directly. Use the global_cache() accessor
/// instead, which will take care of initializing it on first access and setting
/// it up for automated teardown on process exit.
static future_global_cache_t global_cache_impl;

/// Destroy the global future resource cache
///
/// This will run at process exit time and take care of liberating all contents
/// of the global resource cache.
///
/// This is actually not needed to ensure proper resource liberation, as the OS
/// liberates all resources at process exit time. But automated resource leak
/// analyzers like valgrind cannot tell the difference between a voluntary and
/// involuntary resource leak, and we would rather not break those useful tools.
static void global_cache_finalize(void) {
    // NOTE: This function is called at process exit time and therefore cannot
    //       use logging as no logger should be set up at this point.

    future_global_cache_t* global = &global_cache_impl;

    atomic_store_explicit(&global->event_cache_full, true, memory_order_relaxed);
    atomic_store_explicit(&global->event_cache_empty, true, memory_order_relaxed);
    #ifdef __linux__
        atomic_store_explicit(&global->epoll_event_cache_full,
                              true,
                              memory_order_relaxed);
        atomic_store_explicit(&global->epoll_event_cache_empty,
                              true,
                              memory_order_relaxed);
    #endif

    future_resource_cache_t* const cache = &global->cache;
    // TODO: Destroy the events and epolls_with_event cache
    // TODO: Destroy everything in the inner cache. Must not use the logger
    //       here. Same goes for the thread exit hook of the local cache.
    // TODO: Leave the global cache in such a state that if something goes wrong
    //       and the cache is accessed after this finalization, the client will
    //       be able to notice it.
    // TODO: Extract into a function that can also be called by the thread local
    //       cache teardown, only changing whether resources are transferred to
    //       the global cache or discarded.
    fprintf(stderr, "Not implemented yet!\n");
    exit(EXIT_FAILURE);
    future_cache_global_finalize(&cache->futures);

    mtx_destroy(&global->mutex);
}

/// Initialize the global future resource cache
///
/// This will run on the first call to global_cache() and take care of setting
/// up the global resource cache and making sure that it gets liberated by
/// global_cache_finalize() at process exit time.
///
/// This function must be called within the scope of with_logger().
static void global_cache_initialize(void) {
    future_global_cache_t* const global = &global_cache_impl;

    if (mtx_init(&global->mutex, mtx_plain) != thrd_success) {
        exit_after_c_error("Failed to initialize global future cache mutex");
    }

    // TODO: Extract cache setup into a function that can also be called by the
    //       thread local cache setup, only changing whether resources are
    //       transferred to the global cache or discarded.
    global->cache = (future_resource_cache_t){ 0 };
    future_resource_cache_t* const cache = &global->cache;
    cache->futures = future_cache_initialize(true);
    // TODO: Set up the events and epolls_with_event cache
    exit_with_error("Not implemented yet!");
    atexit(global_cache_finalize);

    atomic_init(&global->event_cache_full, false);
    atomic_init(&global->event_cache_empty, true);
    #ifdef __linux__
        atomic_init(&global->event_cache_full, false);
        atomic_init(&global->event_cache_empty, true);
    #endif
}

/// Access the global future resource cache
///
/// See \ref future_resource_cache_t for more information on the overall
/// structure of the future resource cache.
///
/// This function must be called within the scope of with_logger().
///
/// \returns a pointer to the global resource cache
UDIPE_NODISCARD
UDIPE_NON_NULL_RESULT
static future_global_cache_t* global_cache() {
    static once_flag global_cache_created = ONCE_FLAG_INIT;
    call_once(&global_cache_created, &global_cache_initialize);
    return &global_cache_impl;
}

/// Thread-local storage key used to retrieve a thread's future resource cache
///
/// The low-level `tss_t` API must be used here because any resources kept in
/// the thread-local cache must be spilled to the global cache on thread exit.
static tss_t thread_cache_key;

// TODO: Initializer and finalizers for thread_cache_key

/// Access the thread-local future resource cache
///
/// The cache will be set up the first time a particular thread allocates and
/// liberates a future. It will be spilled to the global cache on thread exit.
UDIPE_NODISCARD
UDIPE_NON_NULL_RESULT
static future_resource_cache_t* thread_local_cache() {
    // TODO: Lazily allocate thread_cache_key using a once flag, try to fetch
    //       the local cache pointer from it with tss_get(), if there is none
    //       create a cache and return it with tss_set(), eventually return a
    //       pointer to the initialized cache.
    exit_with_error("Not implemented yet!");
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* future_allocate(udipe_context_t* context,
                                future_type_t type) {
    // TODO: Implement (see function docs)
    // TODO: Check initial status, using swap in debug builds.
    exit_with_error("Not implemented yet!");
}

UDIPE_NON_NULL_ARGS
void future_liberate(udipe_future_t* /*future*/) {
    // TODO: Implement.
    // TODO: Check initial status, using swap in debug builds.
    // TODO: Set most values to zero-ish and the output fd to -1 before
    //       recycling the future into the thread-local pool.
    // TODO: See udipe_future_t field descriptions to see which inner file
    //       descriptors should be recycled and which should be
    //       destroyed/recreated.
    exit_with_error("Not implemented yet!");
}


// === Awaiting future results ===

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_PUBLIC
udipe_result_t udipe_finish(udipe_future_t* future) {
    with_logger(&future->context->logger, {
        tracef("Marking future %p as liberated...", future);
        // Synchronize-with the initial future state
        future_status_t latest_status =
            future_status_load(future, memory_order_acquire);
        do {
            future_status_debug_check(latest_status, true);
            assert(("Should not call udipe_finish() on a future that's still used",
                    (size_t)latest_status.downstream_count == (size_t)0));
            assert(("Should only call udipe_finish() once per future",
                    latest_status.available));

            future_status_t desired_status = latest_status;
            desired_status.available = false;
            future_status_debug_check(desired_status, true);
            const bool success = future_status_compare_exchange_weak(
                future,
                &latest_status,
                desired_status,
                // - Need an acquire barrier so that no post-liberation actions
                //   are taken before liberation is signaled to other threads.
                // - Need a release barrier so that all previous changes
                //   (especially other operations on the same future) become
                //   visible to other threads before future liberation starts
                //   from their perspective.
                // - No need for sequential consistency as we're not
                //   synchronizing across multiple futures/other objects.
                memory_order_acq_rel,
                // Synchronize-with concurrent future state changes.
                memory_order_acquire
            );

            if (success) {
                trace("Done updating the future status.");
                latest_status = desired_status;
                break;
            }
            future_status_debug_check(latest_status, true);
            trace("Failed because another thread updated the status word or "
                  "weak compare_exchange failed spuriously. Let's try again.");
        } while(true);

        if (latest_status.state == STATE_RESULT) {
            trace("Result was available from the start!");
        } else {
            trace("Waiting for the result to become available...");
            latest_status = future_wait(future,
                                        UDIPE_DURATION_MAX,
                                        DOWNSTREAM_COUNT_KEEP);
            future_status_debug_check(latest_status, true);
            assert(latest_status.state == STATE_RESULT);
        }

        trace("Collecting the end result...");
        udipe_result_t result = (udipe_result_t){ 0 };
        switch (latest_status.outcome) {
        case OUTCOME_SUCCESS:
        case OUTCOME_FAILURE_INTERNAL:
            trace("Future went far enough to produce a typed result");
            bool is_network = false;
            switch (latest_status.type) {
            case TYPE_NETWORK_CONNECT:
                result.type = UDIPE_CONNECT;
                is_network = true;
                break;
            case TYPE_NETWORK_DISCONNECT:
                result.type = UDIPE_DISCONNECT;
                is_network = true;
                break;
            case TYPE_NETWORK_SEND:
                result.type = UDIPE_SEND;
                is_network = true;
                break;
            case TYPE_NETWORK_RECV:
                result.type = UDIPE_RECV;
                is_network = true;
                break;
            case TYPE_CUSTOM:
                result.type = UDIPE_CUSTOM;
                result.payload.custom = future->specific.custom;
                break;
            case TYPE_JOIN:
                result.type = UDIPE_JOIN;
                // No payload for this result type, which cannot fail internally
                assert(latest_status.outcome != OUTCOME_FAILURE_INTERNAL);
                break;
            case TYPE_UNORDERED:
                result.type = UDIPE_UNORDERED;
                result.payload.unordered = future->specific.unordered.payload;
                break;
            case TYPE_TIMER_ONCE:
                result.type = UDIPE_TIMER_ONCE;
                // No payload for this result type, which cannot fail internally
                assert(latest_status.outcome != OUTCOME_FAILURE_INTERNAL);
                break;
            case TYPE_TIMER_REPEAT:
                result.type = UDIPE_TIMER_REPEAT;
                result.payload.timer_repeat = future->specific.timer_repeat.payload;
                break;
            case TYPE_INVALID:
            case NUM_TYPES:
                exit_with_error("Should never happen.");
            }
            if (is_network) {
                result.payload.network = future->specific.network;
            }
            break;
        case OUTCOME_FAILURE_DEPENDENCY:
            trace("Future failed because one of its dependencies has failed.");
            result.type = UDIPE_FAILURE_DEPENDENCY;
            break;
        case OUTCOME_FAILURE_CANCELED:
            trace("Future failed because it was canceled.");
            result.type = UDIPE_FAILURE_CANCELED;
            break;
        case OUTCOME_UNKNOWN:
        case NUM_OUTCOMES:
            exit_with_error("Should never happen");
        }

        trace("Liberating the future...");
        future_liberate(future);
        return result;
    });
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
DEFINE_PUBLIC
bool udipe_wait(udipe_future_t* future, udipe_duration_ns_t timeout) {
    with_logger(&future->context->logger, {
        if (timeout == UDIPE_DURATION_DEFAULT) timeout = UDIPE_DURATION_MAX;
        const future_status_t final_status =
            future_wait(future, timeout, DOWNSTREAM_COUNT_CYCLE);
        return final_status.state == STATE_RESULT;
    });
}


// === Other public methods ===

/*DEFINE_PUBLIC
UDIPE_NON_NULL_ARGS
void udipe_join(udipe_context_t* context,
                udipe_future_t* const futures[],
                size_t num_futures) {
    // TODO: Benchmark on various platforms, use e.g. a udipe_wait() loop if it
    //       is faster on selected platforms. On Windows, consider using
    //       WaitForMultipleObjects loop... you get the idea.
    udipe_future_t* future = udipe_start_join(context, futures, num_futures);
    assert(future);
    udipe_result_t result = udipe_finish(future);
    assert(result.type == UDIPE_JOIN);
}*/


#ifdef UDIPE_BUILD_TESTS

    void future_unit_tests() {
        info("Running future unit tests...");

        future_status_unit_tests();

        // TODO: Add more future tests as they come up. In particular, need to
        //       test all future_wait() variants + new status word
        //       manipulations.
    }

#endif  // UDIPE_BUILD_TESTS
