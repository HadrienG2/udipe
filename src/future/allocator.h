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

#include "type.h"


/// Allocate a future
///
/// The future is provided in a partially initialized state:
///
/// - `context` pointer is forwarded from this function's parameter
/// - `status_word` has...
///   * `downstream_count` set to 0
///   * `downstream_count_overflow` cleared
///   * `active` bit cleared (must be set once this future is ready for use)
///   * \ref STATE_UNINITIALIZED (must be set according to the presence/absence
///     of upstream futures, their initial status, etc.)
///   * \ref OUTCOME_UNKNOWN (may need to be set if the outcome is determined
///     right from the start).
///   * `type` set as appropriate to the specified future type.
///   * `notify_address` unset.
///   * `notify_event_or_lazy_lock` unset.
/// - `status_sync` and `specific` are partially configured according to the
///   future type, in such a way that all required system resources are
///   preallocated and relations between these are already set up, but other
///   state which requires access to other future configuration parameters is
///   not set up.
///   * `status_sync.event`, is is allocated and in an unsignaled state.
///   * `status_sync.timer` is allocated but in an unspecified state. It may be
///     set to a particular deadline/period or be unset. You must set it to the
///     desired deadline with no period before use.
///   * `status_sync.latched_epoll` (Linux-only) is already allocated and
///     attached to the associated \ref epoll_latch_event_t with identifier
///     `U64_MAX`, and...
///     - ...nothing else yet for \ref TYPE_JOIN. You must attach to it the
///       `status_sync` fds of upstream futures, identified with their index in
///       \ref collective_upstream_t before use.
///     - ...the `upstream_epollfd` for \ref TYPE_UNORDERED, which is
///       preallocated but not yet attached to any file descriptor. See the \ref
///       TYPE_JOIN case described above, except upstream fds must be attached
///       to `upstream_epollfd` not `status_sync.latched_epoll`.
///     - ...the `timerfd` for \ref TYPE_TIMER_REPEAT, which must be configured
///       as in the case of `status_sync.timer` above, but with a period.
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
// TODO: Implement. Should go through the thread-local cache first, then through
//       the global cache after locking it, and if the global cache is empty too
//       then should allocate a new page of futures, register it into the global
//       cache for liberation by atexit(), release the global cache lock,
//       put the futures in a zeroed/invalid state, and add all but one
//       future to the thread-local cache. The future which we set aside will
//       then be returned by this function.
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

// TODO: Ensure that 1/when a user thread exits, its thread-local unallocated
//       future cache is spilled into a global unallocated future cache and 2/on
//       atexit(), this global future cache is fully wiped: not just individual
//       futures, but also the memory pages as part of which these futures were
//       allocated. I think it makes most sense for the global future cache to
//       not be specific to any udipe context but shared across all udipe
//       contexts.


// TODO: Unit tests
