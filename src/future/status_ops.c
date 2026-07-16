#include "status_ops.h"

#include "outcome.h"
#include "state.h"
#include "status.h"
#include "type.h"

#include "../error.h"
#include "../log.h"
#include "../unit_tests.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>


void future_status_debug_check(future_status_t status,
                               bool is_allocated) {
    #ifdef NDEBUG
        // Don't do anything in Release builds. This allows us to use eager
        // checks in utility functions that this function calls.
        return;
    #endif

    assert(is_allocated || !status.available);

    assert(("65k dependents ought to be enough for anybody",
            !status.downstream_count_overflow));
    assert(("Futures can only be used between allocation and start of udipe_finish()",
            (status.downstream_count == 0) || status.available));
    assert(("Should be true unless MAX_DOWNSTREAM_COUNT define needs updating",
            status.downstream_count <= MAX_DOWNSTREAM_COUNT));

    assert(is_allocated ^ (status.type == TYPE_INVALID));
    const bool has_dependencies = future_type_has_dependencies(status.type);
    const bool has_processing = future_type_has_processing(status.type);
    const bool uses_lazy_lock = future_type_uses_lazy_lock(status.type);
    const bool uses_worker = future_type_uses_worker_thread(status.type);

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
        could_be_locked = uses_lazy_lock;
        break;
    case STATE_PROCESSING:
        assert(is_allocated);
        assert(has_processing);
        has_outcome = false;
        could_be_locked = uses_lazy_lock;
        break;
    case STATE_CANCELING:
        assert(is_allocated);
        assert(uses_worker);
        has_outcome = true;
        could_be_locked = false;
        break;
    case STATE_RESULT:
        assert(is_allocated);
        has_outcome = true;
        // Can be locked in the RESULT state if a lazy future gets canceled
        // while a thread acquired the lock to call inpoll_wait().
        could_be_locked = uses_lazy_lock;
        break;
    case NUM_STATES:
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
    case NUM_OUTCOMES:
    default:
        assert(("Never valid", false));
    }

    if (is_allocated) {
        if (uses_lazy_lock) {
            // notify_address can be switched on from the fist state onwards,
            // and remains on after being turned on. Or it may never be switched
            // on. So we can't tell anything about its value. On the other hand,
            // we know of some cases where lazy_lock cannot be set.
            if (status.notify_event_or_lazy_lock) assert(could_be_locked);
        } else {
            // notify_address and notify_event can be switched on from the
            // WAITING state, and remain on after being turned on. They may also
            // never be switched on. So we can only tell something about their
            // values for future types that don't use events i.e. are not
            // implemented using asynchronous worker threads.
            if (!uses_worker) assert(!status.notify_event_or_lazy_lock);
        }
    } else {
        assert(!status.notify_address);
        assert(!status.notify_event_or_lazy_lock);
    }

    assert(status.reserved == 0);
}


#ifdef UDIPE_BUILD_TESTS

    #define NUM_TRIALS  ((size_t)(50*1000))

    typedef enum random_status_kind_e {
        STATUS_KIND_UNALLOCATED = 0,
        STATUS_KIND_AVAILABLE,
        STATUS_KIND_FINISHING,
        STATUS_KIND_ANY,  ///< Must come after true status kinds
        STATUS_KIND_ALLOCATED  ///< Must come after STATUS_KIND_ANY
    } random_status_kind_t;

    future_status_t random_status(random_status_kind_t kind) {
        LOGGED_FUNCTION_START("%d", kind)
            future_status_t result;

            switch (kind) {
            case STATUS_KIND_UNALLOCATED:
            case STATUS_KIND_FINISHING:
                result.downstream_count = 0;
                break;
            case STATUS_KIND_AVAILABLE:
                switch (rand() % 5) {
                case 0:
                    result.downstream_count = 0;
                    break;
                case 4:
                    result.downstream_count = MAX_DOWNSTREAM_COUNT;
                    break;
                default:
                    result.downstream_count =
                        (rand() % (MAX_DOWNSTREAM_COUNT - 1)) + 1;
                }
                break;
            case STATUS_KIND_ANY:
                return random_status(rand() % STATUS_KIND_ANY);
            case STATUS_KIND_ALLOCATED:
                return random_status(STATUS_KIND_AVAILABLE + (rand() % 2));
            }
            trace_expr(result.downstream_count);

            result.downstream_count_overflow = false;

            switch(kind) {
            case STATUS_KIND_UNALLOCATED:
                result.type = TYPE_INVALID;
                break;
            case STATUS_KIND_AVAILABLE:
            case STATUS_KIND_FINISHING:
                // TODO: Re-enable network types once fully implemented
                do {
                    result.type = (rand() % (NUM_TYPES - 1)) + 1;
                } while (result.type >= TYPE_NETWORK_START
                         && result.type < TYPE_NETWORK_END);
                break;
            case STATUS_KIND_ANY:
            case STATUS_KIND_ALLOCATED:
            default:
                exit_with_error("Excluded above");
            }
            trace_expr((size_t)result.type);

            result.available = (kind == STATUS_KIND_AVAILABLE);
            trace_expr((bool)result.available);

            const bool has_dependencies =
                future_type_has_dependencies(result.type);
            const bool has_processing = future_type_has_processing(result.type);
            bool could_be_locked;
            switch (result.type) {
            case TYPE_INVALID:
                result.state = STATE_UNINITIALIZED;
                could_be_locked = false;
                break;
            case TYPE_NETWORK_CONNECT:
            case TYPE_NETWORK_DISCONNECT:
            case TYPE_NETWORK_SEND:
            case TYPE_NETWORK_RECV:
                // Network futures can be in all possible states: WAITING,
                // PROCESSING, CANCELING and RESULT
                result.state = (rand() % (NUM_STATES - 1)) + 1;
                could_be_locked = false;
                break;
            case TYPE_CUSTOM:
                // Custom futures can be in states PROCESSING, CANCELING and RESULT,
                // but not in the WAITING state as they have no dependencies.
                result.state = (rand() % (NUM_STATES - 2)) + 2;
                could_be_locked = false;
                break;
            case TYPE_JOIN:
            case TYPE_UNORDERED:
                // Collective futures can only be in the WAITING and RESULT states,
                // but not in the PROCESSING state as they switch to RESULT as soon
                // as their dependencies are ready or canceled, and not in the
                // CANCELING state as there's not need to notify a worker thread to
                // cancel, so their cancelation process is instantaneous.
                if (rand() % 2) {
                    result.state = STATE_WAITING;
                } else {
                    result.state = STATE_RESULT;
                }
                could_be_locked = true;
                break;
            case TYPE_TIMER_ONCE:
            case TYPE_TIMER_REPEAT:
                // Timer futures can only be in the PROCESSING and RESULT states,
                // but not in the WAITING state as they have no deps, and not in the
                // CANCELING state as there's not need to notify a worker thread to
                // cancel, so their cancelation process is instantaneous.
                if (rand() % 2) {
                    result.state = STATE_PROCESSING;
                } else {
                    result.state = STATE_RESULT;
                }
                could_be_locked = (result.type != TYPE_TIMER_ONCE);
                break;
            case NUM_TYPES:
            default:
                exit_with_error("Should never happen");
            }
            trace_expr((size_t)result.state);

            switch (result.state) {
            case STATE_UNINITIALIZED:
            case STATE_WAITING:
            case STATE_PROCESSING:
                result.outcome = OUTCOME_UNKNOWN;
                break;
            case STATE_CANCELING:
                result.outcome = OUTCOME_FAILURE_CANCELED;
                break;
            case STATE_RESULT:
                future_outcome_t outcomes[NUM_OUTCOMES];
                size_t outcomes_len = 0;
                outcomes[outcomes_len++] = OUTCOME_SUCCESS;
                outcomes[outcomes_len++] = OUTCOME_FAILURE_CANCELED;
                if (has_dependencies) {
                    outcomes[outcomes_len++] = OUTCOME_FAILURE_DEPENDENCY;
                }
                if (has_processing) {
                    outcomes[outcomes_len++] = OUTCOME_FAILURE_INTERNAL;
                }
                result.outcome = outcomes[rand() % outcomes_len];
                break;
            case NUM_STATES:
            default:
                exit_with_error("Should never happen");
            }
            trace_expr((size_t)result.outcome);

            result.notify_address = (result.type == TYPE_INVALID) ? false
                                                                  : rand() % 2;
            if (future_type_uses_lazy_lock(result.type)) {
                result.notify_event_or_lazy_lock = could_be_locked ? rand() % 2
                                                                   : false;
            } else {
                const bool uses_worker =
                    future_type_uses_worker_thread(result.type);
                result.notify_event_or_lazy_lock = uses_worker ? rand() % 2
                                                               : false;
            }
            trace_expr((size_t)result.notify_address);
            trace_expr((bool)result.notify_event_or_lazy_lock);

            result.reserved = 0;
            return result;
        LOGGED_FUNCTION_END
    }

    uint32_t status_to_u32(future_status_t status) {
        return (future_status_word_t){ .as_bitfield = status }.as_word;
    }

    #define LOGGED_FUTURE_STATUS_TEST_START  \
        LOGGED_FUNCTION_START(  \
            "%p, { .dc = %u, .dco = %u, .a = %u, .s = %u, .o = %u, .t = %u, "  \
                  ".na = %u, .ne/ll = %u, %u }",  \
            future,  \
            status.downstream_count,  \
            status.downstream_count_overflow,  \
            status.available,  \
            status.state,  \
            status.outcome,  \
            status.type,  \
            status.notify_address,  \
            status.notify_event_or_lazy_lock,  \
            status.reserved  \
        )

    #define ensure_status_eq(x, y)  \
        ensure_eq(status_to_u32(x), status_to_u32(y))

    void check_future_status_init(udipe_future_t* future,
                                  future_status_t status) {
        LOGGED_FUTURE_STATUS_TEST_START
            future_status_initialize(future, status);
            ensure_status_eq(future_status_load(future, memory_order_relaxed),
                             status);
        LOGGED_FUNCTION_END
    }

    void check_future_status_write(udipe_future_t* future,
                                   future_status_t status) {
        LOGGED_FUTURE_STATUS_TEST_START
            future_status_store(future, status, memory_order_relaxed);
            ensure_status_eq(future_status_load(future, memory_order_relaxed),
                             status);
        LOGGED_FUNCTION_END
    }

    void check_future_status_cas_fail(udipe_future_t* future) {
        LOGGED_FUNCTION_START("%p", future)
            debug("Generating initial status...");
            const future_status_t initial_status =
                random_status(STATUS_KIND_ALLOCATED);
            future_status_store(future, initial_status, memory_order_relaxed);

            debug("Generating expected status...");
            future_status_t expected;
            do {
                expected = random_status(STATUS_KIND_ALLOCATED);
            } while (status_to_u32(expected) == status_to_u32(initial_status));

            debug("Generating desired status...");
            future_status_t desired;
            do {
                desired = random_status(STATUS_KIND_ALLOCATED);
            } while (status_to_u32(desired) == status_to_u32(initial_status)
                     || status_to_u32(desired) == status_to_u32(expected));

            debug(
                "Trying strong CAS(expected -> desired), which should fail..."
            );
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

            debug("Trying weak CAS(expected -> desired) which should fail...");
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
        LOGGED_FUNCTION_END
    }

    void check_future_status_cas_success(udipe_future_t* future) {
        LOGGED_FUNCTION_START("%p", future)
            debug("Generating initial status...");
            const future_status_t initial_status =
                random_status(STATUS_KIND_ALLOCATED);
            future_status_store(future, initial_status, memory_order_relaxed);

            debug("Generating first desired status...");
            future_status_t desired1;
            do {
                desired1 = random_status(STATUS_KIND_ALLOCATED);
            } while (status_to_u32(desired1) == status_to_u32(initial_status));

            debug("Generating second desired status...");
            future_status_t desired2;
            do {
                desired2 = random_status(STATUS_KIND_ALLOCATED);
            } while (status_to_u32(desired2) == status_to_u32(initial_status)
                     || status_to_u32(desired2) == status_to_u32(desired1));

            debug("Trying strong CAS(initial -> desired1), "
                  "which should succeed...");
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

            debug("Trying weak CAS(desired1->desired2) until it succeeds...");
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
        LOGGED_FUNCTION_END
    }

    void check_future_downstream_count_inc_dec(udipe_future_t* future) {
        LOGGED_FUNCTION_START("%p", future)
            for (int i = 0; i < 2; ++i) {
                const bool has_result = (bool)(i % 2);
                debugf("Generating initial status with has_result = %u...",
                       has_result);
                future_status_t initial_status;
                do {
                    initial_status = random_status(STATUS_KIND_AVAILABLE);
                } while (
                    initial_status.downstream_count == MAX_DOWNSTREAM_COUNT
                    || has_result != (initial_status.state == STATE_RESULT)
                );
                initial_status.downstream_count_overflow = false;
                future_status_store(future,
                                    initial_status,
                                    memory_order_relaxed);

                debug("Trying to increase the downstream count...");
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

                debug("Trying to decrease the downstream count...");
                future_downstream_count_dec(future, memory_order_release);
                ensure_status_eq(
                    future_status_load(future, memory_order_relaxed),
                    initial_status
                );
            }
        LOGGED_FUNCTION_END
    }

    void future_status_unit_tests() {
        LOGGED_FUNCTION_START_NO_PARAMS
            debug("Running future status ops unit tests...");
            configure_rand();

            for (size_t i = 0; i < NUM_TRIALS; ++i) {
                tracef("- Running test trial #%zu...", i);

                trace("  * Testing future initialization...");
                udipe_future_t future;
                check_future_status_init(&future, random_status(STATUS_KIND_UNALLOCATED));

                trace("  * Testing status word writes...");
                check_future_status_write(&future, random_status(STATUS_KIND_ANY));

                trace("  * Testing status word CAS failure...");
                check_future_status_cas_fail(&future);

                trace("  * Testing status word CAS success...");
                check_future_status_cas_success(&future);

                trace("  * Testing downstream count increment/decrement...");
                check_future_downstream_count_inc_dec(&future);
            }
        LOGGED_FUNCTION_END
    }

#endif  // UDIPE_BUILD_TESTS
