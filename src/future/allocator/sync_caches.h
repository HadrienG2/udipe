#pragma once

//! \file
//! \brief Synchronization object cache
//!
//! This code module implements caches for OS synchronization objects, which are
//! special file descriptors on Linux (epollfd, eventfd) and NT synchronization
//! object handles on Windows (`HANDLE`s to Event etc).

#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "../../event.h"
#include "../../log.h"

#include <stdint.h>

#ifdef __linux__

    #include "../epoll_event_pair.h"

    #include "../../fd.h"

#endif


/// \name Common ring buffer logic
/// \{

/// Index or length from a synchronization object cache
///
/// Since Linux's file descriptor limits force us into small sync object caches,
/// we may as well try to improve CPU cache locality with a small index type.
typedef uint8_t sync_cache_index_t;

/// Move \ref sync_cache_index_t forward across a ring buffer
///
/// \param index is the index to be moved forward
/// \param capacity is the capacity of the underlying ring buffer
UDIPE_NON_NULL_ARGS
static inline
void sync_cache_increment_index(sync_cache_index_t* index,
                                sync_cache_index_t capacity) {
    *index = (*index + 1) % capacity;
}

/// Move \ref sync_cache_index_t backward across a ring buffer
///
/// \param index is the index to be moved backward
/// \param capacity is the capacity of the underlying ring buffer
UDIPE_NON_NULL_ARGS
static inline
void sync_cache_decrement_index(sync_cache_index_t* index,
                                sync_cache_index_t capacity) {
    // Will be optimized to fast dec-modulo if capacity is a power of two
    if (*index >= 1) {
        *index -= 1;
    } else {
        *index = capacity - 1;
    }
}

/// \}


/// \name Event cache
/// \{

/// Event cache capacity
///
/// Can be increased if profiling proves that this is worthwhile, but beware the
/// tight default per-process file descriptor limit on Linux. Sysadmins may need
/// to adjust the kernel configuration before a larger cache capacity becomes
/// viable in apps with lots of threads.
///
/// If you do increase this capacity, keep it a power of two, as otherwise the
/// performance of the ring buffer integer arithmetic performed by the event
/// cache may decrease quite dramatically.
#define EVENT_CACHE_CAPACITY  ((sync_cache_index_t)4)

/// Cache for unallocated and unsignaled event objects
///
/// This cache is used to keep around event objects that are used in isolation,
/// as in the implementation of network and custom futures.
///
/// It follows a ring buffer logic to ensure that the most recently discarded
/// event is reused first in order to optimize cache locality.
typedef struct event_cache_s {
    /// Storage for unused unsignaled event objects
    ///
    /// Invalid entries are set to \ref EVENT_INVALID.
    event_t events[EVENT_CACHE_CAPACITY];

    /// Index of the latest entry, if any
    ///
    /// If the associated entry of `events` is \ref EVENT_INVALID, then there is
    /// no event object available for reuse in this cache.
    sync_cache_index_t latest;
} event_cache_t;

/// Set up an event object cache
///
/// This function must be called in the scope of with_logger().
///
/// \returns an event cache that must be later liberated with
///          event_cache_finalize().
UDIPE_NODISCARD
event_cache_t event_cache_initialize();

/// Try to reuse an event object from the specified cache, allocating a fresh
/// one on failure.
///
/// This function must be called in the scope of with_logger().
///
/// \param cache must be an event cache that was initialized with
///              event_cache_initialize() and wasn't finalized with
///              event_cache_finalize() yet.
///
/// \returns an event object in the unsignaled state.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
static inline
event_t event_cache_allocate(event_cache_t* cache) {
    debugf("Event object requested from cache %p.", cache);
    const event_t candidate = cache->events[cache->latest];
    if (candidate == EVENT_INVALID) {
        debug("Must allocate a new event object as the cache is empty.");
        return event_initialize(false);
    } else {
        debugf("Picked an event object from the latest entry (#%zu).",
               (size_t)(cache->latest));
        cache->events[cache->latest] = EVENT_INVALID;
        sync_cache_decrement_index(&cache->latest, EVENT_CACHE_CAPACITY);
        return candidate;
    }
}

/// Liberate an event object into the specified cache
///
/// Unlike event_finalize(), this creates opportunities for event object reuse.
///
/// This function must be called in the scope of with_logger().
///
/// The liberated event object must be in the unsignaled state.
///
/// \param cache must be an event cache that was initialized with
///              event_cache_initialize() and wasn't finalized with
///              event_cache_finalize() yet.
/// \param event must be an event object in the unsignaled state.
UDIPE_NON_NULL_ARGS
static inline
void event_cache_liberate(event_cache_t* cache, event_t event) {
    debugf("Discarding event object into cache %p.", cache);
    sync_cache_increment_index(&cache->latest, EVENT_CACHE_CAPACITY);
    debugf("Will put it into the next entry (#%zu).",
           (size_t)(cache->latest));
    if (cache->events[cache->latest] != EVENT_INVALID) {
        debug("Cache is full, must liberate oldest entry first.");
        event_finalize(&cache->events[cache->latest]);
    }
    cache->events[cache->latest] = event;
}

/// Destroy event object cache
///
/// This function must be called in the scope of with_logger().
///
/// \param cache must be an event cache that was initialized with
///              event_cache_initialize() and wasn't finalized with
///              event_cache_finalize() yet. It cannot be used again after
///              calling this function.
UDIPE_NON_NULL_ARGS
void event_cache_finalize(event_cache_t* cache);

/// \}


#ifdef __linux__

    /// \name epollfd+eventfd cache (Linux-only)
    /// \{

    /// Cache for epollfds with pre-attached eventfds
    ///
    /// This is an extension of \ref event_cache_t that manages (eventfd,
    /// epollfd) pairs where the epollfd is pre-attached to the eventfd with an
    /// `epoll_data` of `UINT64_MAX`, at the exclusion of any other file
    /// descriptor.
    ///
    /// Resetting an epollfd involves detaching it from every other fd which
    /// it's currently bound to, and that can be a lot more work than liberating
    /// it and allocating a new one. Therefore epollfds are only reset and
    /// cached in situations where they are attached to a tightly bounded amount
    /// of other fds, a.g. a single other fd for \ref TYPE_UNORDERED or \ref
    /// TYPE_TIMER_REPEAT.
    typedef struct epoll_event_cache_s {
        /// Storage for epollfds
        ///
        /// For each valid index `i` from `event_cache`, the associated
        /// `epolls[i]` is an epollfd that is attached to
        /// `event_cache.events[i]` as described in the struct documentation.
        ///
        /// For each invalid index `i` of `event_cache` that is set to \ref
        /// EVENT_INVALID, the associated `epolls[i]` is set to \ref FD_INVALID.
        fd_t epolls[EVENT_CACHE_CAPACITY];

        /// Event cache which this builds upon
        ///
        /// Must not be manipulated directly, or `epolls` will go out of sync.
        /// Use the dedicated `epoll_event_cache_` functions instead.
        event_cache_t event_cache;
    } epoll_event_cache_t;

    /// Set up an epollfd+eventfd cache
    ///
    /// This function must be called in the scope of with_logger().
    ///
    /// \returns an epollfd+eventfd cache that must be later liberated with
    ///          epoll_event_cache_finalize().
    UDIPE_NODISCARD
    epoll_event_cache_t epoll_event_cache_initialize();

    /// Try to reuse an epollfd+eventfd pair from the specified cache,
    /// allocating a fresh pair on failure.
    ///
    /// This function must be called in the scope of with_logger().
    ///
    /// \param cache must be an event cache that was initialized with
    ///              event_cache_initialize() and wasn't finalized with
    ///              event_cache_finalize() yet.
    ///
    /// \returns an epollfd+eventfd pair in the state described by the
    ///          documentation of \ref epoll_event_pair_t.
    UDIPE_NODISCARD
    UDIPE_NON_NULL_ARGS
    epoll_event_pair_t epoll_event_cache_allocate(epoll_event_cache_t* cache);

    /// Liberate an epollfd+eventfd pair into the specified cache
    ///
    /// This function must be called in the scope of with_logger().
    ///
    /// \param cache must be an event cache that was initialized with
    ///              event_cache_initialize() and wasn't finalized with
    ///              event_cache_finalize() yet.
    /// \param pair must be an epollfd+eventfd pair in the state described by
    ///             the documentation of \ref epoll_event_pair_t.
    UDIPE_NON_NULL_ARGS
    void epoll_event_cache_liberate(epoll_event_cache_t* cache,
                                    epoll_event_pair_t pair);

    /// Destroy an epollfd+eventfd cache
    ///
    /// This function must be called in the scope of with_logger().
    ///
    /// \param cache must be an event cache that was initialized with
    ///              event_cache_initialize() and wasn't finalized with
    ///              event_cache_finalize() yet. It cannot be used again after
    ///              calling this function.
    UDIPE_NON_NULL_ARGS
    void epoll_event_cache_finalize(epoll_event_cache_t* cache);

    /// \}

#endif  // __linux__


// TODO: Unit tests
