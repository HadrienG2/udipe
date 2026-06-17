#include "sync_caches.h"

#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "../../event.h"
#include "../../log.h"

#ifdef __linux__
    #include "../inpoll_event_pair.h"

    #include "../../fd.h"
#endif


UDIPE_NODISCARD
event_cache_t event_cache_initialize() {
    debug("Setting up new event object cache...");
    event_cache_t result = { 0 };
    for (sync_cache_index_t i = 0; i < EVENT_CACHE_CAPACITY; ++i) {
        result.events[i] = EVENT_INVALID;
    }
    return result;
}

UDIPE_NON_NULL_ARGS
void event_cache_finalize(event_cache_t* cache) {
    debugf("Destroying event object cache %p...", cache);
    while (cache->events[cache->latest] != EVENT_INVALID) {
        assert(cache->latest < EVENT_CACHE_CAPACITY);
        tracef("Liberating occupied entry %zu...", (size_t)cache->latest);
        event_finalize(&cache->events[cache->latest]);
        sync_cache_decrement_index(&cache->latest, EVENT_CACHE_CAPACITY);
    }
    cache->latest = 0;
}


#ifdef __linux__

    UDIPE_NODISCARD
    inpoll_event_cache_t inpoll_event_cache_initialize() {
        debug("Setting up new inpoll+eventfd cache...");
        inpoll_event_cache_t result = { 0 };
        result.event_cache = event_cache_initialize();
        for (sync_cache_index_t i = 0; i < EVENT_CACHE_CAPACITY; ++i) {
            result.inpolls[i] = FD_INVALID;
        }
        return result;
    }

    UDIPE_NODISCARD
    UDIPE_NON_NULL_ARGS
    inpoll_event_pair_t
    inpoll_event_cache_allocate(inpoll_event_cache_t* cache) {
        tracef("Allocating an inpoll+eventfd pair from cache %p.", cache);
        sync_cache_index_t const target = cache->event_cache.latest;
        const fd_t inpoll_candidate = cache->inpolls[target];
        if (inpoll_candidate == FD_INVALID) {
            debug("Must allocate a new inpoll+eventfd pair as the cache is empty.");
            return inpoll_event_pair_initialize();
        } else {
            assert(inpoll_candidate >= 0);
            cache->inpolls[target] = FD_INVALID;
            assert(cache->event_cache.events[target] != EVENT_INVALID);
            return (inpoll_event_pair_t){
                .inpoll = inpoll_candidate,
                .event = event_cache_allocate(&cache->event_cache)
            };
        }
    }

    UDIPE_NON_NULL_ARGS
    void inpoll_event_cache_liberate(inpoll_event_cache_t* cache,
                                     inpoll_event_pair_t pair) {
        tracef("Discarding coupled inpoll %d + eventfd %d into cache %p.",
               pair.inpoll, pair.event, cache);

        trace("Offloading eventfd liberation to inner event cache...");
        event_cache_liberate(&cache->event_cache, pair.event);

        sync_cache_index_t const target = cache->event_cache.latest;
        assert(target < EVENT_CACHE_CAPACITY);
        tracef("Discarding inpoll into ring slot inpolls[%zu]...",
               (size_t)target);
        if (cache->inpolls[target] != FD_INVALID) {
            debug("inpoll ring is full, must liberate oldest entry first.");
            inpoll_finalize(&cache->inpolls[target]);
        }
        cache->inpolls[target] = pair.inpoll;
    }

    UDIPE_NON_NULL_ARGS
    void inpoll_event_cache_finalize(inpoll_event_cache_t* cache) {
        debugf("Destroying inpoll+eventfd cache %p...", cache);

        // Must not modify cache->event_cache.latest in order not to break
        // event_cache_finalize() later
        sync_cache_index_t target = cache->event_cache.latest;
        while (cache->inpolls[target] != FD_INVALID) {
            assert(target < EVENT_CACHE_CAPACITY);
            const fd_t fd = cache->inpolls[target];
            assert(fd >= 0);
            tracef("Liberating inpoll %d from entry %zu...",
                   fd, (size_t)target);
            inpoll_finalize(&cache->inpolls[target]);
            sync_cache_decrement_index(&target, EVENT_CACHE_CAPACITY);
        }

        event_cache_finalize(&cache->event_cache);
    }

#endif  // __linux__
