#pragma once

//! \file
//! \brief Allocation of \ref udipe_future_t and associated resources
//!
//! This code module provides top-level methods to allocate and liberate future
//! objects, but most of the associated logic is actually implemented in the
//! `allocator/` sub-directory.

#include <udipe/context.h>
#include <udipe/future.h>
#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "allocator/thread_cache.h"
#include "allocator/context_cache.h"

#include "type.h"


/// \name Public allocator interface
/// \{

/// Allocate a future
///
/// The future is provided in a partially initialized state, where auxilliary
/// system resources like Linux special file descriptors are preallocated, but
/// may not be fully configured.
///
/// - `context` pointer is forwarded from this function's parameter
/// - `status_word` is only gets `type` set as appropriate, with other fields
///   remaining in a zeroed-out state that may still need to be adjusted.
///     * `downstream_count` is set to 0
///     * `downstream_count_overflow` is cleared
///     * `active` is cleared (to be set once fully initialized)
///     * `state` is \ref STATE_UNINITIALIZED (to be set as appropriate)
///     * `outcome is \ref OUTCOME_UNKNOWN (to be set if appropriate)
///     * `notify_address` is cleared
///     * `notify_event_or_lazy_lock` is cleared
/// - `status_sync` is configured with a preallocated synchronization object
///   of the appropriate type for the future type of interest.
///     * `event` for all future types that are eagerly signaled (at the time of
///       writing that is network and custom futures on Linux, in the future it
///       will most likely include join, unordered and timer_repeat on Windows)
///     * `timer_once` for one-shot timer futures.
///     * On Linux, `latched_inpoll` is allocated and partially configured but
///       some work remains before the future is fully initialized:
///         - For join, the output fds or upstream futures must be attached to
///           the `latched_inpoll`.
///         - For unordered, `inpoll_latch` and `upstream_inpoll` are allocated
///           and attached to the `latched_inpoll` but the output fds of
///           upstream futures must be attached to the `upstream_inpoll`.
///         - For timer_repeat, `inpoll_latch` and `timerfd` are allocated and
///           attached to the `latched_inpoll`, but `timerfd` must still be set
///           upp with an initial deadline and an interval.
///
/// No other type-specific state is initially configured. For example the \ref
/// collective_upstream_t of collective futures is left uninitialized as
/// configuring it requires extra information unknown to this function.
///
/// This function must be called within the scope of with_logger().
///
/// \param context must be a udipe context that was set up with
///                udipe_initialized() and not yet liberated with
///                udipe_finalize(). It must not be liberated until the output
///                future is liberated.
/// \param type indicates the type of the future that is being built. It will
///             be used to allocate associated system resources which are
///             partially type-specific.
///
/// \returns a future that must later be liberated with future_liberate().
//
// TODO: May need to replace the boolean switch of
//       future_status_debug_check() with a 3-states enum to account for the
//       fact that futures will now have three states: unallocated, allocated
//       but not yet fully initialized, and under active use.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* future_allocate(udipe_context_t* context,
                                future_type_t type);

/// Liberate a future
///
/// The future will be reset to an unallocated state, then shelved into a
/// thread-local cache where later calls to future_allocate() will be able to
/// find and reuse it instead of resorting to a global allocation.
///
/// This function must be called within the scope of with_logger().
///
/// \param future must point to a future that was previously allocated to some
///               asynchronous operation, and has been liberated via
///               udipe_finish() if it was ever exposed to the user. This future
///               cannot be used again afterwards.
//
// TODO: Add GNU attributes to mark this + future_allocate() as an
//       allocator/liberator pair if possible.
UDIPE_NON_NULL_ARGS
void future_liberate(udipe_future_t* future);

/// \}


/// \name Implementation details
/// \{

/// Allocate a future object without any further setup
///
/// This is an implementation detail of future_allocate() that should not be
/// called directly.
///
/// \internal
///
/// The future will be provided in a fully uninitialized state where the status
/// word is zeroed out and all file descriptors are set to -1.
///
/// \param thread_cache should point to the thread-local cache from this thread.
/// \param context_cache should point to the context-global cache from the
///                      associated context.
///
/// \returns a freshly allocated future object that must be initialized as
///          described by the documentation of future_allocate() before being
///          returned to the user.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t*
future_allocate_uninitialized(future_thread_cache_t* thread_cache,
                              future_context_cache_t* context_cache);

/// Set up the system resources used to notify a future's readiness.
///
/// This is an implementation detail of future_allocate() that should not be
/// called directly.
///
/// \internal
///
/// The future must be provided with its `type` set up in its status word. This
/// function will take care of setting up all associated Linux file descriptors
/// and Windows synchronization objects as documented in the documentation of
/// future_allocate().
///
/// \param future should point to a future that was allocated by
///               future_allocate_uninitialized() and had its `type` set up in
///               its `status_word`.
/// \param thread_cache should point to the thread-local cache from this thread.
UDIPE_NON_NULL_ARGS
void future_sync_initialize(udipe_future_t* future,
                            future_thread_cache_t* thread_cache);

/// Tear down the system resources used to notify a future's readiness.
///
/// This is an implementation detail of future_liberate() that should not be
/// called directly.
///
/// \internal
///
/// The future should still have its `type` still set up in its status word.
/// This function will take care of destroying all associated Linux file
/// descriptors and Windows synchronization objects, replacing them with \ref
/// INVALID_FD or `NULL` handles as appropriate.
///
/// \param future should point to a future that previously went through the
///               future_sync_initialize() initialization phase and has not gone
///               through this step of the finalization process since then.
/// \param thread_cache should point to the thread-local cache from this thread.
// TODO implement
UDIPE_NON_NULL_ARGS
void future_sync_finalize(udipe_future_t* future,
                          future_thread_cache_t* thread_cache);

/// \}


// TODO: Unit tests
