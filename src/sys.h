#pragma once

//! \file
//! \brief Operating system-specific definitions
//!
//! This code module provides a few primitives that abstract away differences
//! between supported operating systems.

#include <udipe/pointer.h>

#include "arch.h"

#include <stddef.h>


/// \name Memory management
/// \{

/// Page size used for memory allocations
///
/// This is the alignment and size granularity of several important system
/// memory management processes including swapping and NUMA migrations.
/// Logically distinct activities (e.g. traffic associated with different
/// network connections) should thus take place in buffers that are aligned on
/// a page boundary and whose size is a multiple of the page size.
///
/// That property is implicitly ensured by realtime_allocate() for the buffer
/// that it returns. But if you intend to later suballocate that buffer into
/// smaller buffers, as you should, then you must be careful to round up the
/// sub-buffer size that you use to compute the total `size` that you pass down
/// to realtime_allocate() to a multiple of this quantity.
///
/// This function must be called within the scope of with_logger().
size_t get_page_size();

/// Liberate a memory buffer previously allocated via realtime_allocate()
///
/// After this is done, the buffer must not be used again for any purpose.
///
/// This function must be called within the scope of with_logger().
///
/// \param buffer points to a buffer that has previously been allocated using
///               realtime_allocate() and hasn't been liberated via
///               realtime_liberate() yet.
/// \param size must be the `size` parameter that was passed to
///             realtime_allocate() when this buffer was allocated.
UDIPE_NON_NULL_ARGS
void realtime_liberate(void* buffer, size_t size);

/// GNU attributes of page-aligned memory allocation functions
///
/// These attributes are used to let the compiler know that a certain function
/// allocates memory at page granularity, which enables better compiler
/// performance optimizations and static analysis.
///
/// They are best completed with the malloc(liberate...) attribute to indicate
/// what function is used to liberate the memory later on. For infaillible
/// allocators, UDIPE_NON_NULL_RESULT can be used as well.
#ifdef __GNUC__
    #define PAGE_ALLOCATOR_ATTRIBUTES  \
        __attribute__((assume_aligned(MIN_PAGE_ALIGNMENT)  \
                     , malloc  \
                     , warn_unused_result))
#else
    #define PAGE_ALLOCATOR_ATTRIBUTES
#endif

/// GNU attributes of the realtime_allocate() functions
///
/// These attributes are used to let GCC and clang know that realtime_allocate()
/// is a memory allocator that provides certain guarantees and is meant to be
/// used in a certain way. These compilers can leverage that information to
/// optimize code better and provide higher quality static analysis.
#ifdef __GNUC__
    #define REALTIME_ALLOCATE_ATTRIBUTES  \
        PAGE_ALLOCATOR_ATTRIBUTES  \
        __attribute__((malloc(realtime_liberate)))
#else
    #define REALTIME_ALLOCATE_ATTRIBUTES PAGE_ALLOCATOR_ATTRIBUTES
#endif

/// Allocate memory optimized for use by timing-sensitive network threads
///
/// Compared to standard malloc(), this memory allocation function takes a few
/// extra precautions that can benefit networking performance.
///
/// - The memory buffer will be pre-faulted into RAM, which reduces the risk of
///   packet drops at the beginning of the network exchange as a result of OS
///   kernels lazily allocate physical memory when it is first accessed.
/// - If the user is allowed to do it, the memory buffer will also be locked
///   into RAM, which ensures that the OS kernel cannot swap it out to disk.
///   This is good for high-throughput UDP connections, which may be idle for a
///   while but need to react very quickly once traffic starts coming in again.
///   * Memory locking is treated as a nice-to-have rather than a mandatory
///     requirement, so failing to do it due to a permission error will result
///     in a warning followed by the allocation being returned normally.
/// - Partly as a consequence of the above, the memory buffer is overaligned to
///   a page boundary and its size is rounded up to a multiple of the system
///   page size. This enables SIMD buffer processing code to be written in a
///   simpler and more efficient way.
///
/// The price to pay for these optimizations is that...
///
/// - The allocation will be initially resident on the NUMA node that allocated
///   it, so allocations that are private to a worker thread should be allocated
///   by said worker thread not the main thread, and a general effort should be
///   made to pin each worker on a single NUMA node and avoid inter-worker
///   communication if workers span multiple NUMA nodes.
/// - The allocation will be rounded up to the next multiple of the system
///   allocation granularity. This can increase the memory footprint of the
///   application if many buffers are allocated this way. Generally speaking,
///   you should try to request as few of these allocations as possible by
///   allocating large blocks and logically splitting them into smaller ones.
/// - The allocation that comes out of this function cannot be freed using
///   normal free(), it must be freed using realtime_liberate().
///
/// As with standard malloc(), `size` must not be 0.
///
/// This function must be called within the scope of with_logger().
///
/// \param size sets a lower bound on the size of the buffer that will be
///             returned, in bytes. Due to granularity constraints of the
///             underlying OS APIs, the amount of actually allocated memory may
///             be higher than what was requested.
/// \returns a buffer of `size` bytes or more. Failure is handled by aborting
///          the host program with exit().
UDIPE_NON_NULL_RESULT
REALTIME_ALLOCATE_ATTRIBUTES
void* realtime_allocate(size_t size);

/// \}


// TODO: Add a function to get/set the current thread name on posix and the
//       thread description on Windows + unit tests + replace prctl to get
//       thread name in logger.

// TODO: Add a function for futex wait/wake and the Windows equivalent + unit
//       tests + replace futex syscalls here and there.


/// \name Unit tests
/// \{

#ifdef UDIPE_BUILD_TESTS
    /// Unit tests for OS-specific functionality
    ///
    /// This function runs all the unit tests for OS-specific functionality. It
    /// must be called within the scope of with_logger().
    void sys_unit_tests();
#endif

/// \}
