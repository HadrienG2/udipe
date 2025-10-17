#pragma once

//! \file
//! \brief Memory allocator
//!
//! This header is the home of \ref allocator_t, the memory allocator
//! that is internally used by `libudipe` worker threads.

#include <udipe/allocator.h>
#include <udipe/pointer.h>

#include <hwloc.h>
#include <stddef.h>


/// Memory allocator
///
/// Each `libudipe` worker thread sets up its own \ref allocator_t on
/// startup, which manages a pool of identically sized page-aligned buffers.
///
/// In the default configuration, which can be overriden, the size of individual
/// buffers is chosen to fit the CPU's L1 cache. And the number of buffers in
/// the memory pool is chosen such that all buffers collectively fit in L2 cache
/// (or an even share of it if the L2 cache is shared across CPU cores).
///
/// Every concurrent network operation can request a buffer from this allocator
/// until the point where the pool fills up. Once that point is reached,
/// allocations fail to signal that the newly incoming operation cannot be
/// scheduled until some outstanding operation complete.
///
/// Together, these design choices should ensure good CPU cache locality and
/// minimize the risk of interference between `libudipe` worker threads and
/// unrelated threads, as long as such threads are kept out of the CPU cores
/// that `libudipe` uses via appropriate CPU pinning.
///
/// The use of homogeneously sized buffers is a design bet, whose validity is
/// not proven yet. It allows the allocator implementation to be extremely
/// simple and efficient, at the expense of providing inadequately sized
/// allocations for some tasks. The bet here is that this simplicity is good
/// enough for UDP communication (especially if GRO is used), so we can just
/// stop worrying and enjoy the simplicity/speed. We'll see how that plays out
/// as the project develops (TODO: evaluate once benchmarking allows for it).
///
/// An allocator is set up using allocator_initialize() and destroyed using
/// allocator_finalize().
typedef struct allocator_s {
    /// Memory pool base pointer
    ///
    /// This points to the first page of memory that was allocated when this
    /// allocator was set up.
    void* memory_pool;


    /// Configuration of this allocator
    ///
    /// This contains the final configuration after replacing placeholder zeroes
    /// with default values and rounding up to the next multiple of the system's
    /// page size.
    udipe_thread_allocator_config_t config;

    /// Bitmap of buffer availability within the memory pool
    ///
    /// The N-th within this bitmap tracks whether the N-th buffer (where N is
    /// between 0 and \link #udipe_thread_allocator_config_t::buffer_count
    /// config.buffer_count \endlink) is currently available for use.
    ///
    /// A set bit means that a buffer is available for use
    size_t buffer_availability[UDIPE_MAX_USAGE_WORDS];
} allocator_t;


/// Initialize a \link #allocator_t memory allocator \endlink.
///
/// The memory allocator must later be liberated using allocator_finalize().
///
/// This function must be called within the scope of with_logger().
///
/// \param config indicates how the user wants the allocator to be configured.
///
/// \param topology is an hwloc topology used for the default allocator
///                 configuration, which is optimized for L1/L2 cache locality.
UDIPE_NON_NULL_ARGS
allocator_t allocator_initialize(udipe_allocator_config_t config,
                                 hwloc_topology_t topology);


/// Finalize a \link #allocator_t memory allocator \endlink.
///
/// The memory allocator cannot be used again after this is done.
///
/// This function must be called within the scope of with_logger().
void allocator_finalize(allocator_t allocator);


/// Liberate a memory buffer previously allocated via allocate()
///
/// After this is done, the buffer must not be used again for any purpose.
///
/// This function must be called within the scope of with_logger().
///
/// \param allocator points to an allocator that has previously been set up
///                  using allocator_initialize() and hasn't been destroyed
///                  through allocator_finalize() yet.
///
/// \param buffer points to a buffer that has previously been allocated from
///               `allocator` using allocate() and hasn't been destroyed through
///               liberate().
UDIPE_NON_NULL_ARGS
void liberate(allocator_t* allocator, void* buffer);


#ifdef __x86_64__
    /// Minimum page alignment
    ///
    /// This is used to improve compiler optimizations around allocate().
    #define MIN_PAGE_ALIGNMENT 4096
#else
    /// Minimum page alignment
    ///
    /// Unfortunately, on this particular hardware architecture we do not know,
    /// so we stick with the minimum alignment guaranteed by malloc() i.e. large
    /// enough to align any standard type.
    #define MIN_PAGE_ALIGNMENT alignof(max_align_t)
#endif


/// GNU attributes of the allocate() functions
///
/// These attributes are used to let the compiler know that allocate() is a
/// memory allocator that provides certain guarantees and expects certain usage
/// requirements, in order to enjoy higher-quality performance optimization and
/// static analysis. None of these attributes is mandatory for correctness.
#define ALLOCATE_ATTRIBUTES  \
    __attribute__((assume_aligned(MIN_PAGE_ALIGNMENT)  \
                 , malloc  \
                 , malloc(liberate, 2)  \
                 , warn_unused_result))


/// Attempt to allocate a memory buffer
///
/// Returns `NULL` if no buffer is available, in which case the caller should
/// wait for some network requests to complete (and thus liberate the associated
/// data buffer) before trying again.
///
/// This function must be called within the scope of with_logger().
///
/// \param allocator points to an allocator that has previously been set up
///                  using allocator_initialize() and hasn't been destroyed
///                  through allocator_finalize() yet.
///
/// \returns points to a buffer of size \link
///          #udipe_thread_allocator_config_t::buffer_size
///          allocator->config.buffer_size \endlink, or `NULL` if no buffer is
///          presently available for use.
UDIPE_NON_NULL_ARGS
ALLOCATE_ATTRIBUTES
void* allocate(allocator_t* allocator);
