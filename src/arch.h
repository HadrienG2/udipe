#pragma once

//! \file
//! \brief Hardware-specific definitions
//!
//! This code module contains preprocessor defines that encode compile-time
//! knowledge about supported CPU architectures.

#include <assert.h>
#include <stddef.h>


/// Upper bound on the CPU's memory access granularity in bytes
///
/// This is the alignment that is set on struct members that are shared between
/// threads in order to avoid false sharing issues.
///
/// The current definition is known to work for x86_64, aarch64 and powerpc64.
/// It should be extended with ifdefs whenever the need arises as more CPU
/// architectures become supported.
///
/// \internal
///
/// This is 128B and not 64B as you might expect because according to the Intel
/// optimization manual, some modern x86_64 CPUs fetch data at the granularity
/// of pairs of cache lines, effectively doubling the false sharing granularity.
/// with respect to the cache line size that is normally used.
///
/// However, not all x86_64 CPUs implement such pairwise cache line fetching, so
/// when you aim for best spatial cache locality, 64B remains the maximal data
/// structure size that you should aim for on x86_64.
#define FALSE_SHARING_GRANULARITY ((size_t)128)

/// Lower bound on the CPU cache line size, in bytes
///
/// This is the size that any data structure which is not manipulated in array
/// batches should strive to stay under for optimal access performance.
///
/// This number is only used for testing at the time of writing, so it's fine
/// (although obviously not ideal) if the estimate is off.
///
/// The current definition is known to work for x86_64, and should be extended
/// with ifdefs whenever the need arises as more CPU architectures become
/// supported.
#define CACHE_LINE_SIZE ((size_t)64)
static_assert(FALSE_SHARING_GRANULARITY % CACHE_LINE_SIZE == 0,
              "The CPU should access data at the granularity of cache lines");

/// Expected size of the smallest memory page available, in bytes
///
/// This is used to set the size of the flexible array inside of
/// mmap()-allocated storage buffers that are meant to fit in one memory page.
///
/// For this use case, it is okay if the value of the constant is wrong (we just
/// allocate more pages than we should which is not the end of the world), so
/// we tolerate an incorrect estimate on unknown CPU architectures.
///
/// The current definition is x86_64 specific, but coincidentally happens to
/// work for several other popular CPU architectures. Extend if with ifdefs as
/// required once more CPU architectures with other page sizes become supported.
#define EXPECTED_MIN_PAGE_SIZE ((size_t)4096)

/// Lower bound on the memory page alignment, in bytes
///
/// This is used to improve compiler optimizations around allocate() by telling
/// the compiler how aligned allocations are guaranteed to be.
///
/// Unlike \ref EXPECTED_MIN_PAGE_SIZE, this definition is a **guaranteed**
/// lower bound, and failure to meet it will result in undefined behavior. Which
/// is why on CPU architectures where the page size isn't known, a very
/// pessimistic guess is taken.
#if defined(__x86_64__) || defined(_M_X64)
    #define MIN_PAGE_ALIGNMENT ((size_t)4096)
#else
    #warning "Compiling on an unknown CPU architectures, will take a " \
             "pessimistic lower bound for MIN_PAGE_ALIGNMENT."
    #define MIN_PAGE_ALIGNMENT alignof(max_align_t)
#endif
