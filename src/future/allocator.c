#include "allocator.h"

#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "allocator/pointer_cache.h"
#include "allocator/storage_page.h"
#include "allocator/sync_caches.h"
#include "allocator/thread_cache.h"
#include "epoll_event_pair.h"
#include "status.h"
#include "status_ops.h"
#include "type.h"

#include "../context.h"
#include "../error.h"
#include "../future.h"
#include "../log.h"
#include "../refcounted_tss.h"

#include <stdatomic.h>
#include <stdbool.h>


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
    tracef("Accessing the thread-local cache for context %p, "
           "creating it if this is the first allocation from this thread...",
           (void*)context);
    future_thread_cache_t* const thread_cache =
        (future_thread_cache_t*)refcounted_tss_acquire(
            &context->future_local_cache_key,
            future_thread_cache_constructor,
            (void*)context
        );

    trace("Allocating a future...");
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

    trace("Configuring initial future status...");
    future_status_t initial_status = { 0 };
    initial_status.type = type;
    future_status_store(future, initial_status, memory_order_relaxed);

    trace("Setting up type-specific file descriptors...");
    // TODO: Extract this into a utility function
    epoll_event_pair_t epoll_event;
    switch (type) {
    case TYPE_NETWORK_CONNECT:  // aliases TYPE_NETWORK_START
    case TYPE_NETWORK_DISCONNECT:
    case TYPE_NETWORK_SEND:
    case TYPE_NETWORK_RECV:
    case TYPE_CUSTOM:  // aliases TYPE_NETWORK_END
        future->status_sync.event = event_cache_allocate(&thread_cache->events);
        break;
    #ifdef __linux__
        case TYPE_JOIN:
            epoll_event = epoll_event_cache_allocate(
                &thread_cache->epolls_with_events
            );
            future->specific.join.epoll_latch = epoll_event.event;
            future->status_sync.latched_epoll = epoll_event.epoll;
            break;
        case TYPE_UNORDERED:
            // TODO: Allocate specific.unordered.upstream_epollfd, extracting
            //       the epollfd allocation code from epoll_event_pair.c
            epoll_event = epoll_event_cache_allocate(
                &thread_cache->epolls_with_events
            );
            future->specific.unordered.epoll_latch = epoll_event.event;
            // TODO: Attach upstream_epollfd to epoll_event.epoll, extracting
            //       the epollfd binding code from epoll_event_pair.c
            future->status_sync.latched_epoll = epoll_event.epoll;
            break;
        case TYPE_TIMER_REPEAT:
            // TODO: Allocate specific.timer_repeat.timerfd, sharing code with
            //       the TIMER_ONCE path
            epoll_event = epoll_event_cache_allocate(
                &thread_cache->epolls_with_events
            );
            future->specific.timer_repeat.epoll_latch = epoll_event.event;
            // TODO: Attach timerfd to epoll_event.epoll, extracting the epollfd
            //       binding code from epoll_event_pair.c
            future->status_sync.latched_epoll = epoll_event.epoll;
            break;
        case TYPE_TIMER_ONCE:
            // TODO: Allocate future->status_sync.timer, sharing code with
            //       the TIMER_REPEAT path.
            break;
    #else
        // TODO: Add windows versions
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
    case TYPE_INVALID:
    case NUM_TYPES:
        exit_with_error("These cases should not be encountered.");
    }

    // TODO: Check what comes before then remove this
    exit_with_error("Not implemented yet!");
    return future;
}

UDIPE_NON_NULL_ARGS
void future_liberate(udipe_future_t* /*future*/) {
    // TODO: Implement. Must use trace logging on the hot path.
    // TODO: Check initial status, using swap in debug builds.
    // TODO: Set most values to zero-ish and the output fd to -1 before
    //       recycling the future into the thread-local pool.
    // TODO: See udipe_future_t field descriptions to see which inner file
    //       descriptors should be recycled and which should be
    //       destroyed/recreated.
    exit_with_error("Not implemented yet!");
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
