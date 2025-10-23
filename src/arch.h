#pragma once

//! \file
//! \brief Hardware-specific definitions
//!
//! This code module contains preprocessor defines that encode compile-time
//! knowledge about supported CPU architectures, x86_64 only for now.

#include <stddef.h>


/// Upper bound on the CPU memory access granularity in bytes
///
/// This is used as a struct member alignment to prevent false sharing.
///
/// x86_64 specific for now, add ifdefs once more hardware becomes supported.
///
/// This is 128B and not 64B as you might expect because according to the Intel
/// optimization manual, some modern x86_64 CPUs fetch data at the granularity
/// of pairs of cache lines, doubling the false sharing granularity.
#define FALSE_SHARING_GRANULARITY ((size_t)128)

/// Smallest available memory page size
///
/// x86_64 specific for now, add ifdefs once more hardware becomes supported.
#define LOWEST_PAGE_SIZE ((size_t)4096)

#ifdef __x86_64__
    /// Minimum guaranteed page alignment
    ///
    /// This is used to improve compiler optimizations around allocate().
    #define LOWEST_PAGE_ALIGNMENT ((size_t)4096)
#else
    /// Minimum guaranteed page alignment
    ///
    /// Unfortunately, on this particular hardware architecture we do not know,
    /// so we stick with the minimum alignment guaranteed by malloc() i.e. large
    /// enough to align any standard type.
    ///
    /// But if you are reading this on doxygen, note that it may be an artifact
    /// of doxygen's parser not setting the hardware architecture preprocessor
    /// defines that normal compilers do set.
    #define LOWEST_PAGE_ALIGNMENT alignof(max_align_t)
#endif
