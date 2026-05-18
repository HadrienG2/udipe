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
    /// The length of this array is given by future_pointer_page_len().
    udipe_future_t* futures[];
};

/// Length of \ref future_pointer_page_t::futures
///
/// This is the length of the `futures` flexible array member of \ref
/// future_pointer_page_t. Its size is not known until runtime because it
/// depends on the page size used by the host operating system.
///
/// This function must be called within the scope of with_logger().
UDIPE_NODISCARD
static inline
size_t future_pointer_page_len() {
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
///   `std::deque`. The list of pages is fixed-size for thread-local caches and
///   variable-sized for the global \ref udipe_context_t cache shared by all
///   threads. Extending the above intra-page logic, the page list can in full
///   generality start with some pages full of futures, possibly followed by one
///   page that starts with some futures and ends with some `NULL`s, and after
///   that there can only be pages full of `NULL`.
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
    /// has futures in it. That page contains the futures that were inserted
    /// into this cache the longest time ago, and are thus least expensive to
    /// spill into the global cache from a cache locality perspective.
    ///
    /// If multiple pages have futures in it (as indicated by `bottom != top`),
    /// this is additionally guaranteed to point to a page full of
    /// least-recently-used futures.
    future_pointer_page_t* bottom;

    /// Top of the stack of unallocated future pointers pages
    ///
    /// If any page has futures in it, this will point to the last page that has
    /// futures in it. That page contains the futures that were most recently
    /// inserted into this cache, and should thus be reused first for optimal
    /// cache locality.
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
/// - Add future objects to your "shopping list" for future global cache lookup,
///   and proceed to figure out which other future components you can get from
///   the local cache and which ones must be looked up in the global cache too.
/// - Once you are done enumerating everything you need from the global cache,
///   lock it and (among other things) attempt to steal a page of futures with
///   future_pointer_cache_extract_futures().
///     - If you succeed, swap that page with a local empty page by combining
///       future_pointer_cache_insert_futures() and
///       future_pointer_cache_obtain_empty(), then dump the extracted empty
///       page into the global cache with future_pointer_cache_insert_empty().
///     - If you fail, add a new future storage page to the global cache with
///       future_storage_allocate() then add the associated futures to this
///       empty local cache with future_pointer_cache_refill_local().
/// - After you are done with these cache operations, call
///   future_pointer_cache_allocate_local() on the local cache again. It will
///   then be guaranteed to succeed.
///
/// This function must be called within the scope of with_logger().
///
/// \param local_cache must point to a thread-local cache that was set up with
///                    future_pointer_cache_initialize(false) and wasn't
///                    destroyed by future_pointer_cache_recycle_local() or
///                    future_pointer_cache_finalize() yet.
///
/// \returns an uninitialized future object if available, or `NULL` if no future
///          object is available in this thread-local cache (what to do
///          then is described above).
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
udipe_future_t*
future_pointer_cache_allocate_local(future_pointer_cache_t* local_cache);

/// Attempt to liberate a future object from a thread-local cache
///
/// A future does not need to be liberated into the exact cache from which it
/// was previously allocated, but it does need to have been allocated from a
/// cache that belongs to the same \ref udipe_context_t as the target cache.
///
/// Liberation can fail if the thread-local cache is full of future objects.
/// When this happens, you need to...
///
/// - Add future objects to your "garbage list" for future global cache spills
///   and proceed to figure out which other future components you can recycle
///   into the local cache and which ones must spill into the global cache.
/// - Once you are done enumerating what must spill into the global cache, lock
///   it and (among other things) obtain a page of empty futures from it with
///   future_pointer_cache_obtain_empty(). Then swap this empty page with a full
///   page from the local cache with a combination of
///   future_pointer_cache_insert_empty() and
///   future_pointer_cache_extract_futures(), and then dump that full page into
///   the global cache with future_pointer_cache_insert_futures().
/// - After you are done with the global cache, call
///   future_pointer_cache_liberate_local() on the local cache again. It will
///   then be guaranteed to succeed.
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
///          not be liberated because the target local cache is full (what to do
///          then is described above).
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
bool future_pointer_cache_liberate_local(future_pointer_cache_t* local_cache,
                                         udipe_future_t* future);

/// Attempt to extract a page of future pointers from a cache
///
/// This function can be called on both thread-local caches and the global
/// cache, but for different reasons and with different outcomes:
///
/// - When called on thread-local caches, its purpose is spilling futures to the
///   global cache after failed liberation, so it should always be called on a
///   full cache and therefore should always succeed and return a a full page of
///   future pointers. Because thread-local caches operate at constant pointer
///   storage capacity, the extracted page must later be replaced with a page of
///   `NULL` pointers with future_pointer_cache_insert_empty().
/// - When called on the global cache, its purpose is transfering any previously
///   spilled futures to a local cache. As the initial state of the global cache
///   is unknown in this configuration, there are many more possible outcomes:
///     - The operation may fail because the global cache has no futures to hand
///       over to the local cache.
///     - The operation may succeed and return a page full of future pointers as
///       in the thread-local case.
///     - The operation may succeed but return a page of future pointers that is
///       only partially filled. This outcome should be rare, but can happen
///       occasionally because thread-local caches spill into the global cache
///       on thread exit and they may spill partially filled pointer pages then.
///
/// In the latter case, the caller is responsible for determining if the page is
/// full or partially full (which can be done by checking the last pointer of
/// the page for null-ness) then taking appropriate action.
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
/// Like future_pointer_cache_extract_futures(), this function can be called on
/// both thread-local caches and the global cache, but for different reasons:
///
/// - When called on thread-local caches, its purpose is to "make room" for a
///   page of futures that was extracted from the global cache because this
///   local cache was empty. Therefore the local cache must be empty and because
///   these caches operate at constant >1 page storage capacity, it means this
///   function it is guaranteed to successfully extract a page from said cache.
/// - When called on the global cache, it is part of the process of swapping out
///   a page of futures from a thread-local cache into the global cache for the
///   purpose of making room for liberating more futures. Because thread-local
///   caches operate at constant pointer storage capacity, pages of pointers
///   that get spilled this way must be replaced, and we would rather do so with
///   a pointer page from the global cache instead of allocating a new one. But
///   if no such page is available, it's fine, we can just allocate a new one.
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
/// This function is part of the process of transfering pages of futures between
/// a thread-local cache and a global cache. See
/// future_pointer_cache_extract_futures() for more info about the intended
/// usage patterns.
///
/// In particular, when operating on a thread-local cache, users of this
/// function should be mindful to keep the number of \ref future_pointer_page_t
/// constant. When a page of futures comes in, a page of `NULL`s must go out.
///
/// This function must be called within the scope of with_logger().
///
/// \param cache must point to a cache that was set up with
///              future_pointer_cache_initialize() and wasn't destroyed by
///              future_pointer_cache_recycle_local() or
///              future_pointer_cache_finalize() yet.
/// \param futures must point to a \ref future_pointer_page_t that contains at
///                least one valid pointer, and will usually (but not always) be
///                composed only of valid pointers. This page should have been
///                obtained via future_pointer_cache_extract_futures() on a
///                cache of the opposite type.
UDIPE_NON_NULL_ARGS
void future_pointer_cache_insert_futures(future_pointer_cache_t* cache,
                                         future_pointer_page_t* futures);

/// Insert a page of `NULL` future pointers that was previously extracted via
/// future_pointer_cache_obtain_empty() into its destination cache
///
/// This function is part of the process of transfering pages of `NULL` future
/// pointers between a thread-local cache and a global cache. See
/// future_pointer_cache_obtain_empty() for more info about the intended usage
/// patterns.
///
/// In particular, when operating on a thread-local cache, users of this
/// function should be mindful to keep the number of \ref future_pointer_page_t
/// constant. When a page of `NULL`s comes in, a page of futures must go out.
///
/// This function must be called within the scope of with_logger().
///
/// \param cache must point to a cache that was set up with
///              future_pointer_cache_initialize() and wasn't destroyed by
///              future_pointer_cache_recycle_local() or
///              future_pointer_cache_finalize() yet.
/// \param futures must point to a \ref future_pointer_page_t that contains only
///                `NULL` pointers. This page should have been obtained via
///                future_pointer_cache_obtain_empty() on a cache of the
///                opposite type.
UDIPE_NON_NULL_ARGS
void future_pointer_cache_insert_empty(future_pointer_cache_t* cache,
                                       future_pointer_page_t* empty);

/// Add a freshly allocated batch of new futures to an empty thread-local cache
///
/// This function is called on a thread-local cache after both this local cache
/// and the global cache are found to be empty during the process of allocating
/// a new future. It fills back the empty local cache with a new batch of
/// freshly allocated futures from the associated global cache.
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
/// newtypes for extra type safety against this error to be worthwhile.
///
/// TODO: Do not use the logger or call any function that uses it in this
///       function, it may not be available at thread exit time.
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
/// \param local_cache must point to a pointer cache that was set up with
///                    future_pointer_cache_initialize() and wasn't destroyed by
///                    future_pointer_cache_recycle_local() or
///                    future_pointer_cache_finalize() yet. It cannot be used
///                    again after calling this function.
UDIPE_NON_NULL_ARGS
void future_pointer_cache_finalize(future_pointer_cache_t* cache);
