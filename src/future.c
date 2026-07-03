#include "future.h"

#include <udipe/duration.h>
#include <udipe/nodiscard.h>
#include <udipe/pointer.h>
#include <udipe/result.h>

#include "future/allocator.h"
#include "future/outcome.h"
#include "future/state.h"
#include "future/status_ops.h"
#include "future/type.h"
#include "future/wait.h"

#include "context.h"
#include "error.h"
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
                result.payload.custom = future->specific.custom;
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

    void future_unit_tests() {
        info("Running future unit tests...");

        future_status_unit_tests();

        // TODO: Add more future tests as they come up. In particular, need to
        //       test all future_wait() variants + new status word
        //       manipulations.
    }

#endif  // UDIPE_BUILD_TESTS
