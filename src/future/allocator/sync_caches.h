#pragma once

//! \file
//! \brief Synchronization object cache
//!
//! This code module implements caches for OS synchronization objects, which are
//! special file descriptors on Linux (epollfd, eventfd) and NT synchronization
//! object handles on Windows (`HANDLE`s to Event etc).

#include "../../arch.h"

#include <stdint.h>


/// Index or length from a synchronization object cache
///
/// Since Linux's file descriptor limits force us into small sync object caches,
/// we may as well try to improve CPU cache locality with a small index type.
typedef uint8_t sync_cache_index_t;

/// Indices delineating a synchronization object ring
///
/// Synchronization object caches currently use a ring buffer layout, where this
/// pair of indices is used to implement the ring buffer logic.
typedef struct sync_ring_indices_s {
    /// Index of the newest entry, if any
    ///
    /// Moves forward (increments modulo ring size) when an object is inserted
    /// into the ring, moves backward when an object is taken from the ring.
    ///
    /// When `newest` is equal to `oldest`, the cache is empty and
    /// synchronization objects cannot be taken from it.
    ///
    /// When moving this index forward makes it reach `oldest`, the cache is
    /// beyond full and the oldest entry must be liberated. See `oldest`
    /// documentation for more about this.
    sync_cache_index_t newest;

    /// Index of the oldest event object, if any
    ///
    /// As we handle circular ambiguity by taking `oldest == newest` to mean
    /// that the ring is empty, we must do something when `newest` reaches
    /// `oldest` through entry insertion.
    ///
    /// In this case, we liberate the oldest cache entry then move `oldest`
    /// forward, thus going back to a valid `oldest != newest` non-empty state.
    sync_cache_index_t oldest;
} sync_ring_indices_t;

// TODO: Add a couple of methods implementing the above logic

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
/// This cache follows the ring buffer logic described in the documentation of
/// \ref sync_ring_indices_t. It is used to keep around event objects that are
/// used in isolation, as in the implementation of network and custom futures.
typedef struct event_cache_s {
    /// Storage for unused unsignaled event objects
    ///
    /// To make resource management bugs easier to detect, invalid entries
    /// should be set to \ref EVENT_INVALID, at least in debug builds.
    event_t events[EVENT_CACHE_CAPACITY];

    /// Indices of the newest and oldest event objects, if any
    ///
    /// See \ref sync_ring_indices_t for more information.
    sync_ring_indices_t indices;
} event_cache_t;

#ifdef __linux__
    /// epollfd+eventfd cache capacity
    ///
    /// Tune this capacity up if allocating epollfds and binding eventfds to
    /// them becomes a bottleneck, following the same advice as for \ref
    /// EVENT_CACHE_CAPACITY.
    #define EPOLL_EVENT_CACHE_CAPACITY  ((sync_cache_index_t)4)

    /// Cache for epollfds with pre-attached eventfds
    ///
    /// Works just like \ref event_cache_t except instead of managing eventfds
    /// it manages (eventfd, epollfd) pairs where the epollfd is pre-attached to
    /// the eventfd with an `epoll_data` of `UINT64_MAX` at the exclusion of any
    /// other file descriptor.
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
        /// For each valid index `i`, `epolls[i]` should be an epollfd that is
        /// attached to `events[i]` and no other file descriptor.
        ///
        /// To make resource management bugs easier to detect, invalid entries
        /// should be set to -1, at least in debug builds.
        int epolls[EPOLL_EVENT_CACHE_CAPACITY];

        /// Storage for event objects, which are eventfds on Linux
        ///
        /// For each valid index `i`, `events[i]` should be an eventfd in an
        /// unsignaled state.
        ///
        /// To make resource management bugs easier to detect, invalid entries
        /// should be set to \ref EVENT_INVALID, at least in debug builds.
        event_t events[EPOLL_EVENT_CACHE_CAPACITY];

        /// Indices of the newest and oldest epollfd+eventfd pairs, if any
        ///
        /// See \ref sync_ring_indices_t for more information.
        sync_ring_indices_t indices;
    } epoll_event_cache_t;
#endif


// TODO: Unit tests
