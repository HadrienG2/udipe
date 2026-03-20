#include "future.h"

#include <udipe/context.h>
#include <udipe/future.h>
#include <udipe/pointer.h>

#include "context.h"
#include "error.h"
#include "log.h"
#include "unit_tests.h"
#include "visibility.h"

#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>


UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
bool future_wait_eager(udipe_future_t* future,
                       future_status_t latest_status,
                       udipe_duration_ns_t timeout) {
    // Readiness and early exit were handled upstream
    assert(latest_status.state != STATE_RESULT
           && timeout != UDIPE_DURATION_MIN);

    // Record the time at which the wait started
    struct timespec start_time;
    timespec_get(&start_time, TIME_UTC);

    // Request futex notifications from upstream if no one else did, performing
    // the downstream count increment along the way.
    bool downstream_count_incremented = false;
    while (!latest_status.notify_address) {
        future_status_t desired_status = latest_status;
        desired_status.notify_address = true;
        ensure_lt((size_t)desired_status.downstream_count,
                  (size_t)MAX_DOWNSTREAM_COUNT);
        ensure(!desired_status.downstream_count_overflow);
        ++desired_status.downstream_count;
        const bool success = future_status_compare_exchange_weak(
            future,
            &latest_status,
            desired_status,
            memory_order_relaxed,
            memory_order_relaxed
        );
        // Done if we successfully enabled notifications via compare_exchange
        // (which implicitly means that nothing else happened)
        if (success) {
            downstream_count_incremented = true;
            break;
        }
        // If control reaches this point, latest_status may have changed.
        // Return early if the future concurrently switched to the final state.
        if (latest_status.state == STATE_RESULT) {
            atomic_thread_fence(memory_order_acquire);
            return true;
        }
    }

    // Increment the downstream count if that was not done as part of the
    // previous operation...
    if (!downstream_count_incremented) {
        latest_status = future_downstream_count_inc(future,
                                                    memory_order_relaxed);
        if (latest_status.downstream_count == MAX_DOWNSTREAM_COUNT
            || latest_status.downstream_count_overflow)
        {
            future_downstream_count_dec(future,
                                        memory_order_relaxed);
            errorf("Sorry, current future implementation does not support \
                    attaching more than %zd waiters to a future",
                   (size_t)MAX_DOWNSTREAM_COUNT);
            exit(EXIT_FAILURE);
        } else {
            ++latest_status.downstream_count;
        }
    }

    // ...then wait for the future to switch to the ready state
    do {
        // Perform a round of waiting
        bool not_spurious = future_status_wait(future,
                                               latest_status,
                                               timeout);

        // In case of non-spurious wakeup, update latest_status and check if
        // we entered the state where a result is indeed available
        if (not_spurious) {
            latest_status = future_status_load(future, memory_order_relaxed);
            if (latest_status.state == STATE_RESULT) {
                atomic_thread_fence(memory_order_acquire);
                return true;
            }
        }

        // If not, check how much time elapsed since we started waiting
        struct timespec current_time;
        timespec_get(&current_time, TIME_UTC);
        assert(current_time.tv_sec >= start_time.tv_sec);
        udipe_duration_ns_t elapsed_time =
            (current_time.tv_sec - start_time.tv_sec) * UDIPE_SECOND;
        if (current_time.tv_nsec >= start_time.tv_nsec) {
            elapsed_time += current_time.tv_nsec - start_time.tv_nsec;
        } else {
            elapsed_time += UDIPE_SECOND;
            elapsed_time -= start_time.tv_nsec - current_time.tv_nsec;
        }

        // ...then either exit early if the time is up, or update the
        // timeout and clock baseline otherwise
        if (elapsed_time >= timeout) {
            return false;
        } else {
            timeout -= elapsed_time;
            start_time = current_time;
        }
    } while(true);
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
DEFINE_PUBLIC
bool udipe_wait(udipe_future_t* future, udipe_duration_ns_t timeout) {
    // Check initial status of the future
    future_status_t status = future_status_load(future, memory_order_acquire);
    switch (status.state) {
    case STATE_RESULT:
        // Result is known to be available from the start
        return true;
    case STATE_WAITING:
    case STATE_PROCESSING:
    case STATE_CANCELING:
        // Either the result is not available OR it is available but some action
        // must be taken to update the status word
        break;
    case STATE_UNINITIALIZED:
    default:
        assert(future->context);
        with_logger(&future->context->logger, {
            errorf("Observed invalid future state %d", status.state);
        });
        exit(EXIT_FAILURE);
    }

    // Determine appropriate waiting strategy
    with_logger(&future->context->logger, {
        switch (status.type) {
        case TYPE_NETWORK_CONNECT:
        case TYPE_NETWORK_DISCONNECT:
        case TYPE_NETWORK_SEND:
        case TYPE_NETWORK_RECV:
        case TYPE_CUSTOM:
            // Eager futures that are actively driven by a dedicated thread
            // enjoy the most efficient futex-based synchronization mechanism.
            if (timeout == UDIPE_DURATION_MIN) {
                // The status word is guaranteed to be kept up to date, so
                // "check-only waits" can end instantly...
                return false;
            } else {
                // ...and the waiting logic is independent of the eager type
                // that one is dealing with (only the result fetching differs)
                return future_wait_eager(future, status, timeout);
            }
        case TYPE_JOIN:
            return future_wait_join(future, status, timeout);
        case TYPE_UNORDERED:
            return future_wait_unordered(future, status, timeout);
        case TYPE_TIMER_ONCE:
            return future_wait_timer_once(future, status, timeout);
        case TYPE_TIMER_REPEAT:
            return future_wait_timer_repeat(future, status, timeout);
        case TYPE_INVALID:
        default:
            errorf("Observed invalid future type %d", status.type);
            exit(EXIT_FAILURE);
        }
    });
}


/*DEFINE_PUBLIC
UDIPE_NON_NULL_ARGS
void udipe_join(udipe_context_t* context,
                udipe_future_t* const futures[],
                size_t num_futures) {
    // TODO: Benchmark on various platforms, use e.g. a udipe_wait() loop if it
    //       is faster on selected platforms. On Windows, consider using
    //       WaitForMultipleObjects loop... you get the idea.
    udipe_future_t* future = udipe_start_join(context, futures, num_futures);
    assert(future);
    udipe_result_t result = udipe_finish(future);
    assert(result.type == UDIPE_JOIN);
}*/


#ifdef UDIPE_BUILD_TESTS

    future_status_t random_status() {
        return (future_status_word_t){ .as_word = rand() }.as_bitfield;
    }

    uint32_t status_to_u32(future_status_t status) {
        return (future_status_word_t){ .as_bitfield = status }.as_word;
    }

    #define ensure_status_eq(x, y)  \
        ensure_eq(status_to_u32(x), status_to_u32(y))

    void check_future_status_init(udipe_future_t* future,
                                  future_status_t status) {
        future_status_initialize(future, status);
        ensure_status_eq(future_status_load(future, memory_order_relaxed),
                         status);
    }

    void check_future_status_write(udipe_future_t* future,
                                   future_status_t status) {
        future_status_store(future, status, memory_order_relaxed);
        ensure_status_eq(future_status_load(future, memory_order_relaxed),
                         status);
    }

    void check_future_status_cas_fail(udipe_future_t* future) {
        const future_status_t initial_status = future_status_load(
            future,
            memory_order_relaxed
        );
        future_status_t expected, desired;
        do {
            expected = random_status();
            desired = random_status();
        } while (status_to_u32(expected) == status_to_u32(initial_status)
                 || status_to_u32(desired) == status_to_u32(initial_status)
                 || status_to_u32(expected) == status_to_u32(desired));

        const future_status_t initial_expected = expected;
        ensure(
            !future_status_compare_exchange_strong(future,
                                                   &expected,
                                                   desired,
                                                   memory_order_relaxed,
                                                   memory_order_relaxed)
        );
        ensure_status_eq(expected, initial_status);
        ensure_status_eq(future_status_load(future, memory_order_relaxed),
                         initial_status);
        expected = initial_expected;

        ensure(
            !future_status_compare_exchange_weak(future,
                                                 &expected,
                                                 desired,
                                                 memory_order_relaxed,
                                                 memory_order_relaxed)
        );
        ensure_status_eq(expected, initial_status);
        ensure_status_eq(future_status_load(future, memory_order_relaxed),
                         initial_status);
    }

    void check_future_status_cas_success(udipe_future_t* future) {
        const future_status_t initial_status = future_status_load(
            future,
            memory_order_relaxed
        );
        future_status_t desired1, desired2;
        do {
            desired1 = random_status();
            desired2 = random_status();
        } while (status_to_u32(desired1) == status_to_u32(initial_status)
                 || status_to_u32(desired2) == status_to_u32(initial_status)
                 || status_to_u32(desired1) == status_to_u32(desired2));

        future_status_t expected = initial_status;
        ensure(
            future_status_compare_exchange_strong(future,
                                                  &expected,
                                                  desired1,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)
        );
        ensure_status_eq(expected, initial_status);
        ensure_status_eq(future_status_load(future, memory_order_relaxed),
                         desired1);
        expected = desired1;

        while (!future_status_compare_exchange_weak(future,
                                                    &expected,
                                                    desired2,
                                                    memory_order_relaxed,
                                                    memory_order_relaxed)) {
            ensure_status_eq(expected, desired1);
            ensure_status_eq(future_status_load(future, memory_order_relaxed),
                             desired1);
        }
        ensure_status_eq(expected, desired1);
        ensure_status_eq(future_status_load(future, memory_order_relaxed),
                         desired2);
    }

    void check_future_downstream_count_inc_dec(udipe_future_t* future) {
        for (size_t i = 0; i < 2; ++i) {
            future_status_t initial_status = random_status();
            initial_status.downstream_count_overflow = false;
            if (i == 1) {
                initial_status.downstream_count = MAX_DOWNSTREAM_COUNT;
            }
            future_status_store(future, initial_status, memory_order_relaxed);

            ensure_status_eq(
                future_downstream_count_inc(future, memory_order_relaxed),
                initial_status
            );
            const future_status_t new_status = future_status_load(
                future,
                memory_order_relaxed
            );
            if (initial_status.downstream_count < MAX_DOWNSTREAM_COUNT) {
                ensure_eq((size_t)new_status.downstream_count,
                          (size_t)(initial_status.downstream_count + 1));
                ensure(!new_status.downstream_count_overflow);
            } else {
                ensure_eq((size_t)new_status.downstream_count, (size_t)0);
                ensure(new_status.downstream_count_overflow);
            }

            ensure_status_eq(
                future_downstream_count_dec(future, memory_order_relaxed),
                new_status
            );
            ensure_status_eq(
                future_status_load(future, memory_order_relaxed),
                initial_status
            );
        }
    }

    void future_unit_tests() {
        info("Running future unit tests...");
        configure_rand();

        debug("Testing future initialization...");
        udipe_future_t future;
        check_future_status_init(&future, random_status());

        debug("Testing status word writes...");
        check_future_status_write(&future, random_status());

        debug("Testing status word CAS failure...");
        check_future_status_cas_fail(&future);

        debug("Testing status word CAS success...");
        check_future_status_cas_success(&future);

        debug("Testing downstream count increment/decrement...");
        check_future_downstream_count_inc_dec(&future);

        // TODO: Add more future tests as they come up
    }

#endif  // UDIPE_BUILD_TESTS