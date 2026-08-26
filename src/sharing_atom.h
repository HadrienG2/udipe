#pragma once

//! \file
//! \brief \ref messaging_atom_t and simple allocator thereof
//!
//! TODO: Module doc after type doc to avoid repeating them

#include "arch.h"

#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>


/// Minimal memory allocation that can be used to pass messages across threads,
/// without encountering false sharing issues.
///
/// This is a generic storage location within which you can store any value that
/// must be aligned on a \ref FALSE_SHARING_GRANULARITY boundary for false
/// sharing avoidance, but needs not be larger than \ref
/// FALSE_SHARING_GRANULARITY.
///
/// Unfortunately, to comply with the strict aliasing rule of C, you cannot
/// access said value directly by casting `bytes` from `char*` to `T*` where `T`
/// is the actual type that you are interested in. Instead, you must use
/// memcpy() indirection:
///
/// - To change the value of type `T` stored within `bytes`, you must create a
///   local variable of type `T`, then memcpy() from it to `bytes`.
/// - To read the value of type `T` stored within `bytes`, you must create a
///   local variable of type `T`, then memcpy() from `bytes` to it.
///
/// This is not necessarily a performance problem, as compiler optimizations can
/// turn these memcpy()s into in-place accesses to `bytes`. But it does heavily
/// restrict what you can use these false sharing atoms for, as they can
/// basically only be used for message passing workloads where one thread writes
/// into an atom, then passes it to another threads and loses access in the
/// process as the other thread is reading from it.
///
/// You cannot, for example, have an atomic variable in these shared memory
/// locations that can be concurrently be accessed by multiple threads. All such
/// synchronization must be implemented using separate state that is managed in
/// its own false sharing atom, that will need to be fully managed using
/// dedicated code.
///
/// What is the point of this type then ? That when you need a simple allocator
/// for small messages to be passed across threads, you can just use the generic
/// \ref messaging_atom_allocator_t instead of needing to design and implement
/// one allocator for each message type that you're dealing with.
typedef struct messaging_atom_s {
    alignas(FALSE_SHARING_GRANULARITY) char bytes[FALSE_SHARING_GRANULARITY];
} messaging_atom_t;

/// Number of messaging atoms that can be managed by a \ref
/// messaging_atom_allocator_t
///
/// This value is constrained upwards by the Linux futex API which only supports
/// integers up to 32 bits, meaning that beyond that we need to use some layer
/// of indirection which in turn makes the synchronization protocol more costly
/// and complicated (need either a mutex or an ABA-resilient concurrent garbage
/// collection scheme).
///
/// We could, however, rather easily support allocators that manage a smaller
/// number of values if the need for them arises. Just replace the \ref
/// `messaging_atom_block_t*` in the allocator API with a raw
/// `messaging_atom_t[]` array pointer, provide a way to set up an allocator
/// where not all 32 entries are initially available, and document the resulting
/// new safety requirements.
#define MESSAGING_ATOM_BLOCK_LEN ((size_t)32)

/// Set of messaging atoms managed by a \ref messaging_atom_allocator_t
///
/// See \ref messaging_atom_allocator for more information.
typedef struct messaging_atom_block_s {
    messaging_atom_t atoms[MESSAGING_ATOM_BLOCK_LEN];
} messaging_atom_block_t;

/// Simple allocator of \ref messaging_atom_t
///
/// This is meant to be the simplest concurrent allocator that could possibly
/// work for \ref messaging_atom_t values. Consider using it when you need
/// threads to share a pool of messaging atoms and your requirements are easy
/// enough that neither the limitation to 32 managed atoms nor the need to
/// always allocate storage for these 32 atoms is a problem.
///
/// \internal
///
/// This allocator is based on the following design ideas:
///
/// - **Bitmap allocation:** An integer's bits are used to track which of the
///   managed \ref messaging_atom_t are available (bit set) and allocated (bit
///   cleared). This simple scheme works well for our region of the design space
///   where we're managing a small number of atoms, and thus the O(N) overhead
///   of bitmap operations is not a problem because most operations translate to
///   a tiny number of machine ops when SWAR techniques are used.
/// - **Futex-sized bitmap:** By forcing the size of our bitmap to be 32 bits,
///   we can use the Linux futex API and its cousins on other operating systems
///   to wait for the bitmap to move away from the 0 (fully allocated) state to
///   a state where some messaging atoms can be allocated. In this way, our
///   whole allocator state can fit into a single 32-bit word, we can use the
///   most efficient OS API available for awaiting events, and our allocator
///   allocation/deallocation cycle can be lock-free on the happy path where no
///   thread ever runs out of messaging atoms.
/// - **Storage/control separation:** This struct only tracks which elements of
///   an associated \ref messaging_atom_block_t are available, it does not
///   internally hold such a storage block. This enables more complex structs
///   like the command allocator to efficiently manage multiple \ref
///   messaging_atom_block_t by packing all the \ref messaging_atom_allocator_t
///   bitmaps together, at the expense of requiring more care on the user's side
///   (need to always pair a \ref messaging_atom_allocator_t with its designated
///   \ref messaging_atom_block_t on each API call).
typedef struct messaging_atom_allocator_s {
    /// Futex-sized atomic word that tracks which of the managed messaging atoms
    /// is available
    ///
    /// Each bit of `availability` tracks the availability of one \ref
    /// messaging_atom_t within the \ref messaging_atom_block_t that is managed
    /// by this allocator, using the following protocol:
    ///
    /// - Bits are numbered from lowest-order 0 (which tracks availability of
    ///   the first messaging atom) to highest-order 31 (which tracks
    ///   availability of the last messaging atom).
    /// - A set bit indicates that the corresponding messaging atom is
    ///   available, a cleared bit indicates that this messaging atom is
    ///   unavailable.
    /// - Resources are allocated and liberated using bitwise AND/OR atomic ops.
    /// - Threads that encounter a fully-unavailable bitmap can wait for it to
    ///   become available with wait_on_address().
    /// - Threads that liberate a messaging atom in an allocator that was
    ///   previously fully unavailable must signal this event using the
    ///   appropriate `wake_by_address_` function.
    _Atomic uint32_t availability;
} messaging_atom_allocator_t;

// TODO: API


// TODO: Tests