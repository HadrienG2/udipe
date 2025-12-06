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
    tracef("Waiting for the value at address %p to change away from %#x...",
           (void*)atom,
           expected);
    #ifdef __linux__
        long result = syscall(SYS_futex, atom, FUTEX_WAIT, expected, NULL);
        switch (result) {
        case 0:
            trace("...and got notified (may be spurious in real use cases).");
            break;
        case -1:
            switch (errno) {
            case EAGAIN:
                errno = 0;
                trace("...but the value changed before we even started.");
                break;
            case EINTR:
                errno = 0;
                trace("...but our wait was interrupted by a signal.");
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

    /// Number of workers that we spawn
    ///
    /// See asserts below for the range that this value can take.
    #define NUM_WORKERS ((unsigned)2)
    static_assert(
        NUM_WORKERS >= 2,
        "Need at least 2 workers to compare notify_all and notify_one"
    );
    static_assert(
        NUM_WORKERS < 10,
        "Current implementation doesn't support more than 9 workers"
    );

    /// Duration of worker wait for idle
    ///
    /// How long the main thread waits for workers to do something before
    /// concluding that they are likely all sleeping.
    ///
    /// This should be set as short as possible to keep the test fast, but long
    /// enough that workers do have the time to fall asleep sometimes.
    #define WAIT_FOR_IDLE ((struct timespec){ .tv_sec = 0, .tv_nsec = 200 * 1000 })

    /// Number of waiting cycles that each worker goes through
    ///
    /// Setting this higher makes the test more thorough and more likely to
    /// catch bugs, at the expense of increasing test running time
    #define NUM_WAIT_CYCLES ((uint32_t)100)

    /// State shared between the worker threads and the main thread
    ///
    /// This is used to propagate main thread state to the workers and to let
    /// workers and the main thread synchronize with each other.
    typedef struct shared_state_s {
        /// Main thread logger state backup
        ///
        /// Used to sync up the logging configuration of worker threads with
        /// that of the main thread.
        logger_state_t logger;

        /// Main thread notification counter
        ///
        /// The main thread begins a notify/wait cycle by increasing this value.
        _Atomic uint32_t notify_counter;

        /// Worker notification channel
        ///
        /// The main thread waits for at least one worker to respond to the
        /// notification by incrementing this counter before moving on.
        _Atomic uint32_t global_wake_counter;

        /// Truth that udipe_wait_notify_all() is being used
        ///
        /// If this is false, udipe_wait_notify_one() is being used. As
        /// `notify_one` is specified such that it can be implemented via
        /// `notify_all`, this change of notification function does not change
        /// the basic synchronization logic, but it does reduces the number of
        /// guaranteed properties that the test can check for.
        bool notify_all;
    } shared_state_t;

    /// State that is handed over to each worker thread
    typedef struct worker_state_s {
        /// Access to the shared state
        ///
        /// This is the main channel through which the main thread and worker
        /// thread communicate with each other.
        shared_state_t* shared;

        /// Worker wait cycle tracking
        ///
        /// Each worker keeps track of which wait cycle it is currently
        /// executing, which is useful when debugging test deadlocks.
        _Atomic uint32_t private_wake_counter;

        /// Worker identifier
        ///
        /// Each worker gets an integer identifier, which for implementation
        /// convenience is forced to be between 0 and 9.
        uint8_t id;
    } worker_state_t;

    static int worker_func(void* context) {
        // Grab worker thread state and give it a clear name
        worker_state_t* state = (worker_state_t*)context;
        shared_state_t* shared = state->shared;
        logger_restore(&shared->logger);
        tracef("Setting up worker%u...", state->id);
        char name[8] = "workerN";
        ensure_le(state->id, (uint8_t)9);
        name[6] = '0' + (char)(state->id);
        set_thread_name(name);

        trace("Entering wait/notify loop...");
        const size_t last = 0;
        const size_t current = 1;
        uint32_t notify[2] = { 0, 0 };
        uint32_t global_wake[2] = { 0, 0 };
        for (uint32_t wait = 1; wait <= NUM_WAIT_CYCLES; ++wait) {
            // Wait for the value of notify_counter to change
            tracef("Waiting for notify_counter to increase from %u...",
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
            tracef("Recording that we completed wait cycle %u...",
                   wait);
            const uint32_t old_private =
                atomic_exchange_explicit(&state->private_wake_counter,
                                         wait,
                                         memory_order_relaxed);
            ensure_eq(old_private, wait - 1);

            // Increment the global wake count and ping the main thread
            trace("...then pinging the main thread via global_wake.");
            global_wake[current] =
                1 + atomic_fetch_add_explicit(&shared->global_wake_counter,
                                              1,
                                              memory_order_release);
            ensure_ge(global_wake[current], global_wake[last] + 1);
            if (shared->notify_all) {
                ensure_le(global_wake[current], wait * NUM_WORKERS);
            }
            if (shared->notify_all) {
                udipe_atomic_notify_all(&shared->global_wake_counter);
            } else {
                udipe_atomic_notify_one(&shared->global_wake_counter);
            }
        }

        trace("Done with our last wait cycle, exiting...");
        return 0;
    }

    static void test_wait_notify(bool notify_all) {
        trace("Setting up the shared state...");
        shared_state_t shared;
        shared.logger = logger_backup();
        atomic_init(&shared.notify_counter, 0);
        atomic_init(&shared.global_wake_counter, 0);
        shared.notify_all = notify_all;

        trace("Setting up worker threads...");
        worker_state_t private[NUM_WORKERS];
        thrd_t handles[NUM_WORKERS];
        for (uint8_t worker = 0; worker < NUM_WORKERS; ++worker) {
            private[worker].shared = &shared;
            atomic_init(&private[worker].private_wake_counter, 0);
            private[worker].id = worker;
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
        while (global_wake[last] < NUM_WORKERS * NUM_WAIT_CYCLES) {
            trace("Giving workers time to start waiting...");
            thrd_sleep(&WAIT_FOR_IDLE, NULL);

            tracef("Waking workers by increasing notify_counter to %u...",
                   notify[last] + 1);
            notify[current] =
                1 + atomic_fetch_add_explicit(&shared.notify_counter,
                                              1,
                                              memory_order_release);
            ensure_eq(notify[current], notify[last] + 1);
            notify[last] = notify[current];
            if (notify_all) {
                udipe_atomic_notify_all(&shared.notify_counter);
            } else {
                udipe_atomic_notify_one(&shared.notify_counter);
            }

            trace("Waiting for workers to reply...");
            unsigned awoken;
            do {
                global_wake[current] =
                    atomic_load_explicit(&shared.global_wake_counter,
                                         memory_order_acquire);
                awoken = global_wake[current] - global_wake[last];
                if (awoken == 0) {
                    udipe_atomic_wait(&shared.global_wake_counter,
                                      global_wake[current]);
                    continue;
                } else if (awoken == NUM_WORKERS || !notify_all) {
                    break;
                }
                tracef("Got a reply from %u/%u workers, but we expect more...",
                       awoken, NUM_WORKERS);
            } while(true);
            global_wake[last] = global_wake[current];

            tracef("Got a reply from expected %u/%u workers!",
                   awoken, NUM_WORKERS);
            if (!notify_all) {
                trace("When notify_one is used, this is all we can check.");
                continue;
            }

            for (unsigned worker = 0; worker < NUM_WORKERS; ++worker) {
                tracef("Checking if worker%u is in sync...", worker);
                ensure_eq(
                    atomic_load_explicit(&private[worker].private_wake_counter,
                                         memory_order_relaxed),
                    global_wake[last] / NUM_WORKERS
                );
            }
            trace("All workers in sync, proceeding to next wait cycle.");
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
                test_wait_notify(true);
            });

            debug("Testing wait + notify_one");
            with_log_level(UDIPE_TRACE, {
                test_wait_notify(false);
            });
        });
    }

#endif  // UDIPE_BUILD_TESTS
