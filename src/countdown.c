#ifdef UDIPE_BUILD_TESTS

    #include "countdown.h"

    #include "error.h"
    #include "log.h"

    #include <omp.h>
    #include <stdatomic.h>
    #include <stdlib.h>


    #define NUM_RUNS 100
    #define RANGE 100

    static void test_countdown(countdown_t* countdown, size_t initial) {
        countdown_set(countdown, initial);
        atomic_size_t num_last = 0;
        save_thread_logger_state(logger);
        #pragma omp parallel
        {
            load_thread_logger_state(logger);
            #pragma omp for
            for(size_t i = 0; i < initial; ++i) {
                bool last = countdown_dec_and_check(countdown);
                if (last) {
                    atomic_fetch_add_explicit(&num_last,
                                              1,
                                              memory_order_relaxed);
                }
            }
        }
        ensure_eq(atomic_load_explicit(&num_last, memory_order_relaxed),
                  (size_t)1);
    }

    void countdown_unit_tests() {
        info("Running countdown unit tests...");
        countdown_t countdown;
        with_log_level(UDIPE_DEBUG, {
            countdown_initialize(&countdown);
        });
        for (size_t i = 0; i < NUM_RUNS; ++i) {
            with_log_level(UDIPE_TRACE, {
                test_countdown(&countdown, (rand() % RANGE) + 2);
            });
        }
    }

#endif  // UDIPE_BUILD_TESTS