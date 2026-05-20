#pragma once

//! \file
//! \brief Future storage page
//!
//! TODO: Module-level docs

#include <udipe/pointer.h>

#include "../../future.h"
#include "../../memory.h"

#include <assert.h>
#include <stddef.h>


/// \copydoc future_storage_page_s
typedef struct future_storage_page_s future_storage_page_t;

/// Memory page allocated to future storage + link to other pages
///
/// This is one node from a singly linked list of realtime-safe memory pages
/// that were allocated for the purpose of holding future objects.
///
/// These pages are allocated as a context-global entity and only liberated when
/// the host \ref udipe_context_t is finalized, a simple resource management
/// policy which offers several benefits:
///
/// - The high overhead of realtime-safe memory allocation is well amortized.
/// - By not attempting to free storage pages when no future within them is in
///   use, we do not need to track the usage of individual futures within a
///   page, which would require lots of thread synchronization because futures
///   can be liberated on a thread other than the one that allocated them.
///
/// However, this policy also comes at the expense of constantly keeping
/// future-associated memory footprint at the highest it's been since the udipe
/// context was created, with no way to trim it down other than destroying and
/// recreating the udipe context (which is not something we expect from users).
///
/// This is not considered a big problem as long as allocated future objects can
/// be indefinitely reused, even in pathological scenarios where e.g. one user
/// thread only allocates futures and sends them to another user thread which
/// only liberates them.
///
/// And that issue, in turn, is taken care of by other components of the future
/// allocator such as \ref future_pointer_cache_t, which enable maximally
/// efficient thread-local operation in well-behaved applications while still
/// enabling a reasonable degree of future reuse in less well-behaved
/// applications.
///
/// Because future objects are manipulated by realtime network threads, future
/// storage pages must be allocated with realtime_allocate() and eventually
/// liberated with realtime_liberate().
struct future_storage_page_s {
    /// Next memory page allocated to future storage, if any, otherwise `NULL`
    ///
    future_storage_page_t* next;

    // NOTE: If needed, there is room for a lot more metadata here. For example,
    //       on x86_64, where pointers are 8 bytes and false sharing granules
    //       are 128 bytes, 120 bytes of padding are currently available here.

    /// Futures allocated as part of this memory page
    ///
    /// The length of this array is given by future_storage_page_len().
    udipe_future_t futures[];
};
static_assert(offsetof(future_storage_page_t, futures) <= sizeof(udipe_future_t),
              "Metadata should not take up more room than one future");

/// Length of \ref future_storage_page_t::futures
///
/// This is the length of the `futures` flexible array member of \ref
/// future_storage_page_t. Its size is not known until runtime because it
/// depends on the page size used by the host operating system.
///
/// This function must be called within the scope of with_logger().
UDIPE_NODISCARD
static inline
size_t future_storage_page_len() {
    const size_t available_bytes =
        get_page_size() - offsetof(future_storage_page_t, futures);
    assert(available_bytes >= sizeof(udipe_future_t));
    return available_bytes / sizeof(udipe_future_t);
}

/// Set up a page of futures and link it to previously allocated pages
///
/// Given a pointer to the head of a list of future storage pages (which may be
/// `NULL` if the list is initially empty), this function will allocate a new
/// page of futures, initialize them, and insert them at the head of the storage
/// page list.
///
/// All storage allocated this way must eventually be liberated using
/// future_storage_liberate_all().
///
/// This function must be called within the scope of with_logger().
///
/// \param next must point to the head of the list of storage pages, where a new
///             page of futures will be allocated, initialized and inserted.
UDIPE_NON_NULL_ARGS
void future_storage_allocate(future_storage_page_t** next);

/// Liberate a linked list of future storage pages
///
/// Given a pointer to the head of the list of future storage pages, this
/// function will liberate all of them and reset the head pointer to `NULL`.
///
/// This function must not be called until a point of program execution where no
/// future is reachable from other threads of the program. At the time of
/// writing, this is only guaranteed at the time where the host \ref
/// udipe_context_t is finalized.
///
/// This function must be called within the scope of with_logger().
///
/// \param first must point to the head of the list of storage pages. All inner
///              pages will be liberated, then this pointer will be reset to
///              `NULL`.
UDIPE_NON_NULL_ARGS
void future_storage_liberate_all(future_storage_page_t** first);


// TODO: Unit tests
