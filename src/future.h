#pragma once

//! \file
//! \brief Implementation of \ref udipe_future_t
//!
//! TODO: Outline new implementation.

#include <udipe/future.h>

#include <udipe/context.h>
#include <udipe/nodiscard.h>
#include <udipe/pointer.h>
#include <udipe/result.h>

#include "future/collective_upstream.h"
#include "future/epoll_latch_event.h"
#include "future/inner_fd.h"
#include "future/status_sync.h"
#include "future/type.h"

#include "arch.h"
#include "event.h"
#include "memory.h"

#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


// TODO: This code module is becoming way too big and should be split into a
//       directory of smaller code modules. Could be one module for the struct
//       definitions, one for the status word manipulations, one for the
//       allocator, one for the waiting functions...


/// \name Future data structure
/// \{

/// \copydoc udipe_future_t
struct udipe_future_s {
    /// udipe context which this future belongs to
    ///
    /// Used to ensure that future methods do not need an additional context
    /// parameter after future allocation.
    alignas(FALSE_SHARING_GRANULARITY) udipe_context_t* context;

    /// State that is specific to a particular future type
    ///
    /// At most one of these fields will be set. Which will be set (if any)
    /// depends on the \ref future_type_t that is configured inside of this
    /// future's \ref future_status_t::type.
    union {
        /// Network command result payload
        ///
        /// This union variant is used for network operations, corresponding to
        /// a \ref future_type_t is in range from \ref TYPE_NETWORK_START
        /// inclusive to \ref TYPE_NETWORK_END exclusive.
        ///
        /// Upon completion or internal failure of the network operation, the
        /// udipe implementation will set this to the associated result before
        /// signaling the outcome with `memory_order_release`.
        ///
        /// The precise \ref future_type_t that you are dealing with will tell
        /// you which variant of this payload union has been set.
        udipe_network_payload_t network;

        /// Custom command result payload
        ///
        /// This union variant is used for custom operations, corresponding to
        /// \ref TYPE_CUSTOM.
        ///
        /// Aside from the fact that it is set by a user thread, rather than by
        /// the udipe implementation, it works just like the `network` variant.
        udipe_custom_payload_t custom;

        /// Joined future state
        ///
        /// This union variant corresponds to \ref TYPE_JOIN. It tracks the
        /// state needed to wait for all specified upstream futures to reach
        /// \ref OUTCOME_SUCCESS or at least one of them to reach a failing
        /// outcome. And when this happens, it makes it possible to signal
        /// availability of the final status after it has been set.
        struct {
            /// Set of upstream futures awaited by this collective future
            ///
            collective_upstream_t upstream;

            #ifdef __linux__
                /// Event object used to keep `status_sync.latched_epoll`
                /// perma-ready after the future has reached its final state.
                ///
                /// See \ref epoll_latch_event_t for more information.
                epoll_latch_event_t epoll_latch;
            #endif
        } join;

        /// Unordered future state and result
        ///
        /// This union variant corresponds to \ref TYPE_UNORDERED. It tracks the
        /// state needed to wait for at least one of the specified upstream
        /// futures to reach its final outcome. And when this happens, it makes
        /// it possible to report which future got ready and how to await
        /// subsequent futures (if any).
        struct {
            /// Set of upstream futures awaited by this collective future
            ///
            collective_upstream_t upstream;

            /// Result of the asynchronous operation
            ///
            /// This result is set before signaling \ref OUTCOME_SUCCESS. It
            /// indicates which of the upstream futures became ready and how to
            /// await the rest of the upstream futures.
            ///
            /// Must be written under `lazy_lock` protection. Inner future (if
            /// any) must not be recycled on udipe_finish(), as it will be fed
            /// to the caller which is responsible for liberating it.
            udipe_unordered_payload_t payload;

            #ifdef __linux__
                /// Inner epollfd that monitors the upstream `status_sync` fds
                ///
                /// This inner fd is attached to `status_sync.latched_epollfd`.
                /// See \ref inner_fd_t for more information about this
                /// cascading file descriptor pattern.
                ///
                /// It must be awaited under `lazy_lock` protection, and
                /// eventually detached from `status_sync.latched_epoll` and
                /// attached to the `latched_epoll` of the successor future (if
                /// any) once a result is ready.
                ///
                /// It must be destroyed when the last future in the unordered
                /// chain is liberated. There seems to be little point in trying
                /// to recycle the epollfds of unordered futures because setting
                /// up a collective future requires an arbitrarily large amount
                /// of epoll_ctl() syscalls, so it's not expected that epollfd
                /// allocation/liberation will often be the bottleneck.
                //
                // TODO: Prove the above assertion through benchmarking and
                //       profiling of real-world workloads.
                // TODO: Find an epoll replacement for Windows. Will likely be
                //       based on the Win32 thread pool driving an eager future.
                inner_fd_t upstream_epollfd;

                /// Event object used to keep `status_sync.latched_epoll`
                /// perma-ready after the future has reached its final state.
                ///
                /// See \ref epoll_latch_event_t for more information.
                epoll_latch_event_t epoll_latch;
            #endif
        } unordered;

        /// Repeating timer state
        ///
        /// This union variant corresponds to \ref TYPE_TIMER_REPEAT. It tracks
        /// the state needed to report how many timer ticks elapsed and how to
        /// await subsequent timer ticks.
        struct {
            /// Result of the asynchronous operation
            ///
            /// This field is set before signaling \ref OUTCOME_SUCCESS. It
            /// indicates how many clock ticks were missed and how to await
            /// further clock ticks if desired.
            udipe_timer_repeat_payload_t payload;

            #ifdef __linux__
                /// timerfd that tracks recuring deadlines
                ///
                /// This inner fd is attached to `status_sync.latched_epoll`.
                /// See \ref inner_fd_t for more information about this
                /// cascading file descriptor pattern.
                ///
                /// It must be read under `lazy_lock` protection, and eventually
                /// detached from `status_sync.latched_epoll` and attached to
                /// the `latched_epoll` of the successor future (if any) once a
                /// result is ready.
                ///
                /// It must be destroyed when the future is liberated, for now.
                /// We may switch to disarming and recycling if timerfd
                /// creation/destruction ever becomes a bottleneck, but that
                /// seems unlikely under correct usage since there is no
                /// envisioned use case where one would need lots of periodic
                /// futures with different periodicities.
                //
                // TODO: Prove the above assertion through benchmarking and
                //       profiling of real-world workloads.
                // TODO: Find a windows equivalent, based on Win32 thread pool
                //       timers? That seems necessary to be able to count missed
                //       deadlines, which is a very nice timerfd feature that
                //       we'd rather keep even for those poor Windows souls.
                inner_fd_t timerfd;

                /// Event object used to keep `status_sync.latched_epoll`
                /// perma-ready after the future has reached its final state.
                ///
                /// See \ref epoll_latch_event_t for more information.
                epoll_latch_event_t epoll_latch;
            #endif
        } timer_repeat;
    } specific;

    /// Status word
    ///
    /// This innocent-looking 32-bit word actually contains most of the
    /// synchronization-critical state of a future, bitpacked via \ref
    /// future_status_word_t::as_word so that it can be used for atomic
    /// read-modify-write operations and futex syscalls.
    ///
    /// A future's status word does double duty as a futex that can sometimes
    /// (but not always) be awaited with wait_for_address() to await
    /// `status_word` changes. When a future supports this signaling protocol,
    /// it must be requested first by setting the `notify_address` field of the
    /// status word, before beginning the wait for status changes via
    /// wait_for_address().
    ///
    /// Please refer to \ref future_status_t for more information about what
    /// information is stored into this word.
    ///
    /// As status changes are often preceded by other future state changes, bear
    /// in mind that changes to `status_word` must often be carried out with
    /// `memory_order_release` and status word readouts must often be carried
    /// out with `memory_order_acquire`.
    _Atomic uint32_t status_word;

    /// Synchronization object signaling future status changes
    ///
    /// See \ref status_sync_t for more information.
    status_sync_t status_sync;
};
static_assert(alignof(udipe_future_t) == FALSE_SHARING_GRANULARITY,
              "Each future potentially synchronizes different workers and "
              "client threads, and should therefore reside on its own "
              "false sharing granule");
static_assert(sizeof(udipe_future_t) == FALSE_SHARING_GRANULARITY,
              "Should not need more than one false sharing granule per future");
static_assert(
    offsetof(udipe_future_t, status_sync) + sizeof(uint32_t) <= CACHE_LINE_SIZE,
    "Should fit on a single cache line for optimal memory access performance "
    "on CPUs where the FALSE_SHARING_GRANULARITY upper bound is pessimistic"
);
static_assert(sizeof(udipe_result_t) <= CACHE_LINE_SIZE,
              "Should always be true because future is a superset of result");

/// \}


/// \name Basic future lifecycle
/// \{

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
/// allocator such as \ref future_cache_base_t, which enable maximally efficient
/// thread-local operation in well-behaved applications while still enabling a
/// reasonable degree of future reuse in less well-behaved applications.
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
/// - Targeted by the `bottom` pointer of the host \ref future_pointer_cache_t if
///   it is the first inserted pointer page in said cache.
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

/// Indices for ring buffer cache management
///
/// File descriptor caches currently use a ring buffer layout, where this pair
/// of indices is used to implement the ring buffer logic.
typedef struct ring_indices_s {
    /// Index of the newest entry, if any
    ///
    /// When this index is equal to `before_oldest`, the cache is empty.
    uint8_t newest;

    /// Index before that of the oldest event object, if any
    ///
    /// If an attempt to advance `newest` makes it reach this value, the cache
    /// is full, and at least one older entry must be discarded.
    uint8_t before_oldest;
} ring_indices_t;

/// Event cache capacity
///
/// Consider increasing this limit if profiling shows that application
/// performance is limited by the performance of allocating and liberating event
/// objects. The reason why a conservative capacity was chosen in the initial
/// implementation is that Unix systems have an absurdly low default limit on
/// the maximum number of file descriptors that a process can use, and cached
/// event objects eat into this precious resource. However, sysadmins can easily
/// bump the file descriptor limit, and that should be the preferred option if
/// we make a convincing case that doing so enables better performance.
///
/// You should probably keep this a power of two, as otherwise the performance
/// of the ring buffer integer arithmetic performed by the event cache is
/// expected to decrease dramatically.
#define EVENT_CACHE_CAPACITY  ((size_t)8)

/// Cache for unallocated and unsignaled event objects
///
/// This cache uses a simple ring buffer layout, where newly liberated event
/// objects are inserted at `indices.newest` (which increments it with circular
/// wraparound) and allocated events are grabbed from the same end of the ring
/// (which decrements `indices.newest` with circular wraparound).
///
/// The ring buffer is considered empty when `indices.before_oldest ==
/// indices.newest` and it is considered full when `indices.before_oldest` is
/// `indices.newest - 1` modulo \ref EVENT_CACHE_CAPACITY. Attempting to
/// liberate an event into a full local event cache is handled by...
///
/// - Checking if the global event cache is also full.
/// - When it is not full, transfering up to half of the oldest events from this
///   local cache (on the `indices.before_oldest` end) to the global cache, thus
///   making room for the newly liberated event.
/// - When it is full, destroying the oldest event (at index
///   `indices.before_oldest + 1` modulo \ref EVENT_CACHE_CAPACITY) to make room
///   for the new event.
///
/// Similarly, attempting to allocate an event from an empty local event cache
/// is handled by...
///
/// - Checking if the global event cache is also empty.
/// - If it is not empty, transferring as many events as possible from the
///   global cache to this local cache, then allocating one of these events.
/// - If it is empty, creating a new event object.
typedef struct event_cache_s {
    /// Storage for unallocated and unsignaled event objects
    ///
    /// To make resource management bugs easier to detect, unused entries should
    /// be set to \ref EVENT_INVALID, at least in debug builds.
    event_t events[EVENT_CACHE_CAPACITY];

    /// Indices of the newest and oldest event objects, if any
    ///
    ring_indices_t indices;
} event_cache_t;

#ifdef __linux__
    /// epollfd+eventfd cache capacity
    ///
    /// Tune this capacity up if allocating epollfds and binding eventfds to
    /// them becomes a bottleneck, following the same rules as for \ref
    /// EVENT_CACHE_CAPACITY.
    #define EPOLL_EVENT_CACHE_CAPACITY  ((size_t)4)

    /// Cache for epollfds with pre-attached eventfds
    ///
    /// On Linux, many future types follow the pattern of exposing an epollfd as
    /// their `status_sync` file descriptor, which is attached to an eventfd
    /// which is used to signal task completion, and also to some other file
    /// descriptors of interest that signal progress towards task completion.
    ///
    /// For future types where only one more file descriptor is attached to the
    /// epollfd, the overhead of allocating the epollfd and attaching an eventfd
    /// to it, then eventually destroying the epollfd and eventfd, is expected
    /// to be significant. Therefore, pre-coupled (epollfd, eventfd) pairs are
    /// recycled and kept around in this cache.
    ///
    /// For \ref TYPE_JOIN where arbitrarily many file descriptors are attached
    /// to the epollfd in addition to the eventfd, the overhead associated with
    /// the core epollfd+eventfd pair is expected to be smaller with respect to
    /// the overhead of attaching all other file descriptors, and the overhead
    /// of resetting the epollfd back to the initial state is also potentially
    /// greatly increased. Therefore epollfd+eventfd pairs are not currently
    /// cached for this future type. If this turns out to be a performance
    /// problem in the future, this policy can be revisited in favor of a more
    /// nuanced policy where the epollfd+eventfd pair is recycled for
    /// "sufficiently small" joins.
    ///
    /// Aside from the fact that each cache entry index corresponds to two
    /// pre-coupled file descriptors (one in `epolls`, one in `events`), this
    /// cache works exactly like \ref event_cache_t.
    typedef struct epoll_event_cache_s {
        /// Storage for epollfds
        ///
        /// For each valid index `i`, `epolls[i]` should be an epollfd that is
        /// attached to `events[i]` and no other file descriptor.
        ///
        /// To make resource management bugs easier to detect, unused entries
        /// should be set to -1, at least in debug builds.
        int epolls[EPOLL_EVENT_CACHE_CAPACITY];

        /// Storage for event objects, which are eventfds on Linux
        ///
        /// For each valid index `i`, `events[i]` should be an eventfd in an
        /// unsignaled state.
        ///
        /// To make resource management bugs easier to detect, unused entries
        /// should be set to \ref EVENT_INVALID, at least in debug builds.
        event_t events[EPOLL_EVENT_CACHE_CAPACITY];

        /// Indices of the newest and oldest epollfd+eventfd pairs, if any
        ///
        ring_indices_t indices;
    } epoll_event_cache_t;
#endif

/// Cache for unallocated futures and associated resources
///
/// udipe uses a two-level cache to avoid repeated allocation and liberation of
/// futures and a subset of associated system resources (e.g. some file
/// descriptors on Linux). The cache roughly works as follows:
///
/// - Each user thread gets a thread-local cache, where futures and associated
///   building blocks are recycled upon liberation. When a new future is
///   allocated, resources are fetched from this cache if available, which is
///   the fastest path as it requires no syscalls and no synchronization with
///   other threads. If the thread-local cache does not have some of the
///   required resources, the global cache is queried for those resources.
/// - The entire process gets a mutex-protected global cache, shared across all
///   udipe contexts. This cache is where thread-local caches spill when they
///   are full or the associated thread exits. User threads go looking for
///   resources in this global cache when their thread-local cache is empty.
///   When the process exits, resources from this global cache are finally
///   liberated, thus avoiding leak reports from dynamic analysis tools.
/// - When a desired resource is not available in either the thread-local or the
///   global cache, a new batch of this resource is allocated and inserted into
///   the thread-local cache, where it is available for both the current and
///   future allocations.
///
/// Each level of cache has the same structure described below, but the global
/// cache supplements this common structure with an extra mutex + some atomic
/// flags to enable multi-threaded access.
typedef struct future_resource_cache_s {
    /// Unallocated `udipe_future_t*` pointers
    ///
    /// See \ref future_cache_t for more information.
    future_pointer_cache_t futures;

    /// Unallocated unsignaled event objects
    ///
    /// See \ref event_cache_t for more information.
    event_cache_t events;

    #ifdef __linux__
        /// Unallocated epollfds with pre-attached eventfds
        ///
        /// See \ref epoll_event_cache_t for more information.
        epoll_event_cache_t epolls_with_events;
    #endif
} future_resource_cache_t;

// FIXME: Finish rewriting of the following according to the new future caching
//        logic described above: caches now belong to a context, not to a thread
//        or a process, and their liberation policy has been changed
//        accordingly.

// TODO: Thread-local cache with...
//
//       - A future_local_resource_cache_t
//       - A pointer to the future_global_cache_t that it is attached to
//       - A once_flag that is used to select a resource liberation method:
//          - Spilling resources to the global cache on thread exit, done if the
//            thread exits before the underlying context is destroyed.
//          - Liberating all resources, done if the context is destroyed before
//            the thread exits.
//       - An atomic bitfield that tracks the two conditions for liberating the
//         heap allocation where this struct resides...
//         - The thread that owns this cache is done processing it as part of
//           its exit procedure.
//         - The global context associated with this cache is done processing it
//           as part of its destruction procedure.
//       - A pointer to another struct of the same type, used to build a linked
//         list in TLS so that a thread can attach to multiple contexts, with a
//         comment that such multi-context use is expected to be so rare and so
//         small-scale that the linked list performance shouldn't matter.
//
//       ...all of which must go into their own allocation because the lifetime
//       constraints of this struct are complicated.
//
//       When the thread associated with this cache exits, its TLS destructor
//       iterates over the linked list of these in TLS, setting pointers to NULL
//       right before jumping to the next item (see below for details). For each
//       item in the list...
//
//       - We checks the liberation status bitfield. The bit which tracks
//         whether this thread is done processing it should be cleared, which we
//         can assert. The information of interest is whether the global context
//         is done processing this local cache as part of its destruction
//         procedure or not.
//       - If this flag is set, we must do an acquire barrier, then can conclude
//         that the context has been liberated first and must have cleared this
//         cache in the process. Thus we can quickly assert that this local
//         cache is indeed cleared, then liberate the entire struct.
//       - If this flag is cleared, we do not know if the context has started
//         liberating and has potentially started clearing this cache in the
//         process, but know it is unlikely. Thus we use the once_flag to spill
//         to the global cache only if we got here first, and replace the
//         next-in-list pointer with NULL. Then we use atomic_or with acq_rel
//         ordering to set our own "done" flag to true, and if this gets us to a
//         state where all flags are set, we can liberate this struct.
//
//       Any other access to the local cache is unsynchronized, but should
//       assert that the cache is not in a destroyed state.

/// Global future cache
///
/// See the documentation of \ref future_resource_cache_t for more information
/// on the overall cache structure.
typedef struct future_global_cache_s {
    /// Mutex used to synchronize access to the global cache
    ///
    alignas(FALSE_SHARING_GRANULARITY) mtx_t mutex;

    /// Actual cache whose accesses must be mutex-protected
    ///
    future_resource_cache_t cache;

    // TODO: Linked list of future storage pages

    // TODO: Array of pointers to attached thread-local caches
    //
    //       When the global cache is destroyed by udipe_finalize(), before we
    //       start destroying any global resource in `cache`, we iterate over
    //       those and for each of them...
    //
    //       - We check the liberation status bitfield. The bit which tracks
    //         whether this context has been destroyed yet should be cleared,
    //         which we can assert. The information of interest is whether the
    //         thread is done processing this local cache as part of its exit
    //         procedure.
    //       - If this flag is set, we must do an acquire barrier, then we can
    //         conclude that the thread has exited first and must have spilled
    //         this local cache to the global cache in the process. Thus we can
    //         quickly assert that the local cache is indeed cleared, then
    //         liberate the struct.
    //       - If this flag is cleared, we do not know if the thread has started
    //         exited and has potentially started spilling this cache in the
    //         process, but know it is unlikely. Thus we use the once_flag to
    //         clear this cache only if we got here first, then we use atomic_or
    //         with acq_rel ordering to set our own "done" flag to true, and if
    //         this gets us to a state where all "done" flags are set, we can
    //         liberate this struct.
    //
    //       Once done, we set all these pointers to `NULL`, then proceed to
    //       liberate the global resources too.

    /// Truth that the event cache is full
    ///
    /// To minimize thread contention, this flag + the next ones are on a
    /// separate false sharing granule that only contains read-mostly variables.
    //
    // TODO: Remember to update this on global cache manipulations
    alignas(FALSE_SHARING_GRANULARITY) atomic_bool event_cache_full;

    /// Truth that the event cache is empty
    ///
    // TODO: Remember to update this on global cache manipulations
    atomic_bool event_cache_empty;

    #ifdef __linux__
        /// Truth that the epollfd + eventfd cache is full
        ///
        // TODO: Remember to update this on global cache manipulations
        atomic_bool epoll_event_cache_full;

        /// Truth that the epollfd + eventfd cache is empty
        ///
        // TODO: Remember to update this on global cache manipulations
        atomic_bool epoll_event_cache_empty;
    #endif
} future_global_cache_t;

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

/// \}


#ifdef UDIPE_BUILD_TESTS
    /// Unit tests
    ///
    /// This function runs all the unit tests for this module. It must be called
    /// within the scope of with_logger().
    void future_unit_tests();
#endif
