#include "sync_caches.h"

#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "../../event.h"
#include "../../log.h"

#ifdef __linux__
    #include "../epoll_event_pair.h"

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
    epoll_event_cache_t epoll_event_cache_initialize() {
        debug("Setting up new epollfd+eventfd cache...");
        epoll_event_cache_t result = { 0 };
        result.event_cache = event_cache_initialize();
        for (sync_cache_index_t i = 0; i < EVENT_CACHE_CAPACITY; ++i) {
            result.epolls[i] = FD_INVALID;
        }
        return result;
    }

    UDIPE_NODISCARD
    UDIPE_NON_NULL_ARGS
    epoll_event_pair_t epoll_event_cache_allocate(epoll_event_cache_t* cache) {
        debugf("Allocating an epollfd+eventfd pair from cache %p.", cache);
        sync_cache_index_t const target = cache->event_cache.latest;
        const fd_t epoll_candidate = cache->epolls[target];
        if (epoll_candidate == FD_INVALID) {
            debug("Must allocate a new epollfd+eventfd pair as the cache is empty.");
            return epoll_event_pair_initialize();
        } else {
            assert(epoll_candidate >= 0);
            cache->epolls[target] = FD_INVALID;
            assert(cache->event_cache.events[target] != EVENT_INVALID);
            return (epoll_event_pair_t){
                .epoll = epoll_candidate,
                .event = event_cache_allocate(&cache->event_cache)
            };
        }
    }

    UDIPE_NON_NULL_ARGS
    void epoll_event_cache_liberate(epoll_event_cache_t* cache,
                                    epoll_event_pair_t pair) {
        debugf("Discarding coupled epollfd %d + eventfd %d into cache %p.",
               pair.epoll, pair.event, cache);

        debug("Deferring eventfd liberation to inner event cache...");
        event_cache_liberate(&cache->event_cache, pair.event);

        sync_cache_index_t const target = cache->event_cache.latest;
        assert(target < EVENT_CACHE_CAPACITY);
        debugf("Discarding epollfd into matching epolls ring slot #%zu...",
               (size_t)target);
        if (cache->epolls[target] != FD_INVALID) {
            debug("epolls ring is full, must liberate oldest entry first.");
            close_virtual_fd(&cache->epolls[target]);
        }
        cache->epolls[target] = pair.epoll;
    }

    UDIPE_NON_NULL_ARGS
    void epoll_event_cache_finalize(epoll_event_cache_t* cache) {
        debugf("Destroying epollfd+eventfd cache %p...", cache);

        // Must not modify cache->event_cache.latest in order not to break
        // event_cache_finalize() later
        sync_cache_index_t target = cache->event_cache.latest;
        while (cache->epolls[target] != FD_INVALID) {
            assert(target < EVENT_CACHE_CAPACITY);
            const fd_t fd = cache->epolls[target];
            assert(fd >= 0);
            tracef("Liberating epollfd %d from entry %zu...",
                   fd, (size_t)target);
            close_virtual_fd(&cache->epolls[target]);
            sync_cache_decrement_index(&target, EVENT_CACHE_CAPACITY);
        }

        event_cache_finalize(&cache->event_cache);
    }

#endif  // __linux__
