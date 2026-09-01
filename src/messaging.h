#pragma once

//! \file
//! \brief Messaging atoms and allocator thereof
//!
//! To avoid false sharing issues, small messages that are passed across threads
//! should be isolated into aligned "false sharing atoms" of size and alignment
//! given by \ref FALSE_SHARING_GRANULARITY. This code module simplifies the
//! management of such atoms in simple message-passing cases.

#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "arch.h"
#include "bits.h"

#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
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
/// What is the point of this type then? That when you need a simple allocator
/// for small messages to be passed across threads, you can just use the generic
/// \ref messaging_allocator_t instead of needing to design and implement one
/// allocator for each message type that you're dealing with.
typedef struct messaging_atom_s {
    alignas(FALSE_SHARING_GRANULARITY) char bytes[
        // Ensure that everything that fits into an x86/aarch64 cache line can
        // fit into this struct. This lets us design message structs around the
        // cache line size of currently dominant CPU architectures, while
        // keeping the code working if we ever need to port to (currently rare)
        // architectures with a smaller cache line size.
        MAX(FALSE_SHARING_GRANULARITY, 64)
    ];
} messaging_atom_t;

/// Number of messaging atoms that can be managed by a \ref
/// messaging_allocator_t
///
/// This value is constrained upwards by the Linux futex API, which only
/// supports integers up to 32 bits. Meaning that beyond that we need to use
/// some layer of indirection, which in turn makes the synchronization protocol
/// much more costly and complicated (need either a mutex or an ABA-resilient
/// concurrent garbage collection scheme).
///
/// We could, however, rather easily support allocators that manage a smaller
/// number of values if the need for them arises. Just replace the \ref
/// `messaging_block_t*` in the allocator API with a raw `messaging_atom_t[]`
/// array pointer, provide a way to set up an allocator where not all 32 entries
/// are initially available, and document the resulting new safety requirements.
#define MESSAGING_BLOCK_LEN 32

/// Set of messaging atoms managed by a \ref messaging_allocator_t
///
/// See \ref messaging_allocator for more information.
typedef struct messaging_block_s {
    messaging_atom_t atoms[MESSAGING_BLOCK_LEN];
} messaging_block_t;

/// Simple allocator of \ref messaging_atom_t
///
/// This is meant to be the simplest concurrent allocator that could possibly
/// work for \ref messaging_atom_t values. Consider using it when you need
/// threads to share a pool of messaging atoms and your requirements are easy
/// enough that neither the limitation to 32 managed atoms nor the need to
/// always allocate storage for these 32 atoms is a problem.
///
/// It must be initialized using the messaging_allocator_initialize() function.
///
/// \internal
///
/// This allocator is based on the following design ideas:
///
/// - **Bitmap allocation:** An integer's bits are used to track which of the
///   managed \ref messaging_atom_t are available (bit set) and allocated (bit
///   cleared). This simple scheme works well here as we're managing a small
///   number of atoms, and thus the O(N) overhead of bitmap operations is not a
///   problem because most operations translate to a tiny number of machine ops
///   when SIMD Within A Register (SWAR) techniques are used.
/// - **Futex-sized bitmap:** By forcing the size of our bitmap to be 32 bits,
///   we can use the Linux futex API and its cousins on other operating systems
///   to wait for the bitmap to move away from the 0 (fully allocated) state to
///   a state where some messaging atoms can be allocated. In this way, our
///   whole allocator state can fit into a single 32-bit word, we can use the
///   most efficient OS API available for awaiting events, and our allocator
///   allocation/deallocation cycle can be lock-free on the happy path where no
///   thread ever runs out of messaging atoms.
/// - **Storage/control separation:** This struct only tracks which elements of
///   an associated \ref messaging_block_t are available, it does not internally
///   hold such a storage block. This enables more complex structs like the
///   command allocator to efficiently manage multiple \ref messaging_block_t by
///   packing all the \ref messaging_allocator_t bitmaps together, at the
///   expense of requiring more care on the user's side (need to always pair an
///   allocator with its designated \ref messaging_block_t on each API call).
typedef struct messaging_allocator_s {
    /// Futex-sized atomic word that tracks which of the managed messaging atoms
    /// is available
    ///
    /// Each bit of `availability` tracks the availability of one \ref
    /// messaging_atom_t within the \ref messaging_block_t that is managed by
    /// this allocator, using the following protocol:
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
} messaging_allocator_t;

/// Initialize a \ref messaging_allocator_t
///
/// In order to use this allocator, you must separately set up a \ref
/// messaging_block_t for it. This messaging block must only ever be used along
/// with its associated allocator, it is only stored separately in order to
/// improve the layout of complex structs containing multiple allocators.
///
/// As the ISO C specification disallows zero initialization of atomics, you
/// must call this function in order to initialize a \ref messaging_allocator_t,
/// even when you intend it to be in a fully busy state.
UDIPE_NODISCARD
messaging_allocator_t messaging_allocator_initialize();

/// Finalize a \ref messaging_allocator_t
///
/// The allocator cannot be used again after this is done.
UDIPE_NON_NULL_ARGS
void messaging_allocator_finalize(messaging_allocator_t* allocator);

/// Allocate a \ref messaging_atom_t struct for the purpose of sending
/// a message to another thread.
///
/// If no messaging atom is currently available, this function will block until
/// a thread liberates one using messaging_atom_liberate().
///
/// \param allocator must be a \ref messaging_allocator_t that has been
///                  initialized using messaging_allocator_initialize() and has
///                  not been finalized by messaging_allocator_finalize() yet.
/// \param block must be the \ref messaging_block_t that this allocator is
///              managing. Remember that you must always pass the same `block`
///              to all calls associated with this allocator, and you must never
///              pass it to calls associated with another allocator.
///
/// \returns a \ref messaging_atom_t that must later be liberated with
///          messaging_atom_liberate().
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
messaging_atom_t* messaging_atom_allocate(messaging_allocator_t* allocator,
                                          messaging_block_t* block);

/// Liberate a previously allocated \ref messaging_atom_t
/// so other threads can reuse it.
///
/// \param allocator must be a \ref messaging_allocator_t that has been
///                  initialized using messaging_allocator_initialize() and has
///                  not been finalized by messaging_allocator_finalize() yet.
/// \param block must be the \ref messaging_block_t that this allocator is
///              managing. Remember that you must always pass the same `block`
///              to all calls associated with this allocator, and you must never
///              pass it to calls associated with another allocator.
/// \param atom must be a \ref messaging_atom_t that has previously
///             been allocated using messaging_atom_allocate() and has not yet
///             been liberated. It cannot be used again after this call.
UDIPE_NON_NULL_ARGS
void messaging_atom_liberate(messaging_allocator_t* allocator,
                             messaging_block_t* block,
                             messaging_atom_t* atom);


// TODO: Tests