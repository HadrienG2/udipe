#ifdef __linux__

    #define _GNU_SOURCE

    #include "latched_inpoll.h"

    #include <udipe/nodiscard.h>

    #include "../error.h"
    #include "../event.h"
    #include "../inpoll.h"
    #include "../log.h"

    #include <stdint.h>


    UDIPE_NODISCARD
    inpoll_with_latch_t latched_inpoll_initialize() {
        LOGGED_FUNCTION_START_NO_PARAMS
            debug("Allocating latch eventfd...");
            const event_t latch = event_initialize(false);

            debug("Allocating inpoll...");
            inpoll_t inpoll = inpoll_initialize();

            debugf("Binding latch eventfd %d to inpoll %d with id %zx...",
                   latch, inpoll, INPOLL_LATCH_ID);
            const inpoll_attach_result_t result =
                inpoll_attach(inpoll,
                              latch,
                              INPOLL_LATCH_ID);
            switch (result) {
            case INPOLL_ATTACH_SUCCESS:
                break;
            case INPOLL_ATTACH_TOO_NESTED:  // Cannot happen with an eventfd
            case INPOLL_ATTACH_REDUNDANT:  // Cannot happen for the first fd
                exit_after_c_error("This error is not expected to happen!");
            }

            return (inpoll_with_latch_t){
                .inpoll = inpoll,
                .latch = latch
            };
        LOGGED_FUNCTION_END
    }

#endif  // __linux__
