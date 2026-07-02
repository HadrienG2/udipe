#include "sync_caches.h"

#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "../../event.h"
#include "../../log.h"

#ifdef __linux__
    #include "../latched_inpoll.h"

    #include "../../fd.h"
#endif


UDIPE_NODISCARD
event_cache_t event_cache_initialize() {
    debug("Setting up new event object cache...");
    event_cache_t result = { 0 };
    for (sync_cache_index_t i = 0; i < EVENT_CACHE_CAPACITY; ++i) {
        // If you change this, change latched_inpoll_cache_initialize() too
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
    latched_inpoll_cache_t latched_inpoll_cache_initialize() {
        debug("Setting up new inpoll+eventfd cache...");
        latched_inpoll_cache_t result = { 0 };
        result.event_cache = event_cache_initialize();
        for (sync_cache_index_t i = 0; i < EVENT_CACHE_CAPACITY; ++i) {
            // If you change this, change event_cache_initialize() too
            result.inpolls[i] = FD_INVALID;
        }
        return result;
    }

    UDIPE_NODISCARD
    UDIPE_NON_NULL_ARGS
    inpoll_with_latch_t
    latched_inpoll_cache_allocate(latched_inpoll_cache_t* cache) {
        tracef("Allocating an inpoll+eventfd pair from cache %p.", cache);
        sync_cache_index_t const target = cache->event_cache.latest;
        const fd_t inpoll_candidate = cache->inpolls[target];
        if (inpoll_candidate == FD_INVALID) {
            debug("Must allocate a new inpoll+eventfd pair as the cache is empty.");
            return latched_inpoll_initialize();
        } else {
            assert(inpoll_candidate >= 0);
            cache->inpolls[target] = FD_INVALID;
            assert(cache->event_cache.events[target] != EVENT_INVALID);
            return (inpoll_with_latch_t){
                .inpoll = inpoll_candidate,
                .latch = event_cache_allocate(&cache->event_cache)
            };
        }
    }

    UDIPE_NON_NULL_ARGS
    void latched_inpoll_cache_liberate(latched_inpoll_cache_t* cache,
                                       inpoll_with_latch_t latched) {
        tracef("Discarding coupled inpoll %d + eventfd %d into cache %p.",
               latched.inpoll, latched.latch, cache);

        trace("Offloading eventfd liberation to inner event cache...");
        event_cache_liberate(&cache->event_cache, &latched.latch);

        sync_cache_index_t const target = cache->event_cache.latest;
        assert(target < EVENT_CACHE_CAPACITY);
        tracef("Discarding inpoll into ring slot inpolls[%zu]...",
               (size_t)target);
        if (cache->inpolls[target] != FD_INVALID) {
            debug("inpoll ring is full, must liberate oldest entry first.");
            inpoll_finalize(&cache->inpolls[target]);
        }
        cache->inpolls[target] = latched.inpoll;
    }

    UDIPE_NON_NULL_ARGS
    void latched_inpoll_cache_finalize(latched_inpoll_cache_t* cache) {
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
