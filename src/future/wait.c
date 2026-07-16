#ifdef __linux__
    #define _GNU_SOURCE
#endif

#include "wait.h"

#include <udipe/duration.h>
#include <udipe/future.h>
#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "outcome.h"
#include "state.h"
#include "status.h"
#include "status_ops.h"
#include "type.h"
#ifdef __linux__
    #include "latched_inpoll.h"
#endif

#include "../duration.h"
#include "../error.h"
#include "../event.h"
#include "../future.h"
#include "../log.h"
#include "../stopwatch.h"
#ifdef __linux__
    #include "../inpoll.h"
#endif

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>
#ifdef __linux__
    #include <poll.h>
#endif



// === Tuning parameters ===

/// Maximal number of \ref inpoll_t identifiers that future_wait_join() can
/// fetch.
///
/// Setting this higher may reduce the number of syscalls made by the
/// application when polling large join-sets, at the expense of increasing stack
/// memory usage.
///
/// Other \ref inpoll_t-based futures at time of writing cannot use this
/// optimization:
///
/// - With unordered futures, events should be fetched one by one to ensure that
///   we stop fetching events as soon as one of the parent futures is ready.
/// - With repeating timer futures, we do not expect more than one event, so
///   allocating a buffer of >1 events is useless.
//
// TODO: Tune based on benchmarking on realistic use cases
#define MAX_JOIN_INPOLL_IDENTIFIERS ((size_t)4)


// === Implementation of functions defined in wait.h ===

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait(udipe_future_t* future,
                            udipe_duration_ns_t timeout,
                            downstream_count_policy_t count_policy) {
    LOGGED_FUNCTION_START("%p, %zu, %d", future, timeout, count_policy)
        assert(timeout != UDIPE_DURATION_DEFAULT);

        debugf("Checking initial readiness of future %p...", future);
        const future_status_t status = future_status_load(
            future,
            // Synchronize-with the initial future state
            memory_order_acquire
        );
        future_status_debug_check(status, true);
        switch (status.state) {
        case STATE_RESULT:
            debug("Future was already in STATE_RESULT at start of wait.");
            return status;
        case STATE_WAITING:
        case STATE_PROCESSING:
        case STATE_CANCELING:
            debug("Future is not in STATE_RESULT yet, but getting there "
                  "may require a manual status word updates.");
            break;
        case STATE_UNINITIALIZED:
        case NUM_STATES:
        default:
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
            // Eager futures that are driven by a dedicated thread enjoy
            // automatic status updates by said worker threads...
            if (timeout == UDIPE_DURATION_MIN) {
                debug("Status is guaranteed to be up to date for this type, "
                      "so with a min timeout we can fail instantly.");
                return status;
            } else {
                // ...and a waiting logic that does not depend on the type.
                return future_wait_eager(future,
                                         status,
                                         timeout,
                                         count_policy);
            }
        case TYPE_JOIN:
            return future_wait_join(future, status, timeout, count_policy);
        case TYPE_UNORDERED:
            return future_wait_unordered(future,
                                         status,
                                         timeout,
                                         count_policy);
        case TYPE_TIMER_ONCE:
            return future_wait_timer_once(future,
                                          status,
                                          timeout,
                                          count_policy);
        case TYPE_TIMER_REPEAT:
            return future_wait_timer_repeat(future,
                                            status,
                                            timeout,
                                            count_policy);
        case TYPE_INVALID:
        case NUM_TYPES:
        default:
            errorf("Observed invalid future type %d", status.type);
            exit(EXIT_FAILURE);
        }
    LOGGED_FUNCTION_END
}

#define LOGGED_WAIT_START  \
    LOGGED_FUNCTION_START(  \
        "%p, { .dc = %u, .dco = %u, .a = %u, .s = %u, .o = %u, .t = %u, "  \
               ".na = %u, .ne/ll = %u, %u }, %zu, %d",  \
        future,  \
        latest_status.downstream_count,  \
        latest_status.downstream_count_overflow,  \
        latest_status.available,  \
        latest_status.state,  \
        latest_status.outcome,  \
        latest_status.type,  \
        latest_status.notify_address,  \
        latest_status.notify_event_or_lazy_lock,  \
        latest_status.reserved,  \
        timeout,  \
        count_policy  \
    )

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait_eager(udipe_future_t* future,
                                  future_status_t latest_status,
                                  udipe_duration_ns_t timeout,
                                  downstream_count_policy_t count_policy) {
    LOGGED_WAIT_START
        // Readiness and early exit should be handled upstream
        stopwatch_t stopwatch = stopwatch_initialize();
        future_status_debug_check(latest_status, true);
        assert((latest_status.type >= TYPE_NETWORK_START
                && latest_status.type < TYPE_NETWORK_END)
               || latest_status.type == TYPE_CUSTOM);
        assert(latest_status.state != STATE_RESULT);
        assert(timeout != UDIPE_DURATION_MIN && timeout != UDIPE_DURATION_DEFAULT);

        debug("Updating the future's status word...");
        bool success;
        do {
            future_status_t desired_status = latest_status;
            prepare_downstream_count_inc(&desired_status, count_policy);
            desired_status.notify_address = true;

            if (future_status_eq(desired_status, latest_status)) {
                debug("No status word update needed, skipping...");
                break;
            }
            future_status_debug_check(desired_status, true);
            success = future_status_compare_exchange_weak(
                future,
                &latest_status,
                desired_status,
                // - Need an acquire barrier so that our next actions cannot be
                //   reordered before this initial status setup.
                // - No release or seq_cst needed, previous operations on the shared
                //   state are unrelated to this transaction and can therefore
                //   safely be reordered after this initial status word setup.
                memory_order_acquire,
                // Synchronize-with concurrent future state changes.
                memory_order_acquire
            );

            if (success) {
                debug("Done updating the future status.");
                latest_status = desired_status;
                break;
            }
            future_status_debug_check(latest_status, true);
            if (latest_status.state == STATE_RESULT) {
                debug("Failed to update status, but result became available.");
                return latest_status;
            }
            debug("Failed because another thread updated the status word or "
                  "weak compare_exchange failed spuriously. Let's try again.");
        } while(true);

        debug("Waiting for the dedicated thread to update the future status...");
        switch (future_wait_by_adress(future,
                                      &latest_status,
                                      &timeout,
                                      &stopwatch)) {
        case ADDRESS_WAIT_SUCCESS:
        case ADDRESS_WAIT_TIMEOUT:
            debug("Either a result is available or the wait timed out.");
            switch (count_policy) {
            case DOWNSTREAM_COUNT_CYCLE:
                // - Need an acquire barrier to synchronize-with concurrent
                //   future state changes performed by other threads.
                // - Need a release barrier so that our previous operations
                //   cannot be reordered after this final refcount decrement.
                // - No need for sequential consistency as we're not
                //   synchronizing across multiple futures/other objects.
                return future_downstream_count_dec(future, memory_order_acq_rel);
            case DOWNSTREAM_COUNT_KEEP:
                return latest_status;
            default:
                exit_with_error("No other count_policy is supported yet");
            }
        case ADDRESS_WAIT_UNLOCKED:
        default:
            exit_with_error("Shouldn't happen for this future type");
        }
    LOGGED_FUNCTION_END
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait_join(udipe_future_t* future,
                                 future_status_t latest_status,
                                 udipe_duration_ns_t timeout,
                                 downstream_count_policy_t count_policy) {
    LOGGED_WAIT_START
        // TODO: Much of this code should be identical across all inpoll-based
        //       future types, with only handling of inpoll notifications
        //       differing, so a generic function with a callback should be
        //       eventually devised.
        // TODO: This function is also too complex and should be broken up.
        //       Perhaps the aforementioned refactor could be a first step in
        //       this direction.
        // TODO: Consider moving some of this to latched_inpoll.[ch]

        stopwatch_t stopwatch = stopwatch_initialize();
        future_status_debug_check(latest_status, true);
        assert(latest_status.type == TYPE_JOIN);
        assert(latest_status.state != STATE_RESULT);
        assert(timeout != UDIPE_DURATION_DEFAULT);

        do {
            debug("Preparing future status update...");
            bool previously_locked;
            do {
                future_status_t desired_status = latest_status;
                prepare_downstream_count_inc(&desired_status, count_policy);
                previously_locked = latest_status.notify_event_or_lazy_lock;
                if (previously_locked) {
                    debug("Another thread is already awaiting this future, "
                          "will await this thread via the futex path...");
                    desired_status.notify_address = true;
                } else {
                    debug("No thread is waiting yet, try to acquire the lock.");
                    desired_status.notify_event_or_lazy_lock = true;
                }

                if (future_status_eq(desired_status, latest_status)) {
                    debug("No status word update needed, skipping...");
                    break;
                }
                future_status_debug_check(desired_status, true);
                const bool success = future_status_compare_exchange_weak(
                    future,
                    &latest_status,
                    desired_status,
                    // - Need an acquire barrier so that our next actions are
                    //   not reordered before this initial status setup.
                    // - No release or seq_cst needed, previous operations on
                    //   shared state are unrelated to this transaction and can
                    //   safely be reordered after initial status word setup.
                    memory_order_acquire,
                    // Synchronize-with concurrent future state changes.
                    memory_order_acquire
                );

                if (success) {
                    debug("Successfully updated the future's status.");
                    latest_status = desired_status;
                    break;
                }
                future_status_debug_check(latest_status, true);
                if (latest_status.state == STATE_RESULT) {
                    debug("Failed to update the future's status, "
                          "but the result became available meanwhile.");
                    return latest_status;
                }
                debug(
                    "...and failed because another thread updated the status "
                    "or weak compare_exchange failed spuriously. Try again..."
                );
            } while(true);

            if (previously_locked) {
                debug("Waiting for the lazy_lock holder to "
                      "either yield a result or give up...");
                switch (future_wait_by_adress(future,
                                              &latest_status,
                                              &timeout,
                                              &stopwatch)) {
                case ADDRESS_WAIT_SUCCESS:
                case ADDRESS_WAIT_TIMEOUT:
                    debug("A result is available or the wait timed out.");
                    switch (count_policy) {
                    case DOWNSTREAM_COUNT_CYCLE:
                        // - Need an acquire barrier to synchronize-with
                        //   concurrent future state changes performed by other
                        //   threads.
                        // - Need a release barrier so that our previous
                        //   operations can't be reordered before this final
                        //   decrement.
                        // - No need for sequential consistency as we're not
                        //   synchronizing across multiple futures/other
                        //   objects.
                        return future_downstream_count_dec(
                            future,
                            memory_order_acq_rel
                        );
                    case DOWNSTREAM_COUNT_KEEP:
                        return latest_status;
                    default:
                        exit_with_error(
                            "No other count_policy is supported yet"
                        );
                    }
                case ADDRESS_WAIT_UNLOCKED:
                    debug("The lazy_lock holder gave up, let's try to "
                          "take its place.");
                    continue;
                }
            } else {
                #ifdef __linux__
                    assert(latest_status.outcome == OUTCOME_UNKNOWN);
                    future_outcome_t outcome = OUTCOME_UNKNOWN;
                    do {
                        debug("Beginning wait for inpoll events...");
                        uint64_t identifiers[MAX_JOIN_INPOLL_IDENTIFIERS];
                        size_t result = inpoll_wait(
                            future->status_sync.latched_inpoll,
                            identifiers,
                            MAX_JOIN_INPOLL_IDENTIFIERS,
                            timeout
                        );

                        bool inpoll_timed_out = false;
                        switch (result) {
                        case 0:
                            debug("Reached a timeout during inpoll_wait().");
                            assert(timeout < UDIPE_DURATION_MAX);
                            inpoll_timed_out = true;
                            break;
                        default:
                            // Expecting at least one ready event on this branch
                            debug("Successfully received events from inpoll.");
                            collective_upstream_t* const upstream_set =
                                &future->specific.join.upstream;
                            for (size_t i = 0; i < (size_t)result; ++i) {
                                // Because we hold the lock that controls status
                                // updates other than cancelation, eventfd
                                // notifications can only occur if this future
                                // was concurrently canceled.
                                const size_t upstream_index = identifiers[i];
                                if (upstream_index == INPOLL_LATCH_ID) {
                                    // Synchronize-with concurrent state changes
                                    latest_status = future_status_load(
                                        future,
                                        memory_order_acquire
                                    );
                                    ensure_eq((size_t)latest_status.state,
                                              (size_t)STATE_RESULT);
                                    ensure_eq((size_t)latest_status.outcome,
                                              (size_t)OUTCOME_FAILURE_CANCELED);
                                    outcome = latest_status.outcome;
                                    break;
                                }
                                ensure_lt(upstream_index, upstream_set->length);

                                // Find the upstream future that became ready
                                // and check/update its status word
                                udipe_future_t* const upstream =
                                    upstream_set->array[upstream_index];
                                future_status_t upstream_status =
                                    future_wait(upstream,
                                                UDIPE_DURATION_MIN,
                                                DOWNSTREAM_COUNT_KEEP);
                                future_status_debug_check(upstream_status,
                                                          true);

                                // If this upstream future is not ready yet,
                                // we'll need to keep polling it until it is
                                // ready or another future errors out.
                                if (upstream_status.state != STATE_RESULT) {
                                    continue;
                                }

                                // If this upstream future's outcome is now
                                // known, remove it from our inpoll set...
                                ensure_ge(upstream_set->remaining, (uint32_t)1);
                                --(upstream_set->remaining);
                                const fd_t upstream_fd =
                                    upstream->status_sync.any;
                                inpoll_detach(
                                    future->status_sync.latched_inpoll,
                                    upstream_fd
                                );

                                // Integrate the upstream future's outcome into
                                // our own outcome.
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
                                case NUM_OUTCOMES:
                                default:
                                    exit_with_error(
                                        "These outcomes should not be observed "
                                        "in STATE_RESULT"
                                    );
                                }
                            }
                            if (outcome != OUTCOME_UNKNOWN) break;
                            if (upstream_set->remaining == 0) {
                                outcome = OUTCOME_SUCCESS;
                                break;
                            }
                        }
                        const bool outcome_known = (outcome != OUTCOME_UNKNOWN);
                        const bool canceled =
                            (outcome == OUTCOME_FAILURE_CANCELED);

                        bool reached_timeout = inpoll_timed_out;
                        if (!outcome_known) {
                            udipe_duration_ns_t elapsed_time =
                                stopwatch_measure(&stopwatch);
                            if (elapsed_time >= timeout) {
                                debug("Reached timeout after inpoll_wait().");
                                reached_timeout = true;
                            } else {
                                debug("Updating timeout after inpoll_wait().");
                                timeout -= elapsed_time;
                            }
                        }

                        if (!(outcome_known || reached_timeout)) {
                            debug(
                                "No timeout or outcome yet: resuming wait..."
                            );
                            continue;
                        }
                        // From this point, !outcome_known => reached_timeout

                        do {
                            debug("Preparing future status update...");
                            future_status_t desired_status = latest_status;
                            prepare_downstream_count_dec(&desired_status,
                                                         count_policy);
                            if (outcome_known) {
                                desired_status.state = STATE_RESULT;
                                desired_status.outcome = outcome;
                            } else {
                                assert(reached_timeout);
                            }
                            assert(latest_status.notify_event_or_lazy_lock);
                            desired_status.notify_event_or_lazy_lock = false;

                            future_status_debug_check(desired_status, true);
                            const bool success =
                                future_status_compare_exchange_weak(
                                    future,
                                    &latest_status,
                                    desired_status,
                                    // - Need an acquire barrier to ensure
                                    //   subsequent notifications aren't
                                    //   reordered before the status change that
                                    //   they are supposed to notify.
                                    // - Need a release barrier so that our
                                    //   previous operations can't be reordered
                                    //   after this lazy_lock release.
                                    // - No need for sequential consistency as
                                    //   we're not synchronizing multiple
                                    //   futures/other objects.
                                    memory_order_acq_rel,
                                    // Synchronize-with concurrent changes.
                                    memory_order_acquire
                                );

                            if (success) {
                                debug(
                                    "Successfully updated the future's status."
                                );
                                latest_status = desired_status;
                                break;
                            }
                            future_status_debug_check(latest_status, true);
                            debug(
                                "...and failed because another thread updated "
                                "the status word or weak compare_exchange "
                                "failed spuriously. Let's try again...");
                        } while(true);

                        if (latest_status.downstream_count == 0 || canceled) {
                            // - No notification needed if no thread is waiting for
                            //   this future's status to change. Any thread that
                            //   observes this future's final status afterwards
                            //   should not need to query its eventfd to know that
                            //   it's already in STATE_RESULT.
                            // - No notification needed if we were canceled, the
                            //   thread that switched to a canceled status will take
                            //   care of notifying other threads for us.
                        } else if (outcome_known) {
                            debug(
                                "Letting other threads know that the outcome "
                                "of this join future is now known."
                            );
                            if (latest_status.notify_address) {
                                debug("...including other threads which were "
                                      "waiting for lazy_lock.");
                                wake_by_address_all(&future->status_word);
                            }
                            event_signal(future->specific.join.inpoll_latch);
                        } else {
                            assert(reached_timeout);
                            debug("Reached timeout without completing the wait.");
                            if (latest_status.notify_address) {
                                debug("Will now wake up one waiter (if any) to "
                                      "take over the lazy_lock.");
                                // Use of wake_by_address_single() avoids the
                                // thundering herd that would occur if multiple
                                // waiters woke only to immediately race to acquire
                                // lazy_lock and fall back asleep.
                                wake_by_address_single(&future->status_word);
                            } else {
                                debug("No other thread could be waiting for "
                                      "lazy_lock at the time where it freed "
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
    LOGGED_FUNCTION_END
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait_unordered(udipe_future_t* future,
                                      future_status_t latest_status,
                                      udipe_duration_ns_t timeout,
                                      downstream_count_policy_t count_policy) {
    LOGGED_WAIT_START
        // TODO implement
        exit_with_error("This function is not yet implemented");
    LOGGED_FUNCTION_END
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait_timer_once(
    udipe_future_t* future,
    future_status_t latest_status,
    udipe_duration_ns_t timeout,
    downstream_count_policy_t count_policy
) {
    LOGGED_WAIT_START
        stopwatch_t stopwatch = stopwatch_initialize();
        future_status_debug_check(latest_status, true);
        assert(latest_status.type == TYPE_TIMER_ONCE);
        assert(latest_status.state != STATE_RESULT);
        assert(timeout != UDIPE_DURATION_DEFAULT);

        switch(count_policy) {
        case DOWNSTREAM_COUNT_CYCLE:
            debug("Registering as a waiter...");
            const bool success =
                future_downstream_count_try_inc(future, &latest_status);
            future_status_debug_check(latest_status, true);
            if (!success) {
                ensure_eq((size_t)latest_status.state, (size_t)STATE_RESULT);
                debug("Future reached STATE_RESULT before we started waiting");
                return latest_status;
            }
            break;
        case DOWNSTREAM_COUNT_KEEP:
            // Not checking the future's initial state again if we don't need to
            // increment the downstream count: future_wait() already checked it
            // right before calling this function, it is unlikely to have
            // changed.
            break;
        }

        #ifdef __linux__

            // TODO: Extract commonalities wrt other fd waits

            debug("Encoding timerfd into a pollfd...");
            struct pollfd timer = (struct pollfd){
                .fd = future->status_sync.timer_once,
                .events = POLLIN,
                .revents = 0
            };

            do {
                struct timespec delay;
                struct timespec* pdelay = make_unix_timeout(&delay, timeout);

                debug("Waiting for timerfd readiness...");
                int result = ppoll(&timer,
                                   1,
                                   pdelay,
                                   NULL);
                switch (result) {
                case 1:
                    ensure_eq(timer.revents, POLLIN);
                    debug("timerfd is now ready. Will now propagate the good "
                          "news while decrementing our downstream_count");
                    do {
                        future_status_t desired_status = latest_status;
                        prepare_downstream_count_dec(&desired_status,
                                                     count_policy);
                        desired_status.state = STATE_RESULT;
                        if (desired_status.outcome == OUTCOME_UNKNOWN) {
                            // Only if OUTCOME_UNKNOWN to avoid overriding a
                            // former cancelation signal.
                            desired_status.outcome = OUTCOME_SUCCESS;
                        }

                        if (future_status_eq(desired_status, latest_status)) {
                            debug("No status word update needed, skipping...");
                            break;
                        }
                        future_status_debug_check(desired_status, true);
                        const bool success =
                            future_status_compare_exchange_weak(
                                future,
                                &latest_status,
                                desired_status,
                                // - No acquire barrier needed: we're not
                                //   observing new future states on success and
                                //   the next operations are unrelated to this
                                //   transaction and can safely be reordered
                                //   before this status update.
                                // - Need a release barrier so that our previous
                                //   operations cannot be reordered before this
                                //   status update, which is meant to expose
                                //   their outcome.
                                // - No need for sequential consistency as we're
                                //   not synchronizing multiple objects.
                                memory_order_release,
                                // Synchronize-with concurrent state changes.
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
                    debug("Reached timeout before the future became ready");
                    return latest_status;
                case -1:
                    switch (errno) {
                        case EINTR:
                            errno = 0;
                            debug("Interrupted by signal, updating timeout...");
                            udipe_duration_ns_t elapsed_time =
                                stopwatch_measure(&stopwatch);
                            if (elapsed_time >= timeout) {
                                debug(
                                    "Reached timeout before future became ready"
                                );
                                return latest_status;
                            } else {
                                timeout -= elapsed_time;
                                continue;
                            }
                        case EFAULT:  // No such fd
                        case EINVAL:  // nfds too high or timeout is negative
                        case ENOMEM:  // Unable to allocate kernel memory
                        default:
                            exit_after_c_error(
                                "These errors should never happen"
                            );
                    }
                default:
                    exit_with_error(
                        "This result should never be returned"
                    );
                }
            } while (true);
        #else
            // TODO add windows version based on waitable timers.
            #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
        #endif
    LOGGED_FUNCTION_END
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
future_status_t future_wait_timer_repeat(udipe_future_t* future,
                                         future_status_t latest_status,
                                         udipe_duration_ns_t timeout,
                                         downstream_count_policy_t count_policy) {
    LOGGED_WAIT_START
        // TODO implement
        exit_with_error("This function is not yet implemented");
    LOGGED_FUNCTION_END
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
address_wait_outcome_t future_wait_by_adress(
    udipe_future_t* future,
    future_status_t* latest_status,
    udipe_duration_ns_t* timeout,
    stopwatch_t* stopwatch
) {
    LOGGED_FUNCTION_START("%p, %p, %p, %p",
                          future, latest_status, timeout, stopwatch)
        assert(latest_status->state != STATE_RESULT);
        assert(timeout != UDIPE_DURATION_DEFAULT);

        debug("Checking if future is awaited under lazy_lock protection...");
        const bool lazy = future_type_uses_lazy_lock(latest_status->type);
        if (lazy) {
            // This function should only be called if waiting the output
            // synchronization object is not possible because another thread is
            // already in the process of monitoring it.
            assert(latest_status->notify_event_or_lazy_lock);
        }
        // This function should only be called after directing the threads
        // updating the status word that they needs to send a notification.
        assert(latest_status->notify_address);


        do {
            debug("Waiting for a futex-signaled status change...");
            const bool success = future_status_wait(future,
                                                    *latest_status,
                                                    *timeout);

            if (success) {
                debug("Checking the new state...");
                // Synchronize-with freshly signaled future state changes
                *latest_status = future_status_load(future,
                                                    memory_order_acquire);
                future_status_debug_check(*latest_status, true);
                if (latest_status->state == STATE_RESULT) {
                    debug("Future result is now available: we're done.");
                    break;
                } else if (lazy && !latest_status->notify_event_or_lazy_lock) {
                    debug(
                        "Lazy future has been unlocked: "
                        "must switch to inpoll_wait() or wake up next waiter."
                    );
                    break;
                }
                debug("Did not reach any of the state we are waiting for!");
            } else {
                debug("Wait failed due to timeout or other cause (signal etc).");
            }

            debug("Measuring elapsed time since last clock check...");
            udipe_duration_ns_t elapsed_time = stopwatch_measure(stopwatch);
            if (elapsed_time >= *timeout) {
                debug("Timeout reached without observing STATE_RESULT.");
                *timeout = UDIPE_DURATION_MIN;
                break;
            } else {
                debug("Updating timer for the next round of waiting...");
                *timeout -= elapsed_time;
            }
        } while(true);

        debug("Assessing final status...");
        if (latest_status->state == STATE_RESULT) {
            return ADDRESS_WAIT_SUCCESS;
        } else if (lazy && !latest_status->notify_event_or_lazy_lock) {
            return ADDRESS_WAIT_UNLOCKED;
        } else {
            return ADDRESS_WAIT_TIMEOUT;
        }
    LOGGED_FUNCTION_END
}
