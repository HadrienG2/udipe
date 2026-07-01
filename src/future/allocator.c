#include "allocator.h"

#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "allocator/pointer_cache.h"
#include "allocator/storage_page.h"
#include "allocator/sync_caches.h"
#include "allocator/thread_cache.h"
#include "status.h"
#include "status_ops.h"
#include "type.h"
#ifdef __linux__
    #include "latched_inpoll.h"
#endif

#include "../context.h"
#include "../error.h"
#include "../future.h"
#include "../log.h"
#include "../refcounted_tss.h"
#include "../timer.h"
#ifdef __linux__
    #include "../inpoll.h"
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>


/// future_thread_cache_initialize() wrapper that has the signature expected by
/// `refcounted_tss_ctor_t`.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
static void* future_thread_cache_constructor(void* erased_context) {
    debugf("Setting up the thread-local cache for context %p...",
           erased_context);
    udipe_context_t* const context = (udipe_context_t*)erased_context;
    return (void*)future_thread_cache_initialize(context);
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* future_allocate(udipe_context_t* context,
                                future_type_t type) {
    tracef("Accessing the thread-local cache for context %p...",
           (void*)context);
    future_thread_cache_t* const thread_cache = future_thread_cache(context);

    trace("Allocating a future and attaching it to the context...");
    udipe_future_t* const future = future_allocate_uninitialized(
        thread_cache,
        &context->future_global_cache
    );
    #ifndef NDEBUG
        future_status_debug_check(future_status_load(future,
                                                     memory_order_relaxed),
                                  false);
    #endif
    future->context = context;

    trace("Configuring the future's type...");
    future_status_t initial_status = { 0 };
    initial_status.type = type;
    #ifndef NDEBUG
        const future_status_t prev_status = future_status_exchange(
            future,
            initial_status,
            memory_order_relaxed
        );
        const uint32_t prev_status_word = (future_status_word_t){
            .as_bitfield = prev_status
        }.as_word;
        assert(prev_status_word == (uint32_t)0);
    #else
        future_status_store(future, initial_status, memory_order_relaxed);
    #endif

    trace("Setting up synchronization resources...");
    future_sync_initialize(future, thread_cache);
    return future;
}

UDIPE_NON_NULL_ARGS
void future_liberate(udipe_future_t* future) {
    // TODO: Implement. Must use trace logging on the hot path.
    // TODO: Modularize once done implementing, or early when it's obvious

    trace("Detaching from upstream futures (if any)...");
    future_upstream_detach(future);

    trace("Tearing down synchronization resources...");
    future_thread_cache_t* const thread_cache = future_thread_cache(context);
    future_sync_finalize(future, thread_cache);

    trace("Reading and resetting status...");
    #ifndef NDEBUG
        // Use exchange in debug builds as a slightly more sure-fire way of
        // detecting incorrect concurrent usage after finish.
        const future_status_t status = future_status_exchange(
            future,
            (future_status_t) { 0 },
            memory_order_relaxed
        );
    #else
        const future_status_t status = future_status_load(future,
                                                          memory_order_relaxed);
        future_status_store(future,
                            (future_status_t){ 0 },
                            memory_order_relaxed);
    #endif
    // TODO: Check the status for correctness
    // TODO: Detach from the context and deallocate
    // TODO: Set most values to zero-ish and the output fd to -1 before
    //       recycling the future into the thread-local pool.
    // TODO: See udipe_future_t field descriptions to see which inner file
    //       descriptors should be recycled and which should be
    //       destroyed/recreated.
    exit_with_error("Not implemented yet!");
}

UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
future_thread_cache_t* future_thread_cache(udipe_context_t* context) {
    tracef("Accessing the thread-local cache for context %p, "
           "creating it on first allocation...",
           (void*)context);
    return (future_thread_cache_t*)refcounted_tss_acquire(
        &context->future_local_cache_key,
        future_thread_cache_constructor,
        (void*)context
    );
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t*
future_allocate_uninitialized(future_thread_cache_t* thread_cache,
                              future_context_cache_t* context_cache) {
    trace("Trying to allocate a future from the thread-local cache...");
    udipe_future_t* future =
        future_pointer_cache_allocate_local(&thread_cache->futures);
    if (future) return future;

    debug("Thread-local cache is empty, looking up "
          "the context-global cache...");
    exit_on_thread_error(mtx_lock(&context_cache->mutex),
                         "Failed to lock the context cache's mutex.");
    future_pointer_page_t* const futures =
        future_pointer_cache_extract_futures(&context_cache->futures);
    if (futures) {
        debug("Extracted a page with futures from the context cache, "
              "time to inject it into the thread-local cache.");

        debug("First we make room by spilling a page of NULLs...");
        future_pointer_page_t* const empty =
            future_pointer_cache_obtain_empty(&thread_cache->futures);
        future_pointer_cache_insert_empty(&context_cache->futures,
                                          empty);

        debug("...then we insert the previously extracted futures.");
        future_pointer_cache_insert_futures(&thread_cache->futures,
                                            futures);
    } else {
        debug("Context cache is empty too, must allocate more futures...");
        future_storage_allocate(&context_cache->first_storage_page);

        debug("...then inject them into the thread-local cache.");
        future_pointer_cache_refill_local(&thread_cache->futures,
                                          context_cache->first_storage_page);
    }
    exit_on_thread_error(mtx_unlock(&context_cache->mutex),
                         "Failed to unlock the context cache's mutex.");

    debug("Retrying local allocation, which should now succeed...");
    future = future_pointer_cache_allocate_local(&thread_cache->futures);
    assert(future);
    return future;
}

UDIPE_NON_NULL_ARGS
void future_sync_initialize(udipe_future_t* future,
                            future_thread_cache_t* thread_cache) {
    trace("Setting up a future's synchronization resources...");
    const future_status_t status = future_status_load(future,
                                                      memory_order_relaxed);
    #ifdef __linux__
        inpoll_with_latch_t latched;
        inpoll_attach_result_t attach_result;
    #endif
    switch (status.type) {
    case TYPE_NETWORK_CONNECT:  // aliases TYPE_NETWORK_START
    case TYPE_NETWORK_DISCONNECT:
    case TYPE_NETWORK_SEND:
    case TYPE_NETWORK_RECV:
    case TYPE_CUSTOM:  // aliases TYPE_NETWORK_END
        trace("Allocating the output event object...");
        future->status_sync.event = event_cache_allocate(&thread_cache->events);
        break;
    case TYPE_TIMER_ONCE:
        trace("Allocating the output timer object...");
        future->status_sync.timer_once = timer_initialize();
        break;
    #ifdef __linux__
        case TYPE_JOIN:
            trace("Allocating the output inpoll+eventfd pair...");
            latched = latched_inpoll_cache_allocate(
                &thread_cache->latched_inpolls
            );
            future->specific.join.inpoll_latch = latched.latch;
            future->status_sync.latched_inpoll = latched.inpoll;
            break;
        case TYPE_UNORDERED:
            trace("Allocating the upstream inpoll...");
            future->specific.unordered.upstream_inpoll = inpoll_initialize();

            trace("Allocating the output inpoll+eventfd pair...");
            latched = latched_inpoll_cache_allocate(
                &thread_cache->latched_inpolls
            );
            future->specific.unordered.inpoll_latch = latched.latch;
            future->status_sync.latched_inpoll = latched.inpoll;

            trace("Attaching the upstream inpoll to the output inpoll...");
            attach_result =
                inpoll_attach(future->status_sync.latched_inpoll,
                              future->specific.unordered.upstream_inpoll,
                              INPOLL_SINGLE_UPSTREAM_ID);
            switch (attach_result) {
            case INPOLL_ATTACH_SUCCESS:
                break;
            case INPOLL_ATTACH_TOO_NESTED:  // Cannot happen with a blank inpoll
            case INPOLL_ATTACH_REDUNDANT:  // Cannot happen for the first fd
                exit_after_c_error("This error is not expected to happen!");
            }
            break;
        case TYPE_TIMER_REPEAT:
            trace("Allocating the upstream timerfd...");
            future->specific.timer_repeat.timerfd = timer_initialize();

            trace("Allocating the output inpoll+eventfd pair...");
            latched = latched_inpoll_cache_allocate(
                &thread_cache->latched_inpolls
            );
            future->specific.timer_repeat.inpoll_latch = latched.latch;
            future->status_sync.latched_inpoll = latched.inpoll;

            trace("Attaching the upstream timerfd to the output inpoll...");
            attach_result =
                inpoll_attach(future->status_sync.latched_inpoll,
                              future->specific.timer_repeat.timerfd,
                              INPOLL_SINGLE_UPSTREAM_ID);
            switch (attach_result) {
            case INPOLL_ATTACH_SUCCESS:
                break;
            case INPOLL_ATTACH_TOO_NESTED:  // Cannot happen with a timerfd
            case INPOLL_ATTACH_REDUNDANT:  // Cannot happen for the first fd
                exit_after_c_error("This error is not expected to happen!");
            }
            break;
    #else
        // TODO: Add windows versions
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
    case TYPE_INVALID:
    case NUM_TYPES:
        exit_with_error("These cases should not be encountered.");
    }
}

UDIPE_NON_NULL_ARGS
void future_upstream_detach(udipe_future_t* future) {
    trace("Determining if this future type can have an upstream...");
    const future_status_t status = future_status_load(future,
                                                      memory_order_relaxed);
    collective_upstream_t* upstream = NULL;
    switch (status.type) {
    case TYPE_NETWORK_CONNECT:  // aliases TYPE_NETWORK_START
    case TYPE_NETWORK_DISCONNECT:
    case TYPE_NETWORK_SEND:
    case TYPE_NETWORK_RECV:
        // TODO: Implement once network futures are ready, beware that you
        //       cannot break to the same code path as join/unordered
        exit_with_error("Not implemented yet!");
        break;
    case TYPE_JOIN:
        trace("Extracting upstream from join_state...");
        upstream = &future->specific.join.upstream;
        break;
    case TYPE_UNORDERED:
        trace("Extracting upstream from unordered_state...");
        upstream = &future->specific.unordered.upstream;
        break;
    case TYPE_CUSTOM:  // aliases TYPE_NETWORK_END
    case TYPE_TIMER_ONCE:
    case TYPE_TIMER_REPEAT:
        trace("No upstream to be detached for this future type.");
        break;
    case TYPE_INVALID:
    case NUM_TYPES:
        exit_with_error("These cases should not be encountered.");
    }

    if (upstream) {
        assert(upstream->array);
        assert(upstream->length >= 1);
        assert(upstream->remaining <= upstream->length);

        trace("Detaching futures from collective upstream...");
        for (size_t i = 0; i < (size_t)upstream->length; ++i) {
            udipe_future_t* upstream_future = upstream->array[i];
            tracef("- Detaching future #%zu (%p)...", i, upstream_future);
            future_downstream_count_dec(upstream_future,
                                        memory_order_relaxed);
        }

        trace("Invalidating remainder of collective upstream...");
        upstream->array = NULL;
        upstream->length = 0;
        upstream->remaining = 0;
    }
}
