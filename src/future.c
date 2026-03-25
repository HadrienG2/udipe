#ifdef __linux__
    #define _GNU_SOURCE
#endif

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
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>

#ifdef __linux__
    #include <poll.h>
#endif


// === Future status word manipulation ===

void future_status_debug_check(future_status_t status,
                               bool allocated) {
    assert(("65k dependents ought to be enough for anybody",
            !status.downstream_count_overflow));
    assert(("Futures should only reach a zero refcount at deallocation time",
            (status.downstream_count >= 1) == allocated));
    assert(("If false definition of MAX_DOWNSTREAM_COUNT needs updating",
            status.downstream_count <= MAX_DOWNSTREAM_COUNT));

    bool has_dependencies = false;
    bool has_processing = false;
    bool has_dedicated_thread = false;
    bool is_fd_based = false;
    switch (status.type) {
    case TYPE_INVALID:
        assert(!allocated);
        break;
    case TYPE_NETWORK_CONNECT:
    case TYPE_NETWORK_DISCONNECT:
    case TYPE_NETWORK_SEND:
    case TYPE_NETWORK_RECV:
        assert(allocated);
        has_dependencies = true;
        has_processing = true;
        has_dedicated_thread = true;
        is_fd_based = false;
        break;
    case TYPE_CUSTOM:
        assert(allocated);
        has_dependencies = false;
        has_processing = true;
        has_dedicated_thread = true;
        is_fd_based = false;
        break;
    case TYPE_JOIN:
    case TYPE_UNORDERED:
        assert(allocated);
        has_dependencies = true;
        has_processing = false;
        has_dedicated_thread = false;
        is_fd_based = true;
        break;
    case TYPE_TIMER_ONCE:
    case TYPE_TIMER_REPEAT:
        assert(allocated);
        has_dependencies = false;
        has_processing = true;
        has_dedicated_thread = false;
        is_fd_based = true;
        break;
    default:
        assert(("Never valid", false));
    }

    switch (status.state) {
    case STATE_UNINITIALIZED:
        assert(("Only valid for deallocated futures", !allocated));
        break;
    case STATE_WAITING:
        assert(allocated);
        assert(has_dependencies);
        break;
    case STATE_PROCESSING:
        assert(allocated);
        assert(has_processing);
        break;
    case STATE_CANCELING:
        assert(allocated);
        assert(has_dedicated_thread);
    case STATE_RESULT:
        assert(allocated);
        break;
    default:
        assert(("Never valid", false));
    }

    // TODO: Check other fields: outcome, notify_address,
    //       lazy_update_lock/notify_fd (choose depending on is_fd_based).
    // TODO: Inject in any place that manipulates future status words
}


// === Awaiting future results ===

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
DEFINE_PUBLIC
bool udipe_wait(udipe_future_t* future, udipe_duration_ns_t timeout) {
    with_logger(&future->context->logger, {
        return future_wait(future, timeout).state == STATE_RESULT;
    });
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait(udipe_future_t* future,
                            udipe_duration_ns_t timeout) {
    tracef("Checking initial readiness of future %p...", future);
    future_status_t status = future_status_load(future, memory_order_acquire);
    // TODO: Replace with generic future status check
    assert(status.downstream_count >= 1);
    assert(!status.downstream_count_overflow);
    switch (status.state) {
    case STATE_RESULT:
        trace("Future is already in STATE_RESULT at the start of udipe_wait(): wait succeeded.");
        return status;
    case STATE_WAITING:
    case STATE_PROCESSING:
    case STATE_CANCELING:
        trace("Future is not in STATE_RESULT yet, but may require manual status word updates.");
        break;
    case STATE_UNINITIALIZED:
    default:
        assert(future->context);
        errorf("Observed invalid future state %d", status.state);
        exit(EXIT_FAILURE);
    }

    // Determine appropriate waiting strategy
    // TODO: Replace with generic future status check
    assert(status.outcome == OUTCOME_UNKNOWN || status.state == STATE_CANCELING);
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
            // waits with a minimal timeout can end instantly...
            trace("Eager future not in STATE_RESULT + no wait = wait failed.");
            return status;
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
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait_eager(udipe_future_t* future,
                                  future_status_t latest_status,
                                  udipe_duration_ns_t timeout) {
    // Readiness and early exit should be handled upstream
    // TODO: Replace with a generic status consistency check
    assert(latest_status.downstream_count >= 1);
    assert(!latest_status.downstream_count_overflow);
    assert(latest_status.state != STATE_RESULT);
    assert((latest_status.type >= TYPE_NETWORK_START
            && latest_status.type < TYPE_NETWORK_END)
           || latest_status.type == TYPE_CUSTOM);
    assert(timeout != UDIPE_DURATION_MIN);

    trace("Recording wait start time...");
    struct timespec start_time;
    timespec_get(&start_time, TIME_UTC);

    trace("Enabling futex notifications + incrementing downstream_count...");
    bool success;
    do {
        future_status_t desired_status = latest_status;
        desired_status.notify_address = true;
        ensure_lt((size_t)desired_status.downstream_count,
                  (size_t)MAX_DOWNSTREAM_COUNT);
        ensure(!desired_status.downstream_count_overflow);
        ++desired_status.downstream_count;
        success = future_status_compare_exchange_weak(
            future,
            &latest_status,
            desired_status,
            // Subsequent future operations should not be reordered before the
            // downstream_count increment.
            memory_order_acquire,
            memory_order_relaxed
        );
        if (success) {
            trace("Done enabling futex + registering ourself as a waiter.");
            latest_status = desired_status;
            break;
        }
        if (latest_status.state == STATE_RESULT) {
            trace("...and failed, but meanwhile the result became available.");
            atomic_thread_fence(memory_order_acquire);  // Sync with final state
            return latest_status;
        }
        trace("...and failed because another thread updated the status word or "
              "weak compare_exchange failed spuriously. Let's try again.");
    } while(true);

    do {
        trace("Waiting for a futex-signaled status change...");
        success = future_status_wait(future, latest_status, timeout);

        if (success) {
            trace("Checking the new state...");
            latest_status = future_status_load(future, memory_order_relaxed);
            if (latest_status.state == STATE_RESULT) {
                // No acquire barrier here because there's another load below
                trace("Future result is now available: we're done.");
                break;
            }
            trace("Future didn't reach its final state yet!");
        } else {
            trace("Wait failed due to timeout or other cause (signal etc).");
        }

        trace("Measuring elapsed time since last clock check...");
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

        if (elapsed_time >= timeout) {
            trace("Timeout reached without observing STATE_RESULT.");
            break;
        } else {
            trace("Updating timer for the next round of waiting...");
            timeout -= elapsed_time;
            start_time = current_time;
        }
    } while(true);

    trace("Decrementing downstream_count and assessing final status...");
    // Need at least release to ensure no previous operation get reordered after
    // the downstream_count decrement. Putting the acquire barrier here lets us
    // handle last-minute switches to STATE_RESULT correctly.
    return future_downstream_count_dec(future, memory_order_acq_rel);
}

// TODO future_wait_join()
// TODO future_wait_unordered()

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait_timer_once(udipe_future_t* future,
                                       future_status_t latest_status,
                                       udipe_duration_ns_t timeout) {
    // TODO: Replace with a generic status consistency check
    assert(latest_status.downstream_count >= 1);
    assert(!latest_status.downstream_count_overflow);
    assert(latest_status.state == STATE_PROCESSING);
    assert(latest_status.outcome == OUTCOME_UNKNOWN);
    assert(!latest_status.notify_address);
    assert(latest_status.type == TYPE_TIMER_ONCE);
    assert(!latest_status.lazy_update_lock);

    trace("Recording wait start time...");
    struct timespec start_time;
    timespec_get(&start_time, TIME_UTC);

    trace("Registering as a waiter...");
    bool success = future_downstream_count_try_inc(future, &latest_status);
    if (!success) {
        assert(latest_status.state == STATE_RESULT);
        trace("Future reached STATE_RESULT before we started waiting");
        return latest_status;
    }

    #ifdef __linux__

        // TODO: Extract commonalities wrt other fd waits
        // TODO: Add logging

        // Indicate which fd we want to poll (TODO: generalize for other fd waits)
        struct pollfd timer = (struct pollfd){
            .fd = future->output_fd.timer,
            .events = POLLIN,
            .revents = 0
        };

        // Translate our timeout into the timespec format ingested by ppoll()
        struct timespec timeout_spec;
        struct timespec* timeout_spec_ptr = &timeout_spec;
        if (timeout == UDIPE_DURATION_MIN) {
            timeout_spec = (struct timespec){ .tv_sec = 0, .tv_nsec = 0 };
        } else if (timeout == UDIPE_DURATION_MAX || timeout == UDIPE_DURATION_DEFAULT) {
            timeout_spec_ptr = NULL;
        } else {
            timeout_spec = (struct timespec){
                .tv_sec = timeout / UDIPE_SECOND,
                .tv_nsec = timeout % UDIPE_SECOND
            };
        }

        do {
            int result = ppoll(&timer,
                               1,
                               timeout_spec_ptr,
                               NULL);
            switch (result) {
            case 1:
                trace("timerfd is now ready. Will now propagate the good news "
                      "while decrementing our downstream_count");
                do {
                    future_status_t desired_status = latest_status;
                    // TODO: Replace with a generic status consistency check
                    assert(desired_status.downstream_count >= 1);
                    assert(!desired_status.downstream_count_overflow);
                    assert(desired_status.state == STATE_PROCESSING
                           || desired_status.state == STATE_RESULT);
                    assert(!desired_status.notify_address);
                    assert(desired_status.type == TYPE_TIMER_ONCE);
                    assert(!desired_status.lazy_update_lock);
                    --desired_status.downstream_count;
                    desired_status.state = STATE_RESULT;
                    if (desired_status.outcome == OUTCOME_UNKNOWN) {
                        desired_status.outcome = OUTCOME_SUCCESS;
                    }

                    success = future_status_compare_exchange_weak(
                        future,
                        &latest_status,
                        desired_status,
                        // Must use release ordering when decrementing
                        // downstream_count, so that previous operations cannot
                        // be reordered afterwards.
                        memory_order_release,
                        memory_order_relaxed
                    );
                    if (success) latest_status = desired_status;
                } while (!success);
                // No need for an acquire barrier, ppoll() already acted as one
                return latest_status;
            case 0:
                trace("Reached timeout before the future became ready");
                return latest_status;
            case -1:
                switch (errno) {
                    case EINTR:
                        // TODO: Update the timeout and resume the wait
                        exit(EXIT_FAILURE);
                    case EFAULT:  // No such fd
                    case EINVAL:  // nfds too high or timeout is negative
                    case ENOMEM:  // Unable to allocate kernel memory
                    default:
                        exit_with_error("These errors should never happen");
                }
            default:
                exit_with_error("This result should never be returned");
            }
        } while (true);
    #else
        // TODO add windows version based on waitable timers.
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
}

// TODO future_wait_timer_repeat()


// === Other public methods ===

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
        // TODO: Ensure that all fields can only take valid values, possibly
        //       using modulo. Or just generate fields one by one.
        exit(EXIT_FAILURE);
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
        for (int i = 0; i < 2; ++i) {
            const bool has_result = (bool)(i % 2);
            future_status_t initial_status;
            do {
                initial_status = random_status();
            } while (initial_status.downstream_count == MAX_DOWNSTREAM_COUNT
                     && (!has_result && initial_status.state == STATE_RESULT));
            initial_status.downstream_count_overflow = false;
            if (has_result) initial_status.state = STATE_RESULT;
            future_status_store(future, initial_status, memory_order_relaxed);

            future_status_t latest_status = initial_status;
            const bool success =
                future_downstream_count_try_inc(future, &latest_status);
            ensure_eq(success, initial_status.state != STATE_RESULT);
            if (success) {
                ensure_eq((size_t)latest_status.downstream_count,
                          (size_t)(initial_status.downstream_count + 1));
                ensure(!latest_status.downstream_count_overflow);
            } else {
                ensure_status_eq(latest_status, initial_status);
                return;
            }

            future_downstream_count_dec(future, memory_order_release);
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