#pragma once

//! \file
//! \brief \ref udipe_connect_options_t allocation mechanism
//!
//! The \ref udipe_connect_options_t type is very large compared to other
//! command option types, so we do not want to store it inline inside of a \ref
//! command_t. Instead, we have a pool of these within the \ref udipe_context_t
//! that we dynamically allocate on user demand, using a reference counting
//! mechanism to avoid allocating multiple ones when the same command is sent to
//! multiple worker threads. This requires a bit of infrastructure, which is
//! defined here.

#include <udipe/connect.h>

#include "arch.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdalign.h>


/// Reference-counted connection options
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
/// We therefore use a small pool of preallocated reference-counted connection
/// options within \ref udipe_context_t such that...
///
/// - Each connection attempt from a client thread takes one of these structs if
///   available, or blocks if none is available, using the synchronization
///   protocol described in the internal documentation of \ref udipe_context_t.
/// - If this is a parallel connection that is destined to be serviced by
///   multiple worker threads, then a shared struct is allocated for all of
///   them, and reference counting is used to synchronize worker threads with
///   each other in the subsequent struct liberation process.
typedef struct shared_connect_options_s {
    /// Reference count
    ///
    /// This should be zero upon allocation if correct synchronization was used
    /// by prior worker threads. It is initialized to the number of worker
    /// threads that will consume this struct (1 for sequential connections,
    /// >= 1 for parallel connections) and will go down until it reaches zero.
    ///
    /// If this refcount is initially 1 (which can be checked with a relaxed
    /// load and is the case for all sequential connections), then the
    /// consumer worker thread can take the following fast path:
    ///
    /// - Read the `options` member
    /// - Set this refcount to zero with a relaxed store
    /// - Liberate this struct as directed in the documentation of \ref
    ///   udipe_context_t.
    ///
    /// If this refcount is not initially 1, then the standard reference
    /// counting pattern must be followed instead.
    ///
    /// - Read the `options` member
    /// - Decrement this refcount with a relaxed fetch_sub()
    /// - If the refcount reaches zero (i.e. fetch_sub() returns an initial
    ///   value of 1), then liberate this struct as directed in the
    ///   documentation of \ref udipe_context_t.
    ///   - Currently, this is done using a release atomic operation, so there
    ///     is no need for an additional release fence here, but if the release
    ///     procedure changes then a release fence will need to be added. It
    ///     therefore seems more prudent to make sure all of this is implemented
    ///     in the same function, with an appropriate warning comment.
    alignas(FALSE_SHARING_GRANULARITY) atomic_size_t reference_count;

    /// Connection options
    ///
    /// If the reference count is greater than 1, this struct is visible by
    /// multiple worker threads and must not be modified. This means that
    /// default values must be normalized into final settings within the client
    /// thread before this struct is sent to worker threads.
    udipe_connect_options_t options;
} shared_connect_options_t;
static_assert(alignof(shared_connect_options_t) == FALSE_SHARING_GRANULARITY);
static_assert(sizeof(shared_connect_options_t) == FALSE_SHARING_GRANULARITY);

// TODO: Add methods to allocate and deallocate this
