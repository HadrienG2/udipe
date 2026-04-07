#ifdef __linux__
    #define _GNU_SOURCE
#endif

#include "future.h"

#include <udipe/context.h>
#include <udipe/duration.h>
#include <udipe/future.h>
#include <udipe/pointer.h>

#include "context.h"
#include "duration.h"
#include "error.h"
#include "event.h"
#include "log.h"
#include "stopwatch.h"
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
    #include <sys/epoll.h>
#endif


// === Tuning parameters ===

/// Maximal number of epoll events that future_wait_join() can fetch.
///
/// Setting this higher may reduce the number of syscalls made by the
/// application when polling large join-sets, at the expense of increasing stack
/// memory usage.
///
/// Other epoll-based futures at time of writing cannot use this optimization:
///
/// - With unordered futures, events should be fetched one by one to ensure that
///   we stop fetching events as soon as one of the parent futures is ready.
/// - With repeating timer futures, we do not expect more than one event, so
///   allocating a buffer of >1 events is useless.
//
// TODO: Tune based on benchmarking on realistic use cases
#define MAX_JOIN_EPOLL_EVENTS (size_t)64


// === Future status word manipulation ===

void future_status_debug_check(future_status_t status,
                               bool is_allocated) {
    assert(is_allocated || !status.available);

    assert(("65k dependents ought to be enough for anybody",
            !status.downstream_count_overflow));
    assert(("Futures can only be used between allocation and start of udipe_finish()",
            (status.downstream_count > 0) == status.available));
    assert(("Should be true unless MAX_DOWNSTREAM_COUNT define needs updating",
            status.downstream_count <= MAX_DOWNSTREAM_COUNT));

    bool has_dependencies;
    bool has_processing;
    bool has_dedicated_thread;
    bool is_fd_based;
    bool requires_locking;
    switch (status.type) {
    case TYPE_INVALID:
        assert(!is_allocated);
        has_dependencies = false;
        has_processing = false;
        has_dedicated_thread = false;
        is_fd_based = false;
        requires_locking = false;
        break;
    case TYPE_NETWORK_CONNECT:
    case TYPE_NETWORK_DISCONNECT:
    case TYPE_NETWORK_SEND:
    case TYPE_NETWORK_RECV:
        assert(is_allocated);
        has_dependencies = true;
        has_processing = true;
        has_dedicated_thread = true;
        is_fd_based = false;
        requires_locking = false;
        break;
    case TYPE_CUSTOM:
        assert(is_allocated);
        has_dependencies = false;
        has_processing = true;
        has_dedicated_thread = true;
        is_fd_based = false;
        requires_locking = false;
        break;
    case TYPE_JOIN:
    case TYPE_UNORDERED:
        assert(is_allocated);
        has_dependencies = true;
        has_processing = false;
        has_dedicated_thread = false;
        is_fd_based = true;
        requires_locking = true;
        break;
    case TYPE_TIMER_ONCE:
        assert(is_allocated);
        has_dependencies = false;
        has_processing = true;
        has_dedicated_thread = false;
        is_fd_based = true;
        requires_locking = false;
        break;
    case TYPE_TIMER_REPEAT:
        assert(is_allocated);
        has_dependencies = false;
        has_processing = true;
        has_dedicated_thread = false;
        is_fd_based = true;
        requires_locking = true;
        break;
    default:
        assert(("Never valid", false));
        has_dependencies = false;
        has_processing = false;
        has_dedicated_thread = false;
        is_fd_based = false;
        requires_locking = false;
    }

    bool has_outcome;
    bool could_be_locked;
    switch (status.state) {
    case STATE_UNINITIALIZED:
        assert(!is_allocated);
        has_outcome = false;
        could_be_locked = false;
        break;
    case STATE_WAITING:
        assert(is_allocated);
        assert(has_dependencies);
        has_outcome = false;
        could_be_locked = requires_locking;
        break;
    case STATE_PROCESSING:
        assert(is_allocated);
        assert(has_processing);
        has_outcome = false;
        could_be_locked = requires_locking;
        break;
    case STATE_CANCELING:
        assert(is_allocated);
        assert(has_dedicated_thread);
        has_outcome = true;
        could_be_locked = false;
        break;
    case STATE_RESULT:
        assert(is_allocated);
        has_outcome = true;
        could_be_locked = false;
        break;
    default:
        assert(("Never valid", false));
        has_outcome = false;
        could_be_locked = false;
    }

    switch (status.outcome) {
    case OUTCOME_UNKNOWN:
        assert(!has_outcome);
        break;
    case OUTCOME_SUCCESS:
        assert(has_outcome);
        assert(status.state == STATE_RESULT);
        break;
    case OUTCOME_FAILURE_DEPENDENCY:
        assert(has_outcome);
        assert(has_dependencies);
        break;
    case OUTCOME_FAILURE_INTERNAL:
        assert(has_outcome);
        assert(has_processing);
        break;
    case OUTCOME_FAILURE_CANCELED:
        assert(has_outcome);
        break;
    default:
        assert(("Never valid", false));
    }

    if (is_allocated) {
        if (is_fd_based) {
            // notify_address can be switched on from the fist state onwards,
            // and remains on after being turned on. Or it may never be switched
            // on. So we can't tell anything about its value.
            if (status.lazy_update_lock) assert(could_be_locked);
        } else {
            // notify_address and notify_fd can be switched on from the WAITING
            // state, and remain on after being turned on. They may also never
            // be switched on. So we can't tell anything about their values.
        }
    } else {
        assert(!status.notify_address);
        if (is_fd_based) {
            assert(!status.lazy_update_lock);
        } else {
            assert(!status.notify_fd);
        }
    }
}


// === Awaiting future results ===

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
DEFINE_PUBLIC
bool udipe_wait(udipe_future_t* future, udipe_duration_ns_t timeout) {
    with_logger(&future->context->logger, {
        if (timeout == UDIPE_DURATION_DEFAULT) timeout = UDIPE_DURATION_MAX;
        const future_status_t final_status =
            future_wait(future, timeout, DOWNSTREAM_COUNT_CYCLE);
        return final_status.state == STATE_RESULT;
    });
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait(udipe_future_t* future,
                            udipe_duration_ns_t timeout,
                            downstream_count_behavior_t count_behavior) {
    assert(timeout != UDIPE_DURATION_DEFAULT);

    tracef("Checking initial readiness of future %p...", future);
    // Synchronize-with the current future state
    future_status_t status = future_status_load(future, memory_order_acquire);
    future_status_debug_check(status, true);
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
        errorf("Observed future state is not valid: %d", status.state);
        exit(EXIT_FAILURE);
    }

    // Determine appropriate waiting strategy
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
            return future_wait_eager(future,
                                     status,
                                     timeout,
                                     count_behavior);
        }
    case TYPE_JOIN:
        return future_wait_join(future, status, timeout, count_behavior);
    case TYPE_UNORDERED:
        return future_wait_unordered(future, status, timeout, count_behavior);
    case TYPE_TIMER_ONCE:
        return future_wait_timer_once(future, status, timeout, count_behavior);
    case TYPE_TIMER_REPEAT:
        return future_wait_timer_repeat(future, status, timeout, count_behavior);
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
                                  udipe_duration_ns_t timeout,
                                  downstream_count_behavior_t count_behavior) {
    // Readiness and early exit should be handled upstream
    future_status_debug_check(latest_status, true);
    assert((latest_status.type >= TYPE_NETWORK_START
            && latest_status.type < TYPE_NETWORK_END)
           || latest_status.type == TYPE_CUSTOM);
    assert(latest_status.state != STATE_RESULT);
    assert(timeout != UDIPE_DURATION_MIN && timeout != UDIPE_DURATION_DEFAULT);

    trace("Initializing timeout stopwatch...");
    stopwatch_t stopwatch = stopwatch_initialize();

    trace("Updating the future's status word...");
    bool success;
    do {
        future_status_t desired_status = latest_status;
        desired_status.notify_address = true;

        // TODO: Extract this repeating code chunk into some kind of
        //       prepare_downstream_count_inc(&latest_status). Add documentation
        //       which clarifies that changes must have acquire ordering. Notify
        //       about the existence of this function in the docs of
        //       future_downstream_count_try_inc().
        ensure(!desired_status.downstream_count_overflow);
        switch (count_behavior) {
        case DOWNSTREAM_COUNT_CYCLE:
            ensure_lt((size_t)desired_status.downstream_count,
                      (size_t)MAX_DOWNSTREAM_COUNT);
            ++desired_status.downstream_count;
            break;
        case DOWNSTREAM_COUNT_KEEP:
            break;
        }

        future_status_debug_check(desired_status, true);

        success = future_status_compare_exchange_weak(
            future,
            &latest_status,
            desired_status,
            // Subsequent operations must not be reordered before the above
            // status word changes, as e.g. reordering the wait before the
            // enablement of notifications would lead to a deadlock.
            //
            // We are not notifying other threads about any shared state change
            // other than our status word update, so release ordering or
            // stronger is not needed here.
            memory_order_acquire,
            // Synchronize-with concurrent future state changes.
            memory_order_acquire
        );

        if (success) {
            trace("Done updating the future status.");
            latest_status = desired_status;
            break;
        }
        future_status_debug_check(latest_status, true);
        if (latest_status.state == STATE_RESULT) {
            trace("Failed to update status, but result became available meanwhile.");
            return latest_status;
        }
        trace("Failed because another thread updated the status word or "
              "weak compare_exchange failed spuriously. Let's try again.");
    } while(true);

    trace("Waiting for the dedicated thread to update the future status...");
    switch (future_wait_by_adress(future,
                                  &latest_status,
                                  &timeout,
                                  &stopwatch)) {
    case ADDRESS_WAIT_SUCCESS:
    case ADDRESS_WAIT_TIMEOUT:
        trace("Result is now available or wait timed out.");
        switch (count_behavior) {
        case DOWNSTREAM_COUNT_CYCLE:
            // Need at least release ordering to ensure no previous
            // operation get reordered after the downstream_count
            // decrement. Adding in the acquire ordering ensures that we
            // synchronize-with concurrent state changes.
            return future_downstream_count_dec(future, memory_order_acq_rel);
        case DOWNSTREAM_COUNT_KEEP:
            return latest_status;
        default:
            exit_with_error("No other count_behavior is supported yet");
        }
    case ADDRESS_WAIT_UNLOCKED:
    default:
        exit_with_error("Shouldn't happen for this future type");
    }
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait_join(udipe_future_t* future,
                                 future_status_t latest_status,
                                 udipe_duration_ns_t timeout,
                                 downstream_count_behavior_t count_behavior) {
    // TODO: Much of this code should be identical across all epoll-based
    //       future types, with only handling of epoll notifications differing,
    //       so a generic function with a callback should be eventually devised.
    // TODO: This function is also too complex and should be broken up. Perhaps
    //       the aforementioned refactor could be a first step in this direction.

    future_status_debug_check(latest_status, true);
    assert(latest_status.type == TYPE_JOIN);
    assert(latest_status.state != STATE_RESULT);
    assert(timeout != UDIPE_DURATION_DEFAULT);

    trace("Initializing timeout stopwatch...");
    stopwatch_t stopwatch = stopwatch_initialize();

    do {
        trace("Preparing future status update...");
        bool previously_locked;
        do {
            future_status_t desired_status = latest_status;
            previously_locked = latest_status.lazy_update_lock;
            if (previously_locked) {
                trace("Another thread is already awaiting this future, "
                      "will await it via the futex route...");
                desired_status.notify_address = true;
            } else {
                trace("We're the first to await it, so try to take the lock.");
                desired_status.lazy_update_lock = true;
            }
            // TODO: Extract this repeating code chunk into some kind of
            //       prepare_downstream_count_inc(&latest_status). Add
            //       documentation which clarifies that changes must have
            //       acquire ordering. Notify about the existence of this
            //       function in the docs of future_downstream_count_try_inc().
            ensure(!desired_status.downstream_count_overflow);
            switch (count_behavior) {
            case DOWNSTREAM_COUNT_CYCLE:
                ensure_lt((size_t)desired_status.downstream_count,
                          (size_t)MAX_DOWNSTREAM_COUNT);
                ++desired_status.downstream_count;
                break;
            case DOWNSTREAM_COUNT_KEEP:
                break;
            }

            future_status_debug_check(desired_status, true);

            const bool success = future_status_compare_exchange_weak(
                future,
                &latest_status,
                desired_status,
                // Subsequent operations must not be reordered before the above
                // status word changes, as e.g. reordering the wait before the
                // enablement of notifications would lead to a deadlock.
                //
                // We are not notifying others about any state change other than
                // our status word update, so release ordering or stronger is
                // not needed here.
                memory_order_acquire,
                // Synchronize-with concurrent future state changes.
                memory_order_acquire
            );

            if (success) {
                trace("Successfully updated the future's status.");
                latest_status = desired_status;
                break;
            }
            future_status_debug_check(latest_status, true);
            if (latest_status.state == STATE_RESULT) {
                trace("Failed to update the future's status, "
                      "but the result became available meanwhile.");
                return latest_status;
            }
            trace("...and failed because another thread updated the status or "
                  "weak compare_exchange failed spuriously. Let's try again.");
        } while(true);

        if (previously_locked) {
            trace("Waiting for owner of lazy_update_lock to yield a result or give up...");
            switch (future_wait_by_adress(future,
                                          &latest_status,
                                          &timeout,
                                          &stopwatch)) {
            case ADDRESS_WAIT_SUCCESS:
            case ADDRESS_WAIT_TIMEOUT:
                trace("Result is now available or wait timed out.");
                switch (count_behavior) {
                case DOWNSTREAM_COUNT_CYCLE:
                    // Need at least release ordering to ensure no previous
                    // operation get reordered after the downstream_count
                    // decrement. Adding in the acquire ordering ensures that we
                    // synchronize-with concurrent state changes.
                    return future_downstream_count_dec(future, memory_order_acq_rel);
                case DOWNSTREAM_COUNT_KEEP:
                    return latest_status;
                default:
                    exit_with_error("No other count_behavior is supported yet");
                }
            case ADDRESS_WAIT_UNLOCKED:
                trace("Other thread gave up, let's try to acquire lazy_update_lock again!");
                continue;
            }
        } else {
            #ifdef __linux__
                assert(latest_status.outcome == OUTCOME_UNKNOWN);
                future_outcome_t outcome = OUTCOME_UNKNOWN;
                do {
                    struct timespec delay;
                    struct timespec* pdelay = make_unix_timeout(&delay, timeout);

                    trace("Beginning wait for epoll events...");
                    struct epoll_event events[MAX_JOIN_EPOLL_EVENTS];
                    int result = epoll_pwait2(future->output_fd.epoll_with_event,
                                              events,
                                              MAX_JOIN_EPOLL_EVENTS,
                                              pdelay,
                                              NULL);

                    bool epoll_timed_out = false;
                    bool interrupted_by_signal = false;
                    switch (result) {
                    case 0:
                        trace("Reached timeout during epoll_pwait2()...");
                        epoll_timed_out = true;
                        break;
                    case -1:
                        switch (errno) {
                        case EINTR:
                            trace("Interrupted by signal before timeout/event...");
                            interrupted_by_signal = true;
                            break;
                        case EBADF:  // epfd is not a valid file descriptor.
                        case EFAULT:  // events cannot be written to.
                        case EINVAL:  // epfd is not an epollfd or n <= zero.
                            exit_with_error("These errors should never happen");
                        }
                        break;
                    default:
                        // Expecting at least one ready event on this branch
                        ensure_ge(result, 1);
                        const collective_upstream_t all_upstreams =
                            future->specific.join.upstream;
                        for (size_t i = 0; i < (size_t)result; ++i) {
                            const struct epoll_event* event = &events[i];
                            // We only subscribed to EPOLLIN, and we don't
                            // expect any of the EPOLLERR/EPOLLHUP
                            // auto-subscribed events for the kind of fds that
                            // we are monitoring.
                            ensure_eq(event->events, EPOLLIN);

                            // eventfd notifications can only occur if this
                            // future was concurrently canceled, so we can abort
                            // the loop over events in this case.
                            const size_t upstream_index = event->data.u64;
                            if (upstream_index == UINT64_MAX) {
                                outcome = OUTCOME_FAILURE_CANCELED;
                                break;
                            }
                            ensure_lt(upstream_index, all_upstreams.len);

                            // Find the upstream future that became ready and
                            // check/update its status word
                            udipe_future_t* const upstream =
                                all_upstreams.array[upstream_index];
                            future_status_t upstream_status =
                                future_wait(upstream,
                                            UDIPE_DURATION_MIN,
                                            DOWNSTREAM_COUNT_KEEP);
                            future_status_debug_check(upstream_status, true);

                            // If this upstream future is not ready yet, we'll
                            // need to keep polling it until it is ready or
                            // another future errors out.
                            if (upstream_status.state != STATE_RESULT) continue;

                            // If this upstream future's outcome is now known,
                            // remove it from our epoll set...
                            ensure_ge(future->specific.join.remaining,
                                      (size_t)1);
                            --(future->specific.join.remaining);
                            const int upstream_fd = upstream->output_fd.any;
                            // FIXME: Forgot to handle epoll_ctl errors here!
                            //        But this code is already way too nested,
                            //        should extract it into another function
                            //        before adding more nesting to it.
                            epoll_ctl(future->output_fd.epoll_with_event,
                                      EPOLL_CTL_DEL,
                                      upstream_fd,
                                      NULL);
                            // TODO: Call future_downstream_count_dec() on
                            //       upstream futures at the time where this
                            //       future is destroyed by udipe_finish(). We
                            //       cannot call it beforehand because upon
                            //       cancelation, the upstream future could be
                            //       accessed again, as the cancelation routine
                            //       does not know which upstreams were removed
                            //       from our epoll set. All it can do is call
                            //       epoll_ctl on everything and ignore errors.

                            // Integrate the upstream future's outcome into our
                            // own outcome.
                            switch (upstream_status.outcome) {
                            case OUTCOME_SUCCESS:
                                break;
                            case OUTCOME_FAILURE_CANCELED:
                            case OUTCOME_FAILURE_INTERNAL:
                            case OUTCOME_FAILURE_DEPENDENCY:
                                assert(outcome == OUTCOME_UNKNOWN);
                                outcome = OUTCOME_FAILURE_DEPENDENCY;
                                break;
                            case OUTCOME_UNKNOWN:
                            default:
                                exit_with_error(
                                    "These outcomes should not be observed "
                                    "in STATE_RESULT"
                                );
                            }
                        }
                        const bool all_upstreams_ready =
                            future->specific.join.remaining == 0;
                        if (all_upstreams_ready && outcome == OUTCOME_UNKNOWN) {
                            outcome = OUTCOME_SUCCESS;
                        }
                    }
                    const bool outcome_known = (outcome != OUTCOME_UNKNOWN);

                    bool reached_timeout = epoll_timed_out;
                    if (!outcome_known || interrupted_by_signal) {
                        udipe_duration_ns_t elapsed_time = stopwatch_measure(&stopwatch);
                        if (elapsed_time >= timeout) {
                            trace("Reached timeout after epoll_pwait2()...");
                            reached_timeout = true;
                        } else {
                            trace("Updating timeout after epoll_pwait2()...");
                            timeout -= elapsed_time;
                        }
                    }

                    if (!(outcome_known || reached_timeout)) {
                        trace("No timeout or outcome yet: resuming wait...");
                        continue;
                    }
                    // From this point on, !outcome_known => reached_timeout

                    bool canceled = false;
                    do {
                        trace("Preparing future status update...");
                        future_status_t desired_status = latest_status;

                        // TODO: Extract this repeating code chunk into some
                        //       kind of
                        //       prepare_downstream_count_dec(&latest_status).
                        //       Add documentation which clarifies that changes
                        //       must have require ordering. Notify about the
                        //       existence of this function in the docs of
                        //       future_downstream_count_dec().
                        ensure(!desired_status.downstream_count_overflow);
                        switch (count_behavior) {
                        case DOWNSTREAM_COUNT_CYCLE:
                            ensure_ge((size_t)desired_status.downstream_count,
                                      (size_t)1);
                            --desired_status.downstream_count;
                            break;
                        case DOWNSTREAM_COUNT_KEEP:
                            break;
                        }

                        if (desired_status.outcome != OUTCOME_UNKNOWN) {
                            ensure_eq((int)desired_status.state,
                                      (int)STATE_RESULT);
                            ensure_eq((int)desired_status.outcome,
                                      (int)OUTCOME_FAILURE_CANCELED);
                            canceled = true;
                        } else if (outcome_known) {
                            desired_status.state = STATE_RESULT;
                            desired_status.outcome = outcome;
                        } else {
                            assert(reached_timeout);
                        }

                        ensure_eq((int)desired_status.type, (int)TYPE_JOIN);

                        ensure(("We acquired the lock, it should still be locked",
                                latest_status.lazy_update_lock));
                        desired_status.lazy_update_lock = false;

                        future_status_debug_check(desired_status, true);
                        const bool success = future_status_compare_exchange_weak(
                            future,
                            &latest_status,
                            desired_status,
                            // Must use release ordering when signaling a new
                            // future state through a status word change.
                            memory_order_release,
                            // Synchronize-with concurrent future state changes.
                            memory_order_acquire
                        );

                        if (success) {
                            trace("Successfully updated the future's status.");
                            latest_status = desired_status;
                            break;
                        }
                        future_status_debug_check(latest_status, true);

                        trace("...and failed because another thread updated "
                              "the status word or weak compare_exchange failed "
                              "spuriously. Let's try again...");
                    } while(true);

                    if (latest_status.downstream_count == 0 || canceled) {
                        // - No notification needed if no thread is waiting for
                        //   this future's status to change. Any thread that
                        //   observes this future's final status afterwards
                        //   should not need to query its eventfd to know that
                        //   it's already ready.
                        // - No notification needed if we were canceled, the
                        //   thread that switched to a canceled status will take
                        //   care of notifying others for us.
                    } else if (outcome_known) {
                        trace("Letting other threads know that the outcome of "
                              "this join future is now known.");
                        if (latest_status.notify_address) {
                            trace("...including other threads which were "
                                  "waiting for lazy_update_lock.");
                            wake_by_address_all(&future->status_word);
                        }
                        event_signal(future->specific.join.outcome_event);
                    } else {
                        assert(reached_timeout);
                        trace("Reached timeout without completing the wait.");
                        if (latest_status.notify_address) {
                            trace("Will now wake up another waiter (if any) to "
                                  "take over the lazy_update_lock.");
                            // Use of wake_by_address_single() avoids the
                            // thundering herd that would occur if multiple
                            // waiters woke only to immediately race to acquire
                            // lazy_update_lock and fall back asleep.
                            wake_by_address_single(&future->status_word);
                        } else {
                            trace("No other thread could be waiting for "
                                  "lazy_update_lock at the time where it freed "
                                  "up: will leave the futex alone.");
                        }
                    }
                    return latest_status;
                } while(true);
            #else
                // TODO add windows version based on event objects signaled via the thread pool
                #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
            #endif
        }
    } while(true);
}

// TODO future_wait_unordered()

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait_timer_once(
    udipe_future_t* future,
    future_status_t latest_status,
    udipe_duration_ns_t timeout,
    downstream_count_behavior_t count_behavior
) {
    future_status_debug_check(latest_status, true);
    assert(latest_status.type == TYPE_TIMER_ONCE);
    assert(latest_status.state != STATE_RESULT);
    assert(timeout != UDIPE_DURATION_DEFAULT);

    trace("Initializing timeout stopwatch...");
    stopwatch_t stopwatch = stopwatch_initialize();

    switch(count_behavior) {
    case DOWNSTREAM_COUNT_CYCLE:
        trace("Registering as a waiter...");
        const bool success =
            future_downstream_count_try_inc(future, &latest_status);
        future_status_debug_check(latest_status, true);
        if (!success) {
            assert(latest_status.state == STATE_RESULT);
            trace("Future reached STATE_RESULT before we started waiting");
            return latest_status;
        }
        break;
    case DOWNSTREAM_COUNT_KEEP:
        // Not checking the future's initial state again if we don't need to
        // increment the downstream count: future_wait() already checked it
        // right before calling this function, it is unlikely to have changed.
        break;
    }

    #ifdef __linux__

        // TODO: Extract commonalities wrt other fd waits

        trace("Encoding timerfd into a pollfd...");
        struct pollfd timer = (struct pollfd){
            .fd = future->output_fd.timer,
            .events = POLLIN,
            .revents = 0
        };

        do {
            struct timespec delay;
            struct timespec* pdelay = make_unix_timeout(&delay, timeout);

            trace("Waiting for timerfd readiness...");
            int result = ppoll(&timer,
                               1,
                               pdelay,
                               NULL);
            switch (result) {
            case 1:
                trace("timerfd is now ready. Will now propagate the good news "
                      "while decrementing our downstream_count");
                do {
                    future_status_t desired_status = latest_status;

                    // TODO: Extract this repeating code chunk into some kind of
                    //       prepare_downstream_count_dec(&latest_status). Add
                    //       documentation which clarifies that changes must
                    //       have require ordering. Notify about the existence
                    //       of this function in the docs of
                    //       future_downstream_count_dec().
                    ensure(!desired_status.downstream_count_overflow);
                    switch(count_behavior) {
                    case DOWNSTREAM_COUNT_CYCLE:
                        ensure_ge((size_t)desired_status.downstream_count,
                                  (size_t)1);
                        --desired_status.downstream_count;
                        break;
                    case DOWNSTREAM_COUNT_KEEP:
                        break;
                    }

                    desired_status.state = STATE_RESULT;
                    if (desired_status.outcome == OUTCOME_UNKNOWN) {
                        // Only if OUTCOME_UNKNOWN to avoid overwriting a former
                        // cancelation signal.
                        desired_status.outcome = OUTCOME_SUCCESS;
                    }
                    future_status_debug_check(desired_status, true);

                    const bool success = future_status_compare_exchange_weak(
                        future,
                        &latest_status,
                        desired_status,
                        // Must use release ordering when signaling a new future
                        // state through a status word change.
                        memory_order_release,
                        // Synchronize-with concurrent future state changes.
                        memory_order_acquire
                    );
                    if (success) {
                        latest_status = desired_status;
                        break;
                    } else {
                        future_status_debug_check(latest_status, true);
                        continue;
                    }
                } while (true);
                return latest_status;
            case 0:
                trace("Reached timeout before the future became ready");
                return latest_status;
            case -1:
                switch (errno) {
                    case EINTR:
                        trace("Interrupted by signal, updating timeout...");
                        udipe_duration_ns_t elapsed_time =
                            stopwatch_measure(&stopwatch);
                        if (elapsed_time >= timeout) {
                            trace("Reached timeout before the future became ready");
                            return latest_status;
                        } else {
                            timeout -= elapsed_time;
                            continue;
                        }
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

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
address_wait_outcome_t future_wait_by_adress(
    udipe_future_t* future,
    future_status_t* latest_status,
    udipe_duration_ns_t* timeout,
    stopwatch_t* stopwatch
) {
    assert(latest_status->state != STATE_RESULT);
    assert(timeout != UDIPE_DURATION_DEFAULT);

    trace("Checking if future type is lazy / awaited via epoll_wait()...");
    bool lazy;
    switch (latest_status->type) {
    case TYPE_NETWORK_CONNECT:
    case TYPE_NETWORK_DISCONNECT:
    case TYPE_NETWORK_SEND:
    case TYPE_NETWORK_RECV:
    case TYPE_CUSTOM:
        lazy = false;
        break;
    case TYPE_JOIN:
    case TYPE_UNORDERED:
    case TYPE_TIMER_REPEAT:
        lazy = true;
        // This function should only be called if waiting for the epollfd
        // directly is not possible because another thread is polling it.
        assert(latest_status->lazy_update_lock);
        break;
    case TYPE_INVALID:
    case TYPE_TIMER_ONCE:
    default:
        exit_with_error("future_wait_by_adress() should not have been called");
    }
    // This function should only be called after directing the threads updating
    // the status word that they needs to send a futex notification.
    assert(latest_status->notify_address);


    do {
        trace("Waiting for a futex-signaled status change...");
        const bool success = future_status_wait(future, *latest_status, *timeout);

        if (success) {
            trace("Checking the new state...");
            // Synchronize-with freshly signaled future state changes
            *latest_status = future_status_load(future, memory_order_acquire);
            future_status_debug_check(*latest_status, true);
            if (latest_status->state == STATE_RESULT) {
                trace("Future result is now available: we're done.");
                break;
            } else if (lazy && !latest_status->lazy_update_lock) {
                trace("Lazy future has been unlocked: must switch to epoll_wait() or wake up next waiter.");
                break;
            }
            trace("Future didn't reach its final state yet!");
        } else {
            trace("Wait failed due to timeout or other cause (signal etc).");
        }

        trace("Measuring elapsed time since last clock check...");
        udipe_duration_ns_t elapsed_time = stopwatch_measure(stopwatch);
        if (elapsed_time >= *timeout) {
            trace("Timeout reached without observing STATE_RESULT.");
            *timeout = UDIPE_DURATION_MIN;
            break;
        } else {
            trace("Updating timer for the next round of waiting...");
            *timeout -= elapsed_time;
        }
    } while(true);

    trace("Assessing final status...");
    if (latest_status->state == STATE_RESULT) {
        return ADDRESS_WAIT_SUCCESS;
    } else if (lazy && !latest_status->lazy_update_lock) {
        return ADDRESS_WAIT_UNLOCKED;
    } else {
        return ADDRESS_WAIT_TIMEOUT;
    }
}


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
        future_status_t result =
            (future_status_word_t){ .as_word = rand() }.as_bitfield;
        // TODO: Ensure that all fields can only take valid values, possibly
        //       using modulo. Or just generate fields one by one.
        exit(EXIT_FAILURE);
        return result;
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