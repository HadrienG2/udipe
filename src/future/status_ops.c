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
    assert(is_allocated || !status.available);

    assert(("65k dependents ought to be enough for anybody",
            !status.downstream_count_overflow));
    assert(("Futures can only be used between allocation and start of udipe_finish()",
            (status.downstream_count == 0) || status.available));
    assert(("Should be true unless MAX_DOWNSTREAM_COUNT define needs updating",
            status.downstream_count <= MAX_DOWNSTREAM_COUNT));

    bool has_dependencies;
    bool has_processing;
    bool has_dedicated_thread;
    bool is_lazy;
    bool requires_locking;
    switch (status.type) {
    case TYPE_INVALID:
        assert(!is_allocated);
        has_dependencies = false;
        has_processing = false;
        has_dedicated_thread = false;
        is_lazy = false;
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
        is_lazy = false;
        requires_locking = false;
        break;
    case TYPE_CUSTOM:
        assert(is_allocated);
        has_dependencies = false;
        has_processing = true;
        has_dedicated_thread = true;
        is_lazy = false;
        requires_locking = false;
        break;
    case TYPE_JOIN:
    case TYPE_UNORDERED:
        assert(is_allocated);
        has_dependencies = true;
        has_processing = false;
        has_dedicated_thread = false;
        is_lazy = true;
        requires_locking = true;
        break;
    case TYPE_TIMER_ONCE:
        assert(is_allocated);
        has_dependencies = false;
        has_processing = true;
        has_dedicated_thread = false;
        is_lazy = true;
        requires_locking = false;
        break;
    case TYPE_TIMER_REPEAT:
        assert(is_allocated);
        has_dependencies = false;
        has_processing = true;
        has_dedicated_thread = false;
        is_lazy = true;
        requires_locking = true;
        break;
    case NUM_TYPES:
    default:
        assert(("Never valid", false));
        has_dependencies = false;
        has_processing = false;
        has_dedicated_thread = false;
        is_lazy = false;
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
        // Can be locked in the RESULT state if a lazy future gets canceled
        // while a thread acquired the lock to call inpoll_wait().
        could_be_locked = requires_locking;
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
        if (is_lazy) {
            // notify_address can be switched on from the fist state onwards,
            // and remains on after being turned on. Or it may never be switched
            // on. So we can't tell anything about its value. On the other hand,
            // we know of some cases where lazy_lock cannot be set.
            if (status.notify_event_or_lazy_lock) assert(could_be_locked);
        } else {
            // notify_address and notify_event can be switched on from the
            // WAITING state, and remain on after being turned on. They may also
            // never be switched on. So we can't tell anything about their
            // values.
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
                result.downstream_count = (rand() % (MAX_DOWNSTREAM_COUNT - 1)) + 1;
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
            result.type = (rand() % (NUM_TYPES - 1)) + 1;
            break;
        case STATUS_KIND_ANY:
        case STATUS_KIND_ALLOCATED:
        default:
            exit_with_error("Excluded above");
        }
        trace_expr((size_t)result.type);

        result.available = (kind == STATUS_KIND_AVAILABLE);
        trace_expr((bool)result.available);

        int wait_style;  // 0 = unallocated, 1 = eager, 2 = lazy
        bool has_dependencies;
        bool has_processing;
        bool could_be_locked;
        switch (result.type) {
        case TYPE_INVALID:
            result.state = STATE_UNINITIALIZED;
            wait_style = 0;
            has_dependencies = false;
            has_processing = false;
            could_be_locked = false;
            break;
        case TYPE_NETWORK_CONNECT:
        case TYPE_NETWORK_DISCONNECT:
        case TYPE_NETWORK_SEND:
        case TYPE_NETWORK_RECV:
            // Network futures can be in all possible states: WAITING,
            // PROCESSING, CANCELING and RESULT
            result.state = (rand() % (NUM_STATES - 1)) + 1;
            wait_style = 1;
            has_dependencies = true;
            has_processing = true;
            could_be_locked = false;
            break;
        case TYPE_CUSTOM:
            // Custom futures can be in states PROCESSING, CANCELING and RESULT,
            // but not in the WAITING state as they have no dependencies.
            result.state = (rand() % (NUM_STATES - 2)) + 2;
            wait_style = 1;
            has_dependencies = false;
            has_processing = true;
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
            wait_style = 2;
            has_dependencies = true;
            has_processing = false;
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
            wait_style = 2;
            has_dependencies = false;
            has_processing = false;
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
            if (has_dependencies) outcomes[outcomes_len++] = OUTCOME_FAILURE_DEPENDENCY;
            if (has_processing) outcomes[outcomes_len++] = OUTCOME_FAILURE_INTERNAL;
            result.outcome = outcomes[rand() % outcomes_len];
            break;
        case NUM_STATES:
        default:
            exit_with_error("Should never happen");
        }
        trace_expr((size_t)result.outcome);

        switch (wait_style) {
        case 0:  // Unallocated
            result.notify_address = false;
            result.notify_event_or_lazy_lock = false;
            break;
        case 1:  // Eager
            result.notify_address = rand() % 2;
            result.notify_event_or_lazy_lock = rand() % 2;
            break;
        case 2:  // Lazy
            result.notify_address = rand() % 2;
            if (could_be_locked) {
                result.notify_event_or_lazy_lock = rand() % 2;
            } else {
                result.notify_event_or_lazy_lock = false;
            }
            break;
        default:
            exit_with_error("Should never happen");
        }
        trace_expr((size_t)result.notify_address);
        trace_expr((bool)result.notify_event_or_lazy_lock);

        result.reserved = 0;
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
        trace("Generating initial status...");
        const future_status_t initial_status =
            random_status(STATUS_KIND_ALLOCATED);
        future_status_store(future, initial_status, memory_order_relaxed);
        trace("Generating expected status...");
        future_status_t expected;
        do {
            expected = random_status(STATUS_KIND_ALLOCATED);
        } while (status_to_u32(expected) == status_to_u32(initial_status));
        trace("Generating desired status...");
        future_status_t desired;
        do {
            desired = random_status(STATUS_KIND_ALLOCATED);
        } while (status_to_u32(desired) == status_to_u32(initial_status)
                 || status_to_u32(desired) == status_to_u32(expected));

        trace("Trying strong CAS(expected -> desired) which should fail...");
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

        trace("Trying weak CAS(expected -> desired) which should fail...");
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
        trace("Generating initial status...");
        const future_status_t initial_status =
            random_status(STATUS_KIND_ALLOCATED);
        future_status_store(future, initial_status, memory_order_relaxed);
        trace("Generating first desired status...");
        future_status_t desired1;
        do {
            desired1 = random_status(STATUS_KIND_ALLOCATED);
        } while (status_to_u32(desired1) == status_to_u32(initial_status));
        trace("Generating second desired status...");
        future_status_t desired2;
        do {
            desired2 = random_status(STATUS_KIND_ALLOCATED);
        } while (status_to_u32(desired2) == status_to_u32(initial_status)
                 || status_to_u32(desired2) == status_to_u32(desired1));

        trace("Trying strong CAS(initial -> desired1) which should succeed...");
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

        trace("Trying weak CAS(desired1->desired2) until it succeeds...");
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
            tracef("Generating initial status with has_result = %u...",
                   has_result);
            future_status_t initial_status;
            do {
                initial_status = random_status(STATUS_KIND_AVAILABLE);
            } while (initial_status.downstream_count == MAX_DOWNSTREAM_COUNT
                     || has_result != (initial_status.state == STATE_RESULT));
            initial_status.downstream_count_overflow = false;
            future_status_store(future, initial_status, memory_order_relaxed);

            trace("Trying to increase the downstream count...");
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

            trace("Trying to decrease the downstream count...");
            future_downstream_count_dec(future, memory_order_release);
            ensure_status_eq(
                future_status_load(future, memory_order_relaxed),
                initial_status
            );
        }
    }

    void future_status_unit_tests() {
        debug("Running future status ops unit tests...");
        configure_rand();

        for (size_t i = 0; i < NUM_TRIALS; ++i) {
            trace("Testing future initialization...");
            udipe_future_t future;
            check_future_status_init(&future, random_status(STATUS_KIND_UNALLOCATED));

            trace("Testing status word writes...");
            check_future_status_write(&future, random_status(STATUS_KIND_ANY));

            trace("Testing status word CAS failure...");
            check_future_status_cas_fail(&future);

            trace("Testing status word CAS success...");
            check_future_status_cas_success(&future);

            trace("Testing downstream count increment/decrement...");
            check_future_downstream_count_inc_dec(&future);
        }
    }

#endif  // UDIPE_BUILD_TESTS
