#include "connect.h"

#include "address_wait.h"
#include "bits.h"
#include "error.h"
#include "log.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


/// Initial value of \ref connect_options_allocator_t::availability
///
/// This indicates that all inner \ref udipe_connect_options_t are initially
/// available and ready to be allocated.
///
/// It can also be used in unit tests to turn a random availability mask into a
/// valid availability mask.
static inline
uint32_t initial_availability_mask() {
    return NUM_CONNECT_OPTIONS == 32 ? UINT32_MAX
                                     : ((uint32_t)1 << NUM_CONNECT_OPTIONS) - 1;
}

connect_options_allocator_t
connect_options_allocator_initialize() {
    debug("Zero-initializing the allocator...");
    connect_options_allocator_t allocator = { 0 };

    const uint32_t initial_availability = initial_availability_mask();
    debugf("Initializing availability mask to %#x...", initial_availability);
    atomic_init(&allocator.availability, initial_availability);
    return allocator;
}

UDIPE_NON_NULL_ARGS
void connect_options_allocator_finalize(connect_options_allocator_t* allocator) {
    debug("Finalizing the allocator...");
    const uint32_t current_availability =
        atomic_load_explicit(&allocator->availability, memory_order_relaxed);
    if (current_availability != initial_availability_mask()) {
        exit_with_error("Finalized allocator while options were allocated");
    }

    atomic_store_explicit(&allocator->availability, 0, memory_order_relaxed);
    debug("Poisoned allocator with a fully-allocated state so that "
          "post-finalization allocation attempts deadlock.");
}

UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_connect_options_t*
connect_options_allocate(connect_options_allocator_t* allocator) {
    debug("Looking for unused options that we can allocate...");
    uint32_t availability = atomic_load_explicit(&allocator->availability,
                                                 memory_order_relaxed);
    size_t option_idx;
    do {
        while (availability == 0) {
            trace("All options are in use, waiting for some to free up...");
            wait_on_address(&allocator->availability, 0, UDIPE_DURATION_MAX);
            availability = atomic_load_explicit(&allocator->availability,
                                                memory_order_relaxed);
        }
        tracef("Got nonzero availability mask %#x: ready to allocate!",
               availability);

        const size_t num_available = population_count(availability);
        const size_t available_idx = rand() % num_available;
        tracef("Will now try to allocate available option %zu/%zu...",
               available_idx + 1, num_available);
        option_idx = 0;
        for (size_t i = 0; i < available_idx; ++i) {
            const size_t extra_offset = count_trailing_zeros(availability) + 1;
            option_idx += extra_offset;
            availability >>= extra_offset;
        }
        const uint32_t bit = 1 << option_idx;
        tracef("...which corresponds to options[%zu] with availability bit %#x",
               option_idx, bit);

        availability = atomic_fetch_and_explicit(&allocator->availability,
                                                 ~bit,
                                                 memory_order_relaxed);
        if ((availability & bit) == 0) {
            trace("Sadly, another thread got there first, must try again...");
            continue;
        } else {
            break;
        }
    } while(true);
    // With this fence, we synchronize with the thread that previously
    // deallocated these options.
    atomic_thread_fence(memory_order_acquire);

    udipe_connect_options_t* const result = &allocator->options[option_idx];
    debugf("Successfully allocated options[%zu] @ %p.",
           option_idx, result);
    return result;
}

UDIPE_NON_NULL_ARGS
void connect_options_liberate(connect_options_allocator_t* allocator,
                              udipe_connect_options_t* options) {
    tracef("Marking worker thread as done with options @ %p...", options);
    const size_t options_idx = options - allocator->options;
    const uint32_t bit = 1 << options_idx;
    // With release ordering here, we ensure that our prior accesses to the
    // options occur before the options are liberated
    const uint32_t previous_availability =
        atomic_fetch_or_explicit(&allocator->availability,
                                 bit,
                                 memory_order_release);
    assert(("Options have been deallocated multiple times",
            (previous_availability & bit) == 0));

    if (previous_availability == 0) {
        debug("All connect options were in use, let's wake up "
              "one of the worker threads awaiting some (if any)");
        wake_by_address_all(&allocator->availability);
    }
}
