#pragma once

//! \file
//! \brief Buffer allocator
//!
//! This header is the home of \ref buffer_allocator_t, the specialized memory
//! allocator that is internally used by `libudipe` worker threads to allocate
//! storage for incoming or outgoing datagrams.

#include <udipe/buffer.h>
#include <udipe/pointer.h>

#include "arch.h"
#include "bit_array.h"
#include "sys/memory.h"

#include <hwloc.h>


/// Buffer allocator
///
/// Each `libudipe` worker thread sets up its own \ref buffer_allocator_t on
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
/// An allocator is set up using buffer_allocator_initialize() and destroyed
/// using buffer_allocator_finalize().
typedef struct buffer_allocator_s {
    /// Memory pool base pointer
    ///
    /// This points to the first page of memory that was allocated when this
    /// allocator was set up.
    void* memory_pool;


    /// Configuration of this allocator
    ///
    /// This contains the final configuration after replacing placeholder zeroes
    /// with default values and rounding up the buffer size to the next multiple
    /// of the system's page size.
    udipe_buffer_config_t config;

    /// Bit array of buffer availability within the memory pool
    ///
    /// The N-th bit within this bit array tracks whether the N-th buffer (where
    /// N is between 0 and \link #udipe_buffer_config_t::buffer_count
    /// config.buffer_count\endlink) is currently available for use.
    ///
    /// A set bit means that a buffer is available for use, a cleared bit means
    /// that it is currently allocated.
    INLINE_BIT_ARRAY(buffer_availability, UDIPE_MAX_BUFFERS);
} buffer_allocator_t;

/// Initialize a \ref buffer_allocator_t.
///
/// The buffer allocator must later be liberated using
/// buffer_allocator_finalize().
///
/// This function must be called within the scope of with_logger().
///
/// \param configurator indicates how the user wants the allocator to be
///                     configured.
/// \param topology is an hwloc topology used for the default allocator
///                 configuration, which is optimized for L1/L2 cache locality.
UDIPE_NON_NULL_ARGS
buffer_allocator_t
buffer_allocator_initialize(udipe_buffer_configurator_t configurator,
                            hwloc_topology_t topology);

/// Finalize a \ref buffer_allocator_t.
///
/// All former allocations should have been liberated with buffer_liberate()
/// before calling this function. The buffer allocator cannot be used again
/// after this is done.
///
/// This function must be called within the scope of with_logger().
///
/// \param allocator points to an allocator that has previously been set up
///                  using allocator_initialize() and hasn't been destroyed
///                  through allocator_finalize() yet.
UDIPE_NON_NULL_ARGS
void buffer_allocator_finalize(buffer_allocator_t* allocator);

/// Liberate a memory buffer previously allocated via buffer_allocate()
///
/// After this is done, the buffer must not be used again for any purpose.
///
/// This function must be called within the scope of with_logger().
///
/// \param allocator points to an allocator that has previously been set up
///                  using buffer_allocator_initialize() and hasn't been
///                  destroyed with buffer_allocator_finalize() yet.
/// \param buffer points to a buffer that has previously been allocated from
///               `allocator` using buffer_allocate() and hasn't been liberated
///               via buffer_liberate() yet.
UDIPE_NON_NULL_ARGS
void buffer_liberate(buffer_allocator_t* allocator, void* buffer);

/// GNU attributes of the buffer_allocate() functions
///
/// These attributes are used to let GCC and clang know that buffer_allocate()
/// is a memory allocator that provides certain guarantees and is meant to be
/// used in a certain way. These compilers can leverage that information to
/// optimize code better and provide higher quality static analysis.
#ifdef __GNUC__
    #define BUFFER_ALLOCATE_ATTRIBUTES  \
        PAGE_ALLOCATOR_ATTRIBUTES __attribute__((malloc(buffer_liberate, 2)))
#else
    #define BUFFER_ALLOCATE_ATTRIBUTES PAGE_ALLOCATOR_ATTRIBUTES
#endif

/// Attempt to allocate a memory buffer
///
/// Returns `NULL` if no buffer is available, in which case the caller should
/// wait for some network requests to complete (and thus liberate the associated
/// data buffer) before trying again.
///
/// If this function returns a non-`NULL` buffer, then it must later be
/// liberated using the buffer_liberate() function.
///
/// This function must be called within the scope of with_logger().
///
/// \param allocator points to an allocator that has previously been set up
///                  using buffer_allocator_initialize() and hasn't been
///                  destroyed through buffer_allocator_finalize() yet.
/// \returns a buffer of size \link
///          #udipe_buffer_config_t::buffer_size
///          allocator->config.buffer_size\endlink, or `NULL` if no buffer is
///          presently available for use.
UDIPE_NON_NULL_ARGS
BUFFER_ALLOCATE_ATTRIBUTES
void* buffer_allocate(buffer_allocator_t* allocator);


#ifdef UDIPE_BUILD_TESTS
    /// Unit tests
    ///
    /// This function runs all the unit tests for this module. It must be called
    /// within the scope of with_logger().
    void buffer_unit_tests();
#endif
