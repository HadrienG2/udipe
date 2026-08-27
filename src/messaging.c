#include "messaging.h"

#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "address_wait.h"
#include "bits.h"
#include "error.h"
#include "log.h"

#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>


/// Initial value of \ref messaging_allocator_t::availability
///
/// This indicates that all \ref messaging_atom_t within the managed \ref
/// messaging_block_t are initially available and ready to be allocated.
UDIPE_NODISCARD
static inline
uint32_t initial_availability_mask() {
    static_assert(
        MESSAGING_BLOCK_LEN <= (size_t)32,
        "This length is not supported by the futex-based implementation."
    );
    #if (32 == MESSAGING_BLOCK_LEN)
        return UINT32_MAX;
    #else
        return ((uint32_t)1 << MESSAGING_BLOCK_LEN) - 1;
    #endif
}

UDIPE_NODISCARD
messaging_allocator_t
messaging_allocator_initialize() {
    LOGGED_FUNCTION_START_NO_PARAMS
        debug("Zero-initializing the allocator...");
        messaging_allocator_t allocator = { 0 };

        const uint32_t initial_availability = initial_availability_mask();
        debugf("Initializing availability mask to %#x...",
               initial_availability);
        atomic_init(&allocator.availability, initial_availability);
        return allocator;
    LOGGED_FUNCTION_END
}

UDIPE_NON_NULL_ARGS
void
messaging_allocator_finalize(messaging_allocator_t* allocator) {
    LOGGED_FUNCTION_START("%p", allocator)
        debug("Making sure no messaging atoms are still allocated...");
        const uint32_t current_availability =
            atomic_load_explicit(&allocator->availability,
                                 memory_order_relaxed);
        if (current_availability != initial_availability_mask()) {
            exit_with_error(
                "Finalized a messaging allocator while some managed "
                "messaging atoms were still allocated"
            );
        }

        debug("Poisoning availability mask to ensure that alloc-after-finalize "
              "leads to a noticeable deadlock...");
        atomic_store_explicit(&allocator->availability,
                              0,
                              memory_order_release);
    LOGGED_FUNCTION_END
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
messaging_atom_t* messaging_atom_allocate(messaging_allocator_t* allocator,
                                          messaging_block_t* block) {
    LOGGED_FUNCTION_START("%p, %p", allocator, block)
        debug("Looking for unused messaging atoms that we can allocate...");
        uint32_t availability = atomic_load_explicit(&allocator->availability,
                                                     memory_order_relaxed);
        size_t atom_idx;
        do {
            while (availability == 0) {
                debug("All atoms are in use, waiting for some to free up...");
                wait_on_address(&allocator->availability,
                                0,
                                UDIPE_DURATION_MAX);
                availability = atomic_load_explicit(&allocator->availability,
                                                    memory_order_relaxed);
            }
            debugf("Got nonzero availability mask %#x: ready to allocate!",
                   availability);

            const size_t num_available = population_count(availability);
            const size_t available_idx = rand() % num_available;
            debugf("Will now try to allocate available atom #%zu/%zu...",
                   available_idx + 1, num_available);
            atom_idx = 0;
            for (size_t i = 0; i < available_idx; ++i) {
                const size_t extra_offset =
                    count_trailing_zeros(availability) + 1;
                atom_idx += extra_offset;
                availability >>= extra_offset;
            }
            const uint32_t bit = 1 << atom_idx;
            debugf("...which corresponds to atoms[%zu] "
                   "with availability bit %#x.",
                   atom_idx, bit);

            availability = atomic_fetch_and_explicit(&allocator->availability,
                                                     ~bit,
                                                     memory_order_relaxed);
            if ((availability & bit) == 0) {
                debug("Alas another thread got there first, try again...");
                continue;
            } else {
                break;
            }
        } while(true);
        // With this fence, we synchronize with the thread that previously
        // deallocated this atom.
        atomic_thread_fence(memory_order_acquire);

        messaging_atom_t* const result = &block->atoms[atom_idx];
        debugf("Successfully allocated atoms[%zu] @ %p.",
               atom_idx, result);
        return result;
    LOGGED_FUNCTION_END
}

UDIPE_NON_NULL_ARGS
void messaging_atom_liberate(messaging_allocator_t* allocator,
                             messaging_block_t* block,
                             messaging_atom_t* atom) {
    LOGGED_FUNCTION_START("%p, %p", allocator, atom)
        debugf("Marking messaging atom @ %p as available...", atom);
        const size_t atom_idx = atom - block->atoms;
        const uint32_t bit = 1 << atom_idx;
        // With release ordering here, we ensure that our prior accesses to the
        // atom occur before the atom is liberated
        const uint32_t previous_availability =
            atomic_fetch_or_explicit(&allocator->availability,
                                     bit,
                                     memory_order_release);
        assert(("This atom has either not been allocated or "
                "has been deallocated multiple times",
                (previous_availability & bit) == 0));

        if (previous_availability == 0) {
            debug("All messaging atoms were in use before, let's wake up "
                  "one of the worker threads awaiting some (if any)");
            // We're only liberating one atom at a time so _single is correct
            wake_by_address_single(&allocator->availability);
        }
    LOGGED_FUNCTION_END
}
