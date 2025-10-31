#include "connect.h"

#include "bit_array.h"
#include "error.h"
#include "log.h"

#include <assert.h>
#include <errno.h>
#include <linux/futex.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>


/// Initial value of \ref connect_options_allocator_t::availability
///
/// This indicates that all inner \ref shared_connect_options_t are initially
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
    connect_options_allocator_t allocator;
    memset(&allocator, 0, sizeof(connect_options_allocator_t));

    const uint32_t initial_availability = initial_availability_mask();
    debugf("Initializing availability mask to %#x...", initial_availability);
    atomic_init(&allocator.availability, initial_availability);

    for (size_t i = 0; i < NUM_CONNECT_OPTIONS; ++i) {
        tracef("Initializing reference count of options[%zu]...", i);
        atomic_init(&allocator.options[i].reference_count, 0);
    }
    return allocator;
}

UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
shared_connect_options_t*
connect_options_allocate(connect_options_allocator_t* allocator,
                         size_t num_target_workers) {
    debug("Looking for unused options that we can allocate...");
    uint32_t availability = atomic_load_explicit(&allocator->availability,
                                                 memory_order_relaxed);
    size_t option_idx;
    do {
        while (availability == 0) {
            trace("All options are in use, waiting for some to free up...");
            long result = syscall(SYS_futex,
                                  &allocator->availability,
                                  FUTEX_WAIT_PRIVATE,
                                  0,
                                  NULL);
            if (result == -1) {
                switch (errno) {
                case EAGAIN:
                case EINTR:
                    break;
                case EFAULT:
                case EINVAL:
                case ETIMEDOUT:
                default:
                    exit_after_c_error("Unexpected FUTEX_WAIT_PRIVATE failure");
                }
            } else {
                assert(result == 0);
            }
            availability = atomic_load_explicit(&allocator->availability,
                                                memory_order_relaxed);
        }
        tracef("Got nonzero availability mask %#x: ready to allocate!",
               availability);

        const size_t num_available = population_count(availability);
        const size_t available_idx = rand() % num_available;
        tracef("Will now try to allocate available option %zu/%zu...",
               available_idx + 1, num_available);
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
        }
    } while(false);
    // With this fence, we synchronize with the thread that previously
    // deallocated these options, and transitively synchronize with every other
    // thread that previously liberated them.
    atomic_thread_fence(memory_order_acquire);

    shared_connect_options_t* const result = &allocator->options[option_idx];
    debugf("Successfully allocated options[%zu] @ %p, setting refcount to %zu...",
           option_idx, result, num_target_workers);
    assert((
        "Options have not been properly deallocated, might still be in use",
        atomic_load_explicit(&result->reference_count, memory_order_relaxed)
    ));
    atomic_store_explicit(&result->reference_count,
                          num_target_workers,
                          memory_order_relaxed);
    return result;
}

/// Deallocate some \ref shared_connect_options_t
///
/// This should be done by the thread that decremented the reference count of
/// some \ref shared_connect_options_t to zero.
static inline
void connect_options_deallocate(connect_options_allocator_t* allocator,
                                shared_connect_options_t* options) {
    const size_t options_idx = options - allocator->options;
    const uint32_t bit = 1 << options_idx;
    // With release ordering here, we ensure that another thread which observes
    // this availability bit also observes all prior liberations.
    const uint32_t previous_availability =
        atomic_fetch_or_explicit(&allocator->availability,
                                 bit,
                                 memory_order_release);
    assert(("Options have been deallocated multiple times",
            (previous_availability & bit) == 0));

    if (previous_availability == 0) {
        debug("All connect options were in use, let's wake up "
              "one of the worker threads awaiting some (if any)");
        long result = syscall(SYS_futex,
                              &allocator->availability,
                              FUTEX_WAKE_PRIVATE,
                              1);
        assert(result == 0 || result == 1);
        exit_on_negative((int) result,
                         "Unexpected FUTEX_WAKE_PRIVATE failure");
    }
}

UDIPE_NON_NULL_ARGS
void connect_options_liberate(connect_options_allocator_t* allocator,
                              shared_connect_options_t* options) {
    tracef("Marking one worker thread as done with options @ %p...", options);
    const size_t initial_refcount =
        atomic_load_explicit(&options->reference_count,
                             memory_order_relaxed);
    assert(("Options have been liberated too many times",
            initial_refcount > 0));

    // While this fast path may seem infrequent, it is actually always taken in
    // the common scenario where we're setting up sequential connections.
    if (initial_refcount == 1) {
        debugf("Deallocating options @ %p using the single-threaded fast path...",
               options);
        // With this fence, we synchronize with every other thread that
        // decremented the refcount with release ordering previously, so that
        // the final deallocation fetch_or with release ordering encodes all
        // decrements. This way, those who will later allocate with acquire
        // ordering will see all of the liberate calls as being performed.
        atomic_thread_fence(memory_order_acquire);
        atomic_store_explicit(&options->reference_count,
                              0,
                              memory_order_relaxed);
        connect_options_deallocate(allocator, options);
        return;
    }

    tracef("%zu other workers still have access, must use the slow RMW path...",
           initial_refcount - 1);
    // Need release ordering here so that the thread that will finally
    // deallocate fully observes our liberation operation.
    const size_t previous_refcount =
        atomic_fetch_sub_explicit(&options->reference_count,
                                  1,
                                  memory_order_release);

    if (previous_refcount == 1) {
        debugf("Deallocating options @ %p after the RMW slow path...", options);
        // Need this fence for the same reason as above
        atomic_thread_fence(memory_order_acquire);
        connect_options_deallocate(allocator, options);
    }
}
