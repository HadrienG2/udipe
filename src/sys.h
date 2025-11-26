#pragma once

//! \file
//! \brief Operating system-specific definitions
//!
//! This code module provides a few primitives that abstract away differences
//! between supported operating systems.

#include <stddef.h>


// TODO: Extract some of the GNU attributes from buffer.h into a common macro
//       that also applies here

// TODO: Add a way to query the system page size and allocation granularity

/// Allocate memory optimized for use by network threads
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
///   normal free(), it **must** be freed using network_free().
//
// TODO: Add proper GNU attributes
// TODO: Implement as directed below
// TODO: On linux, mmap() then try mlock() and if it fails due to a permission
//       error warn() then simply prefault, using a page size readout that is
//       only performed once.
// TODO: On windows, get system info once to know page size and allocation
//       granularity, then round up to allocation granularity multiple, then
//       bump the process working set size with a mutex to avoid inter-thread
//       race, then allocate with reserve|commit, then try to lock and prefault
//       if it fails as on Linux.
void* network_alloc(size_t size);

// TODO: Add and document network_free() function that frees up memory

// TODO: Modify buffer.h and context.c to use network_alloc() and network_free().
