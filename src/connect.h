#pragma once

//! \file
//! \brief \ref udipe_connect_options_t allocation mechanism
//!
//! The \ref udipe_connect_options_t type is very large compared to other
//! command option types, so we do not want to store it inline inside of a \ref
//! command_t and thusly reduce the numer of commands we can store per command
//! queue at a given memory footprint.
//!
//! Instead, we have a pool of these within the \ref udipe_context_t that we
//! dynamically allocate on user demand. This requires a bit of infrastructure,
//! which is defined here.

#include <udipe/connect.h>
#include <udipe/pointer.h>

#include "arch.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdalign.h>


/// Number of \ref udipe_connect_options_t within a \ref
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

/// Simple allocator for \ref udipe_connect_options_t
///
/// This is a pool of \ref udipe_connect_options_t that client threads can tap
/// into when they want to send a udipe_connect() command to worker threads in
/// order to establish a new UDP connection.
///
/// Unlike most other structs from `libudipe`, this struct **must** be
/// initialized using a function, namely connect_options_allocator_initialize().
///
/// Members of this struct are not overaligned for false sharing avoidance or
/// other CPU cache layout optimization because connection setup should only
/// happen a relatively small amount of time at the start of an application's
/// lifecycle, which means that connection setup code should priorize simplicity
/// over runtime performance.
typedef struct connect_options_allocator_s {
    /// Pool of connection options that can be allocated from
    ///
    /// See the `connect_options_futex` member of this struct for more info
    /// about how these struct are allocatd.
    udipe_connect_options_t options[NUM_CONNECT_OPTIONS];

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
    _Atomic uint32_t availability;
} connect_options_allocator_t;

/// Initialize a \ref connect_options_allocator_t
///
/// Unfortunately, the C11 atomics specification disallows zero initialization
/// of atomics, so you must call this function in order to initialize a \ref
/// connect_options_allocator_t.
connect_options_allocator_t connect_options_allocator_initialize();

/// Finalize a \ref connect_options_allocator_t
///
/// The allocator cannot be used again after this is done.
UDIPE_NON_NULL_ARGS
void connect_options_allocator_finalize(connect_options_allocator_t* allocator);

/// Allocate a \ref udipe_connect_options_t struct for the purpose of sending
/// connection options from a client thread to a worker thread.
///
/// If no such struct is currently available, this function will block until a
/// worker thread liberates a struct using connect_options_liberate().
///
/// \param allocator must be a \ref connection_options_allocator_t that has
///                  previously been initialized using
///                  connect_options_allocator_initialize() and has not been
///                  finalized using connect_options_allocator_finalize() yet.
///
/// \returns a \ref udipe_connect_options_t that the target worker thread must
///          liberate after use via connect_options_liberate().
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_connect_options_t*
connect_options_allocate(connect_options_allocator_t* allocator);

/// Indicate that a worker thread is done with the \ref udipe_connect_options_t
/// struct that has previously been sent to it.
///
/// \param allocator must be a \ref connection_options_allocator_t that has
///                  previously been initialized using
///                  connect_options_allocator_initialize() and has not been
///                  finalized using connect_options_allocator_finalize() yet.
/// \param options must be a \ref udipe_connect_options_t that has previously
///                been allocated using connect_options_allocate() and has not
///                yet been liberated.
UDIPE_NON_NULL_ARGS
void connect_options_liberate(connect_options_allocator_t* allocator,
                              udipe_connect_options_t* options);


// TODO: Add unit tests
