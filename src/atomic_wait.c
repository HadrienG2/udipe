#include "atomic_wait.h"

#include "error.h"
#include "log.h"
#include "thread_name.h"

#include <assert.h>
#include <errno.h>
#include <time.h>

#ifdef __linux__
    #include <linux/futex.h>
    #include <sys/syscall.h>
    #include <unistd.h>
#elif defined(_WIN32)
    #include <synchapi.h>
#endif


UDIPE_NON_NULL_ARGS
void udipe_atomic_wait(_Atomic uint32_t* atom, uint32_t expected) {
    tracef("Waiting for the value at address %p to change value from %#x...",
           (void*)atom,
           expected);
    #ifdef __linux__
        long result = syscall(SYS_futex, atom, FUTEX_WAIT, expected, NULL);
        switch (result) {
        case 0:
            trace("Got woken up after waiting (maybe from futex recycling).");
            break;
        case -1:
            switch (errno) {
            case EAGAIN:
                errno = 0;
                trace("Value already differed from expectation, didn't wait.");
                break;
            case EINTR:
                errno = 0;
                trace("Started to wait, but was interrupted by a signal.");
                break;
            // timeout did not point to a valid user-space address.
            case EFAULT:
            // The supplied timeout argument was invalid (tv_sec was less than
            // zero, or tv_nsec was not less than 1,000,000,000).
            case EINVAL:
            // The timeout expired before the operation completed.
            case ETIMEDOUT:
                exit_after_c_error("Shouldn't happen without a timeout!");
            default:
                exit_after_c_error("FUTEX_WAIT errno doesn't match manpage!");
            }
            break;
        default:
            exit_after_c_error("FUTEX_WAIT result doesn't match manpage!");
        }
    #elif defined(_WIN32)
        bool result = WaitOnAddress((volatile VOID*)atom,
                                    (PVOID)(&expected),
                                    4,
                                    INFINITE);
        win32_exit_on_zero(result, "No error expected as there is no timeout");
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
}

UDIPE_NON_NULL_ARGS
void udipe_atomic_notify_all(_Atomic uint32_t* atom) {
    tracef("Signaling all waiters that the value at address %p has changed...",
           (void*)atom);
    #ifdef __linux__
        long result = syscall(SYS_futex, atom, FUTEX_WAKE, (uint32_t)INT32_MAX);
        assert(result >= -1);
        exit_on_negative((int)result, "No error expected here");
        tracef("...which woke %u waiter(s).", (uint32_t)result);
    #elif defined(_WIN32)
        WakeByAddressAll((PVOID)atom);
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
}

UDIPE_NON_NULL_ARGS
void udipe_atomic_notify_one(_Atomic uint32_t* atom) {
    tracef("Signaling one waiter that the value at address %p has changed...",
           (void*)atom);
    #ifdef __linux__
        long result = syscall(SYS_futex, atom, FUTEX_WAKE, 1);
        assert(result >= -1 && result <= 1);
        exit_on_negative((int)result, "No error expected here");
        if (result == 1) {
            trace("...which woke a waiter.");
        } else {
            trace("...but no thread was waiting.");
        }
    #elif defined(_WIN32)
        WakeByAddressSingle((PVOID)atom);
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
}


#ifdef UDIPE_BUILD_TESTS

    /// Number of workers that we spawn (must be <= 9)
    #define NUM_WORKERS ((unsigned)2)

    /// How long we wait for a worker to do something before concluding that it
    /// is likely blocked and will not do anything.
    #define WAIT_FOR_IDLE ((struct timespec){ .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 })

    /// Number of waiting cycles that each worker goes through
    #define NUM_WAITS ((uint32_t)100)

    /// State shared between the worker threads and the main thread
    typedef struct shared_state_s {
        _Atomic uint32_t notify_counter;
        _Atomic uint32_t global_wake_counter;
        // FIXME: Save logger state to a struct to avoid this impl leakage
        logger_t* logger;
        udipe_log_level_t log_level;
    } shared_state_t;

    /// State that is private to each worker thread
    typedef struct worker_state_s {
        shared_state_t* shared;
        unsigned id;
        _Atomic uint32_t private_wake_counter;
    } worker_state_t;

    static int worker_func(void* context) {
        // Grab worker thread state and give it a clear name
        worker_state_t* state = (worker_state_t*)context;
        shared_state_t* shared = state->shared;
        char name[8] = "workerN";
        ensure_le(state->id, (unsigned)9);
        name[6] = '0' + (char)(state->id);
        // FIXME: Save logger state to a struct to avoid this impl leakage
        udipe_thread_logger = shared->logger;
        udipe_thread_log_level = shared->log_level;
        set_thread_name(name);

        trace("Entering wait/notify loop...");
        const size_t last = 0;
        const size_t current = 1;
        uint32_t notify[2] = { 0, 0 };
        uint32_t global_wake[2] = { 0, 0 };
        for (uint32_t wait = 1; wait <= NUM_WAITS; ++wait) {
            // Wait for the value of notify_counter to change
            tracef("Waiting for notify_counter to increase from last value %u...",
                   notify[last]);
            do {
                notify[current] = atomic_load_explicit(&shared->notify_counter,
                                                       memory_order_acquire);
                if (notify[current] != notify[last]) break;
                udipe_atomic_wait(&shared->notify_counter, notify[current]);
            } while(true);

            tracef("...done, notify_counter is now %u", notify[current]);
            ensure_gt(notify[current], notify[last]);
            notify[last] = notify[current];

            // Record that we are done waiting
            tracef("Telling the main thread that we completed wait cycle #%u...",
                   wait);
            atomic_store_explicit(&state->private_wake_counter,
                                  wait,
                                  memory_order_relaxed);

            // Increment the global wake count and ping the main thread
            trace("...and incrementing the global cycle count");
            global_wake[current] =
                atomic_fetch_add_explicit(&shared->global_wake_counter,
                                          1,
                                          memory_order_release) + 1;
            ensure_ge(global_wake[current], global_wake[last] + 1);
            ensure_le(global_wake[current], wait * NUM_WORKERS);
            udipe_atomic_notify_all(&shared->global_wake_counter);
        }

        trace("Done with our last wait cycle, exiting...");
        return 0;
    }

    static void test_wait_notify_all() {
        trace("Setting up shared state...");
        shared_state_t shared;
        atomic_init(&shared.notify_counter, 0);
        atomic_init(&shared.global_wake_counter, 0);
        // FIXME: Save logger state to a struct to avoid this impl leakage
        shared.logger = udipe_thread_logger;
        shared.log_level = udipe_thread_log_level;

        trace("Setting up worker threads");
        worker_state_t private[NUM_WORKERS];
        thrd_t handles[NUM_WORKERS];
        for (unsigned worker = 0; worker < NUM_WORKERS; ++worker) {
            private[worker].shared = &shared;
            private[worker].id = worker;
            atomic_init(&private[worker].private_wake_counter, 0);
            ensure_eq(thrd_create(&handles[worker],
                                  worker_func,
                                  (void*)(&private[worker])),
                      thrd_success);
        }

        trace("Entering notify/wait loop...");
        const size_t last = 0;
        const size_t current = 1;
        uint32_t notify[2] = { 0, 0 };
        uint32_t global_wake[2] = { 0, 0 };
        for (size_t wait = 1; wait <= NUM_WAITS; ++wait) {
            trace("Waiting for workers to finish what they're doing and start waiting...");
            thrd_sleep(&WAIT_FOR_IDLE, NULL);

            tracef("Sending a notification to all workers by increasing notify_counter to %u...",
                   notify[last] + 1);
            notify[current] = atomic_fetch_add_explicit(&shared.notify_counter,
                                                        1,
                                                        memory_order_release) + 1;
            ensure_eq(notify[current], notify[last] + 1);
            notify[last] = notify[current];
            udipe_atomic_notify_all(&shared.notify_counter);

            trace("Waiting for all workers to reply...");
            do {
                global_wake[current] = atomic_load_explicit(&shared.global_wake_counter,
                                                       memory_order_acquire);
                const unsigned awoken = global_wake[current] - global_wake[last];
                if (awoken == NUM_WORKERS) break;
                if (awoken > 0) {
                    tracef("Got a reply from %u/%u workers...", awoken, NUM_WORKERS);
                }
                udipe_atomic_wait(&shared.global_wake_counter,
                                  global_wake[current]);
            } while(true);
            global_wake[last] = global_wake[current];

            tracef("Got a reply from all %u workers!", NUM_WORKERS);
            for (unsigned worker = 0; worker < NUM_WORKERS; ++worker) {
                tracef("Checking if worker%u is in sync...", worker);
                ensure_eq(
                    atomic_load_explicit(&private[worker].private_wake_counter,
                                         memory_order_relaxed),
                    wait
                );
            }
        }

        trace("All done, waiting for workers to terminate...");
        for (unsigned worker = 0; worker < NUM_WORKERS; ++worker) {
            int res;
            ensure_eq(thrd_join(handles[worker], &res), thrd_success);
            ensure_eq(res, 0);
        }
    }

    void atomic_wait_unit_tests() {
        info("Running atomic wait unit tests...");
        with_log_level(UDIPE_DEBUG, {
            debug("Testing wait + notify_all");
            with_log_level(UDIPE_TRACE, {
                test_wait_notify_all();
            });

            debug("Testing wait + notify_one");
            with_log_level(UDIPE_TRACE, {
                // TODO: Test notify_one, sharing code with notify_all test
            });
        });
    }

#endif  // UDIPE_BUILD_TESTS
