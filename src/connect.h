#pragma once

//! \file
//! \brief \ref udipe_connect_options_t allocation mechanism
//!
//! The \ref udipe_connect_options_t type is very large compared to other
//! command option types, so we do not want to store it inline inside of a \ref
//! command_t. Instead, we have a pool of these within the \ref udipe_context_t
//! that we dynamically allocate on user demand. This requires a bit of
//! infrastructure, which is defined here.

#include <udipe/connect.h>
#include <udipe/pointer.h>

#include "arch.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdalign.h>


/// Shared connection options
///
/// \ref udipe_connect_options_t is rather large, and it unfortunately has to be
/// because IPv6 addresses are huge. We would rather not have this big struct
/// bloat up the internal union of the \ref command_t struct that is sent to
/// worker threads on every request.
///
/// But on the flip side, connecting to a new host should be a rare event. Which
/// means that it is fine to use some special allocation policy for the
/// connection options struct that is a bit less optimal from a performance or
/// liveness perspective.
///
/// We therefore use a small pool of preallocated connection options within \ref
/// udipe_context_t; such that each connection attempt from a client thread
/// takes one of these structs if available, or blocks if none is available,
/// using the synchronization protocol described in the internal documentation
/// of \ref udipe_context_t.
typedef struct shared_connect_options_s {
    /// Connection options
    ///
    /// Should be aligned on a false sharing granule boundary because different
    /// options may be written to by different client threads and it's important
    /// to avoid false sharing interference across these client threads + reduce
    /// interference between them and the worker threads.
    alignas(FALSE_SHARING_GRANULARITY) udipe_connect_options_t options;
} shared_connect_options_t;
static_assert(alignof(shared_connect_options_t) == FALSE_SHARING_GRANULARITY,
              "Different options may belong to different client threads and "
              "should thus reside in their own false sharing granules");
static_assert(sizeof(shared_connect_options_t) == FALSE_SHARING_GRANULARITY,
              "Options should not need more than one false sharing granule.");

/// Number of \ref shared_connect_options_t within a \ref
/// connect_options_allocator_t
///
/// With the current simple allocator design, this must be <= 32. That is not a
/// problem for any use case that has been considered so far.
///
/// Indeed, if you need more than 32 of these, it means that you are trying to
/// concurrently establish more than 32 distinct network connexions, at a rate
/// faster than worker threads can process. This is something that should only
/// happen during the application initialization stage, and even then 32
/// concurrent connection requests should be more than enough to achieve
/// satisfactory system utilization on typical server, so it is okay to block
/// client threads for a little while until some of the ongoing connection
/// requests have been processed;
#define NUM_CONNECT_OPTIONS ((size_t)32)
static_assert(NUM_CONNECT_OPTIONS <= 32,
              "Imposed by Linux futex limitations + simple bit array design");

/// Simple allocator for \ref shared_connect_options_t
///
/// This is a pool of \ref shared_connect_options_t that client threads can tap
/// into when they want to send a udipe_connect() command to worker threads in
/// order to establish a new UDP connection.
///
/// Unlike most other structs from `libudipe`, this struct **must** be
/// initialized using a method, which is connect_options_allocator_initialize().
typedef struct connect_options_allocator_s {
    /// Futex that tracks which of the `options` are available for use
    ///
    /// This futex is a bit array where each bit is set to 1 to indicate that
    /// the matching entry of the `connect_options` array is available, or 0 to
    /// indicate that it is currently used as part of some outstanding
    /// connection request to worker threads.
    ///
    /// A worker thread that is done using some options struct and figures out
    /// that is was its last user must liberate it as follows:
    ///
    /// 1. Use fetch_and() with release memory ordering to atomically set the
    ///    associated bit of this bitfield.
    /// 2. If all options were formerly used up (word == 0), then use
    ///    single-waiter FUTEX_WAKE to wake one of the waiting client threads.
    alignas(FALSE_SHARING_GRANULARITY) _Atomic uint32_t availability;

    /// Pool of connection options that can be allocated from
    ///
    /// See \ref shared_connect_options_t and the `connect_options_futex` member
    /// of this struct for more info.
    shared_connect_options_t options[NUM_CONNECT_OPTIONS];
} connect_options_allocator_t;

/// Initialize a \ref connect_options_allocator_t
///
/// Unfortunately, the C11 atomics specification disallows zero initialization
/// of atomics, so you must call this method in order to initialize a \ref
/// connect_options_allocator_t.
connect_options_allocator_t connect_options_allocator_initialize();

/// Set up a \ref shared_connect_options_t for use by a worker thread.
///
/// If none is currently available, this method will block until another thread
/// liberates one using connect_options_liberate().
///
/// \param allocator must be a \ref connection_options_allocator_t that has
///                  previously been initialized using
///                  connect_options_allocator_initialize().
///
/// \returns a \ref shared_connect_options_t that the worker thread must
///          liberate after use via connect_options_liberate().
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
shared_connect_options_t*
connect_options_allocate(connect_options_allocator_t* allocator);

/// Indicate that a worker thread is done with some \ref
/// shared_connect_options_t.
///
/// \param allocator must be a \ref connection_options_allocator_t that has
///                  previously been initialized using
///                  connect_options_allocator_initialize().
/// \param options must be a \ref shared_connect_options_t that has previously
///                been allocated using connect_options_allocate() and has not
///                yet been liberated.
UDIPE_NON_NULL_ARGS
void connect_options_liberate(connect_options_allocator_t* allocator,
                              shared_connect_options_t* options);


// TODO: Add unit tests
