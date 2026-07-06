#include "future.h"

#include <udipe/duration.h>
#include <udipe/nodiscard.h>
#include <udipe/pointer.h>
#include <udipe/result.h>

#include "future/allocator.h"
#include "future/outcome.h"
#include "future/state.h"
#include "future/status.h"
#include "future/status_ops.h"
#include "future/type.h"
#include "future/wait.h"

#include "address_wait.h"
#include "context.h"
#include "error.h"
#include "event.h"
#include "log.h"
#include "visibility.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>


// === Awaiting future results ===

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_PUBLIC
udipe_result_t udipe_finish(udipe_future_t* future) {
    LOGGER_START(&future->context->logger)
        tracef("Marking future %p as liberated...", future);
        // Synchronize-with the initial future state
        future_status_t latest_status =
            future_status_load(future, memory_order_acquire);
        do {
            future_status_debug_check(latest_status, true);
            assert(("Should not call udipe_finish() on a future that's still used",
                    (size_t)latest_status.downstream_count == (size_t)0));
            assert(("Should only call udipe_finish() once per future",
                    latest_status.available));

            future_status_t desired_status = latest_status;
            desired_status.available = false;
            future_status_debug_check(desired_status, true);
            const bool success = future_status_compare_exchange_weak(
                future,
                &latest_status,
                desired_status,
                // - Need an acquire barrier so that no post-liberation actions
                //   are taken before liberation is signaled to other threads.
                // - Need a release barrier so that all previous changes
                //   (especially other operations on the same future) become
                //   visible to other threads before future liberation starts
                //   from their perspective.
                // - No need for sequential consistency as we're not
                //   synchronizing across multiple futures/other objects.
                memory_order_acq_rel,
                // Synchronize-with concurrent future state changes.
                memory_order_acquire
            );

            if (success) {
                trace("Done updating the future status.");
                latest_status = desired_status;
                break;
            }
            future_status_debug_check(latest_status, true);
            trace("Failed because another thread updated the status word or "
                  "weak compare_exchange failed spuriously. Let's try again.");
        } while(true);

        if (latest_status.state == STATE_RESULT) {
            trace("Result was available from the start!");
        } else {
            trace("Waiting for the result to become available...");
            latest_status = future_wait(future,
                                        UDIPE_DURATION_MAX,
                                        DOWNSTREAM_COUNT_KEEP);
            future_status_debug_check(latest_status, true);
            assert(latest_status.state == STATE_RESULT);
        }

        trace("Collecting the end result...");
        udipe_result_t result = (udipe_result_t){ 0 };
        switch (latest_status.outcome) {
        case OUTCOME_SUCCESS:
        case OUTCOME_FAILURE_INTERNAL:
            trace("Future went far enough to produce a typed result");
            bool is_network = false;
            switch (latest_status.type) {
            case TYPE_NETWORK_CONNECT:
                result.type = UDIPE_CONNECT;
                is_network = true;
                break;
            case TYPE_NETWORK_DISCONNECT:
                result.type = UDIPE_DISCONNECT;
                is_network = true;
                break;
            case TYPE_NETWORK_SEND:
                result.type = UDIPE_SEND;
                is_network = true;
                break;
            case TYPE_NETWORK_RECV:
                result.type = UDIPE_RECV;
                is_network = true;
                break;
            case TYPE_CUSTOM:
                result.type = UDIPE_CUSTOM;
                result.payload.custom = future->specific.custom_payload;
                break;
            case TYPE_JOIN:
                result.type = UDIPE_JOIN;
                // No payload for this result type, which cannot fail internally
                assert(latest_status.outcome != OUTCOME_FAILURE_INTERNAL);
                break;
            case TYPE_UNORDERED:
                result.type = UDIPE_UNORDERED;
                result.payload.unordered = future->specific.unordered.payload;
                break;
            case TYPE_TIMER_ONCE:
                result.type = UDIPE_TIMER_ONCE;
                // No payload for this result type, which cannot fail internally
                assert(latest_status.outcome != OUTCOME_FAILURE_INTERNAL);
                break;
            case TYPE_TIMER_REPEAT:
                result.type = UDIPE_TIMER_REPEAT;
                result.payload.timer_repeat = future->specific.timer_repeat.payload;
                break;
            case TYPE_INVALID:
            case NUM_TYPES:
            default:
                exit_with_error("Should never happen.");
            }
            if (is_network) {
                result.payload.network = future->specific.network;
            }
            break;
        case OUTCOME_FAILURE_DEPENDENCY:
            trace("Future failed because one of its dependencies has failed.");
            result.type = UDIPE_FAILURE_DEPENDENCY;
            break;
        case OUTCOME_FAILURE_CANCELED:
            trace("Future failed because it was canceled.");
            result.type = UDIPE_FAILURE_CANCELED;
            break;
        case OUTCOME_UNKNOWN:
        case NUM_OUTCOMES:
        default:
            exit_with_error("Should never happen");
        }

        trace("Liberating the future...");
        future_liberate(future);
        return result;
    LOGGER_END
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
DEFINE_PUBLIC
bool udipe_wait(udipe_future_t* future, udipe_duration_ns_t timeout) {
    LOGGER_START(&future->context->logger)
        if (timeout == UDIPE_DURATION_DEFAULT) timeout = UDIPE_DURATION_MAX;
        const future_status_t final_status =
            future_wait(future, timeout, DOWNSTREAM_COUNT_CYCLE);
        return final_status.state == STATE_RESULT;
    LOGGER_END
}


// === Custom futures ===

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
UDIPE_PUBLIC
udipe_future_t* udipe_start_custom(udipe_context_t* context) {
    LOGGER_START(&context->logger)
        debug("Setting up a custom future...");
        udipe_future_t* const future = future_allocate(context, TYPE_CUSTOM);
        assert(future->context == context);
        assert((future_type_t)future_status_load(future,
                                                 memory_order_relaxed).type
               == TYPE_CUSTOM);
        assert(future->status_sync.event != EVENT_INVALID);
        const future_status_t status = (future_status_t){
            .downstream_count = 0,
            .downstream_count_overflow = false,
            .available = true,
            .state = STATE_PROCESSING,
            .outcome = OUTCOME_UNKNOWN,
            .type = TYPE_CUSTOM,
            .notify_address = false,
            .notify_event_or_lazy_lock = false,
            .reserved = 0
        };
        future_status_debug_check(status, true);
        future_status_store(
            future,
            status,
            // Relaxed ordering is fine at this point because only one thread
            // has access to this future and exposing the future to other
            // threads will involve a release memory operation.
            memory_order_relaxed
        );
        return future;
    LOGGER_END
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_PUBLIC
bool udipe_custom_canceled(udipe_future_t* custom) {
    LOGGER_START(&custom->context->logger)
        trace("Checking if custom future is canceled...");
        const future_status_t status = future_status_load(
            custom,
            // Can use relaxed ordering here because we're not reading from any
            // other future field or using this load to synchronize later
            // accesses to other future fields.
            memory_order_relaxed
        );
        return future_custom_check_canceled(status);
    LOGGER_END
}

UDIPE_NON_NULL_ARGS
UDIPE_PUBLIC
void udipe_custom_acknowledge_cancel(udipe_future_t* custom) {
    LOGGER_START(&custom->context->logger)
        debug("Acknowledging a future's cancelation...");
        future_status_t status = future_status_load(
            custom,
            // No need to synchronize with any state other than the status word
            memory_order_relaxed
        );

        do {
            debug("Checking the current future status...");
            ensure(future_custom_check_canceled(status));

            debug("Trying to mark the future as fully canceled...");
            future_status_t desired = status;
            desired.state = STATE_RESULT;
            const bool success = future_status_compare_exchange_weak(
                custom,
                &status,
                desired,
                // Publish our changes with release ordering so that the thread
                // that observes the canceled state also observes anything we
                // did before acknowledging the cancelation.
                memory_order_release,
                // No need to synchronize with any state other than the status
                memory_order_relaxed
            );
            if (success) {
                debug("Success, will now notify other threads as needed!");
                status = desired;
                break;
            } else {
                debug("Raced with another thread or spurious failure occured, "
                      "must try again...");
                continue;
            }
        } while(true);
        future_notify_eager_outcome(custom, status);
    LOGGER_END
}

UDIPE_NON_NULL_ARGS
UDIPE_PUBLIC
bool udipe_custom_try_set_result(udipe_future_t* custom,
                                 bool successful,
                                 udipe_custom_payload_t payload) {
    LOGGER_START(&custom->context->logger)
        debug("Attempting to set a future's result...");
        future_status_t status = future_status_load(
            custom,
            // Can use relaxed ordering here because we're not reading from any
            // other future field or using this load to synchronize later
            // accesses to other future fields.
            memory_order_relaxed
        );

        bool payload_set = false;
        do {
            debug("Checking the current future status...");
            if (future_custom_check_canceled(status)) {
                udipe_custom_acknowledge_cancel(custom);
                return false;
            }

            if (!payload_set) {
                debug("Setting the result payload...");
                custom->specific.custom_payload = payload;
                payload_set = true;
            }

            debug("Trying to mark the future as finished...");
            future_status_t desired = status;
            desired.state = STATE_RESULT;
            desired.outcome = OUTCOME_SUCCESS;
            const bool success = future_status_compare_exchange_weak(
                custom,
                &status,
                desired,
                // Publish our changes with release ordering so that the thread
                // that observes the result payload also observes anything we
                // did before publishing this payload.
                memory_order_release,
                // No need to synchronize with any state other than the status
                memory_order_relaxed
            );
            if (success) {
                debug("Success, will now notify other threads as needed!");
                status = desired;
                break;
            } else {
                debug("Raced with another thread or spurious failure occured, "
                      "must try again...");
                continue;
            }
        } while(true);
        future_notify_eager_outcome(custom, status);
        return true;
    LOGGER_END
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


// === Private methods ===

UDIPE_NON_NULL_ARGS
void future_notify_eager_outcome(udipe_future_t* future,
                                 future_status_t status) {
    #ifndef NDEBUG
        switch (status.type) {
        case TYPE_NETWORK_CONNECT:  // Aliases TYPE_NETWORK_START
        case TYPE_NETWORK_DISCONNECT:
        case TYPE_NETWORK_SEND:
        case TYPE_NETWORK_RECV:
        case TYPE_CUSTOM:  // Aliases TYPE_NETWORK_END
            break;
        case TYPE_TIMER_ONCE:
            exit_with_error("TIMER_ONCE futures are not eagerly signaled.");
        #ifdef __linux__
            case TYPE_JOIN:
            case TYPE_UNORDERED:
            case TYPE_TIMER_REPEAT:
                exit_with_error(
                    "This future type is not eagerly signaled on Linux."
                );
        #else
            // TODO: Add windows versions
            #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
        #endif
        case TYPE_INVALID:  // Never valid
        case NUM_TYPES:  // Never valid
        default:
            exit_with_error("Not a valid future type.");
        }
    #endif

    if (status.downstream_count) {
        // Ensure these notifications are not sent before the new future
        // status word has been published.
        atomic_thread_fence(memory_order_acquire);
        if (status.notify_address) {
            debug("Sending a futex-based status change notification...");
            wake_by_address_all(&future->status_word);
        }
        if (status.notify_event_or_lazy_lock) {
            debug("Sending an event-based status change notification...");
            event_signal(future->status_sync.event);
        }
    } else {
        debug("No one could be waiting for a status change, "
              "so there is no need for a change notification.");
    }
}

UDIPE_NODISCARD
bool future_custom_check_canceled(future_status_t status) {
    debug("Checking a pending custom future's status...");
    future_status_debug_check(status, true);
    ensure_eq((future_type_t)status.type,
              (future_type_t)TYPE_CUSTOM);

    #ifndef NDEBUG
        switch (status.outcome) {
        case OUTCOME_UNKNOWN:  // Valid un-canceled state
            ensure_eq((future_state_t)status.state,
                      (future_state_t)STATE_PROCESSING);
            break;
        case OUTCOME_FAILURE_CANCELED:  // Valid canceled state
            ensure_eq((future_state_t)status.state,
                      (future_state_t)STATE_CANCELING);
            break;
        // These outcomes can only be set by try_set_result(), and the
        // thread that implements the custom operation and performs this
        // check should stop using the future after calling this function.
        case OUTCOME_SUCCESS:
        case OUTCOME_FAILURE_INTERNAL:
            exit_with_error("Used a custom future after freeing it!");
        case OUTCOME_FAILURE_DEPENDENCY:  // Not valid for custom futures
        case NUM_OUTCOMES:  // Never valid
        default:
            exit_with_error("Observed an invalid custom future outcome!");
        }
    #endif

    switch (status.state) {
    case STATE_PROCESSING:  // Valid un-canceled state
        return false;
    case STATE_CANCELING:  // Valid canceled state
        return true;
    // Set by try_set_result() or acknowledge_cancel() and future must
    // not be used again after calling those functions.
    case STATE_RESULT:
        exit_with_error("Used a custom future after freeing it!");
    case STATE_UNINITIALIZED:  // Not valid for any initialized future
    case STATE_WAITING:  // Not valid for custom futures
    case NUM_STATES:  // Never valid
    default:
        exit_with_error("Observed an invalid custom future state!");
    }
}


#ifdef UDIPE_BUILD_TESTS

    void future_unit_tests() {
        info("Running future unit tests...");

        future_status_unit_tests();

        // TODO: Add more future tests as they come up. In particular, need to
        //       test all future_wait() variants + new status word
        //       manipulations.
    }

#endif  // UDIPE_BUILD_TESTS
