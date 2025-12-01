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


/// \name Thread identification
/// \{

/// Maximum thread name length that is guaranteed to be supported by all
/// udipe-supported operating systems
///
/// See set_thread_name() for more information about the various restrictions
/// that apply to thread names.
#define MAX_THREAD_NAME_LEN ((size_t)16)

/// Set the name of the calling thread
///
/// To accomodate the limitations of all supported operating systems and ensure
/// that thread names will not be mangled by any of them, said names must
/// honor the following restrictions:
///
/// - Only use printable ASCII code points except for the trailing NUL. No
///   Unicode tricks allowed here.
/// - Feature exactly one occurence of NUL at the end, like all C strings.
/// - Be no longer than \ref MAX_THREAD_NAME_LEN bytes, including the
///   aforementioned trailing NUL.
///
/// Since \ref MAX_THREAD_NAME_LEN is very short (only 15 useful ASCII chars on
/// Linux), it is recommended to simply give the thread a summary identifier
/// whose semantics are further detailed via logging.
///
/// For example, a backend that spawns one thread per connection could name its
/// threads something like `udp_cx_89ABCDEF`, with a 32 bit hex identifier at
/// the end which is just the index of the connexion thread in some internal
/// table. When the connection thread is created, it emits an `INFO` log message
/// announcing that it is in charge of handling a connexion with certain
/// properties, and thus the user should be able to tell which thread handles
/// which peer(s).
///
/// If the user decides to be difficult by using multiple udipe contexts at the
/// same time, we must detect this and switch to a less optimal naming
/// convention that handles multiple contexts like one that is based on TID
/// (`udp_th_89ABCDEF`), otherwise we'll get multiple threads with the same name
/// which is quite bad for ergonomics.
///
/// This function must be called within the scope of with_logger().
///
/// \param name is a thread name that must follow the constraints listed above.
//
// TODO: Implement. On windows, we can leverage the fact that length is bounded
//       to 16 ASCII-only chars by simply stack-allocating an array of 16
//       wchars which will be used for MultiByteToWideChar().
UDIPE_NON_NULL_ARGS
void set_thread_name(const char* name);

/// Get the name of the calling thread
///
/// Although udipe names its worker threads under the constraints spelled out in
/// the documentation of \ref set_thread_name(), callers of this function should
/// be ready for names that do not follow these constraints when it is called on
/// client threads not spawned by udipe.
///
/// Indeed, these client threads may have been named by the application on an
/// operating system where thread names are less constrained than the lowest
/// constraint denominator used by udipe.
///
/// \returns the name of the current thread, or a stringified hexadecimal TID
///          like `tid_89ABCDEF` if the current thread is not named. This name
///          string cannot be modified and may only be used until the next call
///          to \ref get_thread_name().
//
// TODO: Implement. To handle the unpredictable name length, allocate a
//       thread-local buffer whose size grows as much as needed, with a
//       thread-local key destructor that liberates the buffer.
UDIPE_NON_NULL_RESULT
const char* get_thread_name();

// TODO: Add unit tests for these functions, then replace prctl for thread name
//       in logger

/// \}


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
