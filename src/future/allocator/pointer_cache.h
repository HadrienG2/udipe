#pragma once

//! \file
//! \brief Future pointer cache
//!
//! This code module implements the \ref future_pointer_cache_t, which tracks
//! futures that are not currently in use and can be allocated to network
//! operations.

#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "storage_page.h"

#include "../../future.h"
#include "../../memory.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>


/// \name Inner page linked list of the pointer cache
/// \{

/// \copydoc future_pointer_page_s
typedef struct future_pointer_page_s future_pointer_page_t;

/// Memory page of pointers to unallocated future + link to other pages
///
/// See \ref future_pointer_cache_t for more information on the host data
/// structure.
///
/// Because future pointers are only allocated and liberated by user threads and
/// never touched by realtime network thread, these pages can be allocated using
/// malloc() and will eventually need to be liberated using free().
struct future_pointer_page_s {
    /// Previous page of future pointers, or `NULL` if this is the first page of
    /// the doubly linked list (which will always be the bottom page)
    future_pointer_page_t* previous;

    /// Next page of future pointers, or `NULL` if this is the last page of the
    /// doubly linked list (which may not be the top page)
    future_pointer_page_t* next;

    /// Future pointers held within this page, partitioned by `NULL`-ity.
    ///
    /// As explained in the \ref future_pointer_cache_t documentation...
    ///
    /// - Valid pointers come first, followed by `NULL` pointers
    /// - A page is only allowed to contain valid pointers if it is the first
    ///   page of the list or all pages before it are full of valid pointers.
    /// - A page is only allowed to contain `NULL` pointers if it is the last
    ///   page of the list or all pages after it are full of `NULL` pointers.
    ///
    /// The length of this array is given by future_pointer_page_capacity().
    udipe_future_t* futures[];
};

/// Length of the \ref future_pointer_page_t::futures array
///
/// This is the length of the `futures` flexible array member of \ref
/// future_pointer_page_t. Its size is not known until runtime because it
/// depends on the page size used by the host operating system.
///
/// This function must be called within the scope of with_logger().
UDIPE_NODISCARD
static inline
size_t future_pointer_page_capacity() {
    const size_t available_bytes =
        get_page_size() - offsetof(future_pointer_page_t, futures);
    assert(available_bytes >= sizeof(udipe_future_t*));
    return available_bytes / sizeof(udipe_future_t*);
}

/// Create a future pointer page with `NULL` previous/next pointers and
/// all-`NULL` inner futures.
///
/// The future pointer page must then be...
///
/// - Linked to the future pointer pages from the target \ref
///   future_pointer_cache_t that come before/after it via its `previous`/`next`
///   members, the `next` member of its predecessor (if any), and the `previous`
///   member of its successor (if any).
/// - Targeted by the `bottom` pointer of the host \ref future_pointer_cache_t
///   if it is the first inserted pointer page in said cache.
/// - Targeted by the `top` pointer of the host \ref future_pointer_cache_t if
///   new futures are meant to be inserted here (which is also true if it is the
///   first inserted page in said cache).
/// - Eventually liberated with free() after unlinking it from any other pointer
///   page that continue to exist within the same cache.
///
/// This function must be called within the scope of with_logger().
///
/// \returns a new empty future pointer page
UDIPE_NODISCARD
UDIPE_NON_NULL_RESULT
future_pointer_page_t* future_pointer_page_initialize();

/// Number of futures currently stored inside of a \ref future_pointer_page_t
///
/// Not to be confused with future_pointer_page_capacity() which tracks the
/// number of future pointers that _can_ be stored inside of a page.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
static inline
size_t future_pointer_page_occupancy(future_pointer_page_t* page) {
    if (page->futures[0] == NULL) return 0;
    const size_t capacity = future_pointer_page_capacity();
    if (page->futures[capacity - 1]) return capacity;

    size_t last_present = 0;
    size_t first_absent = capacity - 1;
    while (first_absent != last_present + 1) {
        const size_t midpoint = last_present + (first_absent - last_present) / 2;
        if (page->futures[midpoint]) {
            last_present = midpoint;
        } else {
            first_absent = midpoint;
        }
    }
    return first_absent;
}

/// Unlink a future pointer page from its predecessors and successors
///
/// This is done as part of the process of removing a pointer page from a \ref
/// future_pointer_cache_t.
///
/// Note that if the page is targeted by \ref future_pointer_cache_t::top or
/// \ref future_pointer_cache_t::bottom, these pointers must be shifted forward
/// before calling this function.
UDIPE_NON_NULL_ARGS
static inline
void future_pointer_page_unlink(future_pointer_page_t* extracted) {
    if (extracted->next) extracted->next->previous = extracted->previous;
    if (extracted->previous) extracted->previous->next = extracted->next;
    extracted->next = NULL;
    extracted->previous = NULL;
}

/// \}


/// \name Future pointer cache
/// \{

/// Future pointer cache
///
/// This struct tracks `udipe_future_t*` pointers that are not currently in use
/// and can be allocated to some asynchronous operations.
///
/// The pointers are held within a doubly linked list of \ref
/// future_pointer_page_t, which functions as a hybrid between a ring buffer of
/// pages and a segmented stack of futures:
///
/// - Each page holds a fixed size array of future pointers, some of which may
///   be valid or `NULL`. This "inner" array is partitioned such that all valid
///   future pointers go at the beginning and all `NULL` pointers go at the end.
/// - The doubly linked list of pages is logically treated as a single long
///   array of future pointers, similar to common implementations of C++'s
///   `std::deque`. The list of pages is fixed-size for a thread-local \ref
///   future_thread_cache_t and variable-sized for the global \ref
///   future_context_cache_t shared by all threads. Extending the above
///   intra-page logic, the page list can in full generality start with some
///   pages full of futures, possibly followed by one page that starts with some
///   futures and ends with some `NULL`s, and after that there can only be pages
///   full of `NULL`.
/// - Struct-wide metadata speeds up lookup in this segmented stack by tracking
///   the first page of the list (`bottom`), the last page that may have futures
///   in it (`top`), and the number of futures stored in the top page
///   (`num_top_futures`, which is only 0 when all pages are full of `NULL`s).
/// - When a future is liberated, it is inserted into the top page of the local
///   cache of the current thread, if there is room for it. Otherwise the future
///   goes into the page after that (which becomes the new top page), and so on
///   until the end of the list is reached and all pages are full of futures. In
///   the interest of making this explanation clearer, we defer discussion of
///   what happens then.
/// - When a future is allocated, it is taken from the top page of the local
///   cache of the current thread if there is a future there, then the top
///   pointer goes back to the previous page if that page becomes empty as a
///   result and there is indeed a previous page in the linked list. Again, we
///   defer discussion of what happens when no future is locally available.
/// - When a future is liberated and the local cache is full, the bottom page of
///   the list is extracted and migrated into the global cache. Then a new empty
///   page is inserted at the top end of the local cache, where the newly
///   liberated future can be inserted. Ideally this new empty page would be
///   recycled from the global cache (how it gets there is discussed below), but
///   a new page can also be freshly allocated if no such page is available in
///   the global cache.
/// - When a future is allocated and the local cache is empty, an attempt is
///   made to grab a page with futures from the global cache. On success, this
///   page replaces the (empty) top page of the local cache, which is discarded
///   into the global cache for later reuse (see above). On failure, a page of
///   new futures is allocated and pointers to these futures are inserted into
///   the current top page of the local cache.
/// - When a thread exits, futures from its cache are spilled into the global
///   cache, thus making all of them available for use by other threads.
///
/// This data structure has many good properties for a future pointer cache:
///
/// - It is segmented at the granularity of pages because segments can migrate
///   between threads and pages are the finest granularity at which the OS can
///   migrate memory between NUMA nodes. This enables efficient migration
///   between arbitrary threads that can reside on different NUMA nodes, without
///   requiring complex NUMA-aware migration logic on the udipe side.
/// - In the fast path where local cache overflow and underflow does not happen,
///   future pointers are handled in a stack-like LIFO fashion (which is optimal
///   for CPU cache locality) and no inter-thread synchronization is needed as
///   the global cache is never touched. Conversely, when futures do need to
///   spill to the global cache, we spill futures from the bottom of the stack,
///   which are coldest in the current CPU's cache.
/// - If some user threads start more asynchronous operations and others threads
///   finish more asynchronous operations (disregarding documentation advice),
///   unbounded future leaks are avoided by bounding the size of local caches
///   and eventually spilling futures into the global cache. Spilling futures
///   into the global cache makes them become available for re-use for other
///   threads, so the amount of leaked futures at any point in time is bounded
///   by the finite capacity of each thread's local future cache.
/// - Exchanges between the local and global cache are batched at the very
///   coarse granularity of entire pages of pointers (a 4 KiB memory page can
///   hold 510 future pointers!). This amortizes the overhead of locking the
///   global cache's mutex and interacting with the global state, which has poor
///   cache locality due to MOESI logic and is susceptible to harmful NUMA
///   effects. In addition to making interactions with the global cache more
///   efficient, the odds of application threads contending for access to the
///   shared global cache are also reduced by this batching.
/// - Because each page holds many future pointers, there is only a low
///   probability of ending up in the pathological slow path of segmented stack
///   data structuress, where pushes and pops keep alternating between the top
///   of a segment and the bottom of the next segment, for two reasons. First,
///   this problematic pattern can only happen in applications where a single
///   process manipulates a huge number of futures, where we expect that most
///   applications will be content with a single page of local cache. Second, we
///   can't imagine a realistic scenario where this behavior could happen across
///   more than three storage pages, and we believe that modern CPUs should
///   handle two pages well enough.
/// - In spite of all these good properties, the data structure remains
///   relatively simple, and a typical future allocation/liberation transaction
///   should only requires a couple of CPU instructions. So the above good
///   properties do not come at the cost of compromising fast path performance.
///
/// One unknown remains though, which is whether a full page of future pointers
/// is too coarse. Consider a local cache that can hold a double buffer of
/// pointer pages, each storing pointers to 510 futures on x86_64. These pages
/// can point to a total of 1020 futures. A storage page holds 31 futures on
/// x86_64, so a pathological user thread that only liberates futures without
/// ever allocating any can end up holding hostage around 33 storage pages worth
/// of futures, plus the two pointer pages needed to track them, totaling 140
/// KiB of unused state per pathological thread. This overhead seems acceptable
/// for the typical high-performance x86_64 server, which has an enormous amount
/// of RAM per core, but can end up expensive on much smaller embedded
/// platforms. If udipe ever ends up used on such platforms, we may need to
/// expose a configurable that lets users artificially cap the capacity of \ref
/// future_pointer_page_t below what a page can hold.
//
// TODO: Add microbenchmarks once we can have them.
typedef struct future_pointer_cache_s {
    /// Bottom of the stack of unallocated future pointers pages
    ///
    /// If any page has futures in it, this will point to the first page that
    /// has futures in it. In a thread-local cache, that page contains the
    /// futures that were inserted into this cache the longest time ago, and are
    /// thus least expensive to spill into the global cache from a cache
    /// locality perspective.
    ///
    /// If multiple pages have futures in it (as indicated by `bottom != top`),
    /// this is additionally guaranteed to point to a page full of
    /// least-recently-used futures.
    future_pointer_page_t* bottom;

    /// Top of the stack of unallocated future pointers pages
    ///
    /// If any page has futures in it, this will point to the last page that has
    /// futures in it. In a thread-local cache, that page contains the futures
    /// that were most recently liberated into this cache, and should thus be
    /// reused first for optimal cache locality.
    ///
    /// This may not be the last page of the list, as there may be more empty
    /// pages full of `NULL` after it.
    future_pointer_page_t* top;

    /// Number of unallocated future pointers in the top page of the stack
    ///
    /// This field can only be zero when all pointer pages are empty.
    size_t num_top_futures;
} future_pointer_cache_t;

/// Create a future pointer cache
///
/// The freshly created cache may either be set up as a thread-local cache or as
/// the global cache of a \ref udipe_context_t. The respective role of these
/// caches and the interplay between them is described in more details in the
/// documentation of \ref future_pointer_cache_t, but at the time of writing,
/// the main lifecycle differences between them is that...
///
/// - Thread-local caches have fixed and preallocated pointer storage capacity,
///   and they should be spilled into the global cache with
///   future_pointer_cache_recycle_local() if the corresponding thread exits
///   before the host context is destroyed by udipe_finalize(). When the host
///   context is destroyed, it will liberate all remaining thread local caches
///   after notifying the corresponding threads that they shouldn't spill to the
///   global cache anymore.
/// - The global context cache has unbounded storage capacity with no
///   preallocated pointer pages, and gets eventually liberated with
///   future_pointer_cache_finalize() when the host context is destroyed by
///   udipe_finalize().
///
/// This function must be called within the scope of with_logger().
///
/// \param global controls whether the cache that is being created is the global
///               context cache shared between all threads (if true) or the
///               local cache of a single thread (if false).
///
/// \returns a freshly initialized future cache that must eventually be either
///          1/recycled into the global cache with
///          future_pointer_cache_recycle_local() at thread exit time, if it is
///          a thread-local cache and the host context has not been destroyed
///          yet; or 2/destroyed with future_pointer_cache_finalize() when the
///          host context gets destroyed by udipe_finalize().
UDIPE_NODISCARD
future_pointer_cache_t future_pointer_cache_initialize(bool global);

/// Attempt to allocate a future object from a thread-local cache
///
/// If allocation succeeds, the future object will be provided in an
/// uninitialized state with no file descriptor attached. You must set up the
/// future according to its designated type, and in particular allocate and
/// attach all proper file descriptors using other caches, before returning this
/// future to the user.
///
/// Allocation may fail if no future object is available in this cache, which
/// will result in this function returning a `NULL` pointer. When this happens,
/// you need to...
///
/// - Lock the global context cache and attempt to stead a page of futures from
///   it with future_pointer_cache_extract_futures().
///     - If you succeed, swap that page with a local empty page from the
///       thread-local cache using a combination of
///       future_pointer_cache_insert_futures() and
///       future_pointer_cache_obtain_empty(), then dump the extracted empty
///       page into the global cache with future_pointer_cache_insert_empty().
///     - If you fail, add a new storage page to the global cache with
///       future_storage_allocate() then add the associated futures to this
///       empty local cache with future_pointer_cache_refill_local().
/// - After refilling the thread-local cache this way, you can call
///   future_pointer_cache_allocate_local() again, and this call will succeed.
///
/// This function must be called within the scope of with_logger().
///
/// \param local_cache must point to a thread-local cache that was set up with
///                    future_pointer_cache_initialize(false) and wasn't
///                    destroyed by future_pointer_cache_recycle_local() or
///                    future_pointer_cache_finalize() yet.
///
/// \returns an uninitialized future object if available, or `NULL` if no future
///          object is available in the target cache (what to do then is
///          described above).
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
udipe_future_t*
future_pointer_cache_allocate_local(future_pointer_cache_t* local_cache);

/// Attempt to liberate a future object into a thread-local cache
///
/// A future does not need to be liberated into the exact cache from which it
/// was previously allocated, but it does need to have been allocated from a
/// cache that belongs to the same \ref udipe_context_t as the target cache.
///
/// Liberation may fail if the cache is full. When this happens, you need to...
///
/// - Lock the global context cache and obtain an empty page of pointers from
///   it with future_pointer_cache_obtain_empty().
/// - Swap this empty page with a full page from the thread-local cache using a
///   combination of future_pointer_cache_insert_empty() and
///   future_pointer_cache_extract_futures(), then dump the full page into the
///   global cache using future_pointer_cache_insert_futures().
/// - After making room in the thread-local cache this way, you can call
///   future_pointer_cache_liberate() again, and this call will succeed.
///
/// This function must be called within the scope of with_logger().
///
/// \param local_cache must point to a thread-local cache that was set up with
///                    future_pointer_cache_initialize(false) and wasn't
///                    destroyed by future_pointer_cache_recycle_local() or
///                    future_pointer_cache_finalize() yet.
/// \param future must point to a future that was previously allocated from one
///               cache of the same \ref udipe_context_t with
///               future_pointer_cache_allocate_local() and wasn't liberated
///               with future_pointer_cache_liberate_local() yet.
///
/// \returns true if the future was successfully liberated, false if it could
///          not be liberated because the target cache is full (what to do then
///          is described above).
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
bool future_pointer_cache_liberate_local(future_pointer_cache_t* local_cache,
                                         udipe_future_t* future);

/// Attempt to extract a non-empty page of future pointers from a cache
///
/// The page will be taken from the bottom of the cache because...
///
/// - If there is any page full of futures, this will be one of them. That makes
///   inter-cache transfers as coarse-grained as possible and thus maximizes the
///   efficiency of associated synchronization transactions.
/// - In thread-local caches, this page contains the future pointers that have
///   been liberated the longest time ago, and are thus coldest in terms of
///   cache locality.
/// - In the global cache, the above may not hold, but it doesn't matter because
///   cache locality is not a goal for inter-thread future transfers.
///
/// This operation will fail by returning `NULL` if the target cache is fully
/// empty. It will return a page full of futures whenever possible but may
/// return a page that is not full of futures when extracting the last non-empty
/// page from a cache. Clients can tell how many futures they actually extracted
/// using future_pointer_page_count().
///
/// When calling this on a thread-local cache for the purpose of transfering
/// futures into the global cache, remember that you must keep the capacity of
/// thread-local caches constant until liberation time. This means that except
/// in future_pointer_cache_recycle_local(), every page you extract must be
/// replaced with an empty page through a combination of
/// future_pointer_cache_obtain_empty() on the global cache and
/// future_pointer_cache_insert_empty() on the thread-local cache.
///
/// This function must be called within the scope of with_logger().
///
/// \param cache must point to a cache that was set up with
///              future_pointer_cache_initialize() and wasn't destroyed by
///              future_pointer_cache_recycle_local() or
///              future_pointer_cache_finalize() yet.
///
/// \returns a page containing at least one future pointer on success or `NULL`
///          on failure.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_pointer_page_t*
future_pointer_cache_extract_futures(future_pointer_cache_t* cache);

/// Get a page of `NULL` future pointers, taking it from a cache if possible and
/// allocating a new one otherwise
///
/// This function must be called within the scope of with_logger().
///
/// \param cache must point to a cache that was set up with
///              future_pointer_cache_initialize() and wasn't destroyed by
///              future_pointer_cache_recycle_local() or
///              future_pointer_cache_finalize() yet.
///
/// \returns a page containing only `NULL` future pointers.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
future_pointer_page_t*
future_pointer_cache_obtain_empty(future_pointer_cache_t* cache);

/// Insert a page of futures that was previously extracted via
/// future_pointer_cache_extract_futures() into its destination cache
///
/// This function must be called within the scope of with_logger().
///
/// \param cache must point to a cache that was set up with
///              future_pointer_cache_initialize() and wasn't destroyed by
///              future_pointer_cache_recycle_local() or
///              future_pointer_cache_finalize() yet.
/// \param non_empty must point to a \ref future_pointer_page_t that contains at
///                  least one valid pointer, and will usually (but not always)
///                  be composed only of valid pointers. This page should have
///                  been obtained via future_pointer_cache_extract_futures() on
///                  a cache of the opposite type.
UDIPE_NON_NULL_ARGS
void future_pointer_cache_insert_futures(future_pointer_cache_t* cache,
                                         future_pointer_page_t* non_empty);

/// Insert a page of `NULL` future pointers that was previously extracted via
/// future_pointer_cache_obtain_empty() into its destination cache
///
/// This function must be called within the scope of with_logger().
///
/// \param cache must point to a cache that was set up with
///              future_pointer_cache_initialize() and wasn't destroyed by
///              future_pointer_cache_recycle_local() or
///              future_pointer_cache_finalize() yet.
/// \param empty must point to a \ref future_pointer_page_t that contains only
///              `NULL` pointers. This page should have been obtained via
///              future_pointer_cache_obtain_empty() on a cache of the opposite
///              type.
UDIPE_NON_NULL_ARGS
void future_pointer_cache_insert_empty(future_pointer_cache_t* cache,
                                       future_pointer_page_t* empty);

/// Add a freshly allocated batch of new futures to an empty thread-local cache
///
/// This function is called on a thread-local cache after both this local cache
/// and the global cache are found to be empty during the process of allocating
/// a new future. It fills back the empty local cache with a new batch of
/// freshly allocated futures from the global cache.
///
/// This function must be called within the scope of with_logger().
///
/// \param local_cache must point to a thread-local cache that was set up with
///                    future_pointer_cache_initialize(false), wasn't destroyed
///                    by future_pointer_cache_recycle_local() or
///                    future_pointer_cache_finalize() yet, and is currently
///                    empty.
/// \param new_futures must point to a new page of futures that was freshly
///                    added to the associated global cache by
///                    future_storage_allocate(), and whose futures weren't
///                    added to any other local cache.
UDIPE_NON_NULL_ARGS
void future_pointer_cache_refill_local(future_pointer_cache_t* local_cache,
                                       future_storage_page_t* new_futures);

/// Recycle a thread-local future cache's contents into the global process cache
///
/// This function can be called at thread exit time to spill the contents of the
/// active thread's local future cache into the associated global context cache,
/// if the associated context was not destroyed first
///
/// Mind the parameter order, local cache before global cache. Getting it wrong
/// can result in disastrous outcome but unfortunately C sucks too much at
/// newtypes for extra type safety against this usage error to be worthwhile.
///
/// \param local must point to a thread-local cache that was set up with
///              future_pointer_cache_initialize(false), wasn't destroyed by
///              future_pointer_cache_recycle_local() or
///              future_pointer_cache_finalize() yet. It cannot be used again
///              after this function has been called on it.
/// \param global designates the global cache into which the thread-local cache
///               will be spilled. It should have been set up with
///               future_pointer_cache_initialize(true) and not have been
///               destroyed with future_pointer_cache_finalize() yet. As always,
///               when manipulating the global cache, the associated lock must
///               have been acquired first.
UDIPE_NON_NULL_ARGS
void future_pointer_cache_recycle_local(future_pointer_cache_t* local,
                                        future_pointer_cache_t* global);

/// Destroy a future pointer cache
///
/// This function can only be used when the \ref udipe_context_t that holds this
/// cache is destroyed. If a thread exits before that and it has a thread-local
/// cache, it should instead spill its local cache to the global cache with
/// future_pointer_cache_recycle_local().
///
/// This function must be called within the scope of with_logger().
///
/// \param cache must point to a pointer cache that was set up with
///              future_pointer_cache_initialize() and wasn't destroyed by
///              future_pointer_cache_recycle_local() or
///              future_pointer_cache_finalize() yet. It cannot be used again
///              after calling this function.
UDIPE_NON_NULL_ARGS
void future_pointer_cache_finalize(future_pointer_cache_t* cache);

/// \}


// TODO: Unit tests, benchmarks
