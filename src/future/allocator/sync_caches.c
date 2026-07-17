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
    LOGGED_FUNCTION_START_NO_PARAMS
        event_cache_t result = { 0 };
        for (sync_cache_index_t i = 0; i < EVENT_CACHE_CAPACITY; ++i) {
            // If you change this, change latched_inpoll_cache_initialize() too
            result.events[i] = EVENT_INVALID;
        }
        return result;
    LOGGED_FUNCTION_END
}

UDIPE_NON_NULL_ARGS
void event_cache_finalize(event_cache_t* cache) {
    LOGGED_FUNCTION_START("%p", cache)
        debugf("Liberating all events from cache %p...", cache);
        while (cache->events[cache->latest] != EVENT_INVALID) {
            assert(cache->latest < EVENT_CACHE_CAPACITY);
            tracef("- Liberating occupied entry events[%zu]...", (size_t)cache->latest);
            event_finalize(&cache->events[cache->latest]);
            sync_cache_decrement_index(&cache->latest, EVENT_CACHE_CAPACITY);
        }

        debug("Poisoning the latest event index...");
        cache->latest = 0;
    LOGGED_FUNCTION_END
}


#ifdef __linux__

    UDIPE_NODISCARD
    latched_inpoll_cache_t latched_inpoll_cache_initialize() {
        LOGGED_FUNCTION_START_NO_PARAMS
            latched_inpoll_cache_t result = { 0 };
            result.event_cache = event_cache_initialize();
            for (sync_cache_index_t i = 0; i < EVENT_CACHE_CAPACITY; ++i) {
                // If you change this, change event_cache_initialize() too
                result.inpolls[i] = FD_INVALID;
            }
            return result;
        LOGGED_FUNCTION_END
    }

    UDIPE_NODISCARD
    UDIPE_NON_NULL_ARGS
    inpoll_with_latch_t
    latched_inpoll_cache_allocate(latched_inpoll_cache_t* cache) {
        LOGGED_FUNCTION_START("%p", cache)
            sync_cache_index_t const target = cache->event_cache.latest;
            debugf("Determining if there's a cached inpoll+eventfd at "
                   "latest index #%zu of cache %p...",
                   (size_t)target, cache);

            const fd_t inpoll_candidate = cache->inpolls[target];
            inpoll_with_latch_t result;
            if (inpoll_candidate == FD_INVALID) {
                debug("Must allocate a new inpoll+eventfd pair, "
                      "cache is empty.");
                result = latched_inpoll_initialize();
            } else {
                debug("Grabbing an inpoll+eventfd pair from cache...");
                assert(inpoll_candidate >= 0);
                cache->inpolls[target] = FD_INVALID;
                assert(cache->event_cache.events[target] != EVENT_INVALID);
                result = (inpoll_with_latch_t){
                    .inpoll = inpoll_candidate,
                    .latch = event_cache_allocate(&cache->event_cache)
                };
            }

            debugf("Will emit inpoll %d with latch eventfd %d.",
                   result.inpoll, result.latch);
            return result;
        LOGGED_FUNCTION_END
    }

    UDIPE_NON_NULL_ARGS
    void latched_inpoll_cache_liberate(latched_inpoll_cache_t* cache,
                                       inpoll_with_latch_t latched) {
        LOGGED_FUNCTION_START("%p, { .inpoll = %d, .latch = %d }",
                              cache,
                              latched.inpoll,
                              latched.latch)
            debugf(
                "Offloading liberation of eventfd %d to the event cache...",
                latched.latch
            );
            event_cache_liberate(&cache->event_cache, &latched.latch);

            sync_cache_index_t const target = cache->event_cache.latest;
            assert(target < EVENT_CACHE_CAPACITY);
            debugf("Discarding inpoll %d into ring slot inpolls[%zu]...",
                   latched.inpoll, (size_t)target);
            const fd_t target_fd = cache->inpolls[target];
            if (target_fd != FD_INVALID) {
                debugf(
                    "inpoll ring is full, must liberate oldest inpoll %d first.",
                    target_fd
                );
                inpoll_finalize(&cache->inpolls[target]);
            }
            cache->inpolls[target] = latched.inpoll;
        LOGGED_FUNCTION_END
    }

    UDIPE_NON_NULL_ARGS
    void latched_inpoll_cache_finalize(latched_inpoll_cache_t* cache) {
        LOGGED_FUNCTION_START("%p", cache)
            debugf("Liberating inpolls from inpoll+event cache %p...", cache);
            // Must not modify cache->event_cache.latest in order not to break
            // event_cache_finalize() later
            sync_cache_index_t target = cache->event_cache.latest;
            while (cache->inpolls[target] != FD_INVALID) {
                assert(target < EVENT_CACHE_CAPACITY);
                const fd_t fd = cache->inpolls[target];
                assert(fd >= 0);
                tracef("- Liberating inpoll %d from entry inpolls[%zu]...",
                       fd, (size_t)target);
                inpoll_finalize(&cache->inpolls[target]);
                sync_cache_decrement_index(&target, EVENT_CACHE_CAPACITY);
            }

            debug("Offloading the rest to the event cache destructor...");
            event_cache_finalize(&cache->event_cache);
        LOGGED_FUNCTION_END
    }

#endif  // __linux__
