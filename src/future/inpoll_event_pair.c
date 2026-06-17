#ifdef __linux__

    #define _GNU_SOURCE

    #include "inpoll_event_pair.h"

    #include <udipe/nodiscard.h>

    #include "../error.h"
    #include "../event.h"
    #include "../inpoll.h"
    #include "../log.h"

    #include <errno.h>
    #include <stdint.h>


    UDIPE_NODISCARD
    inpoll_event_pair_t inpoll_event_pair_initialize() {
        debug("Setting up a coupled inpoll+eventfd pair...");

        debug("Allocating eventfd...");
        const event_t event = event_initialize(false);

        debug("Allocating inpoll...");
        inpoll_t inpoll = inpoll_initialize();

        debugf("Binding allocated eventfd %d to allocated inpoll %d...",
               event, inpoll);
        const inpoll_attach_result_t result =
            inpoll_attach(inpoll,
                          event,
                          // TODO: Expose this UINT64_MAX as a constant in
                          //       inpoll_event_pair.h and remove all mentions
                          //       of it elsewhere.
                          UINT64_MAX);
        switch (result) {
        case INPOLL_ATTACH_SUCCESS:
            break;
        case INPOLL_ATTACH_TOO_NESTED:  // Cannot happen, source is an eventfd
            exit_after_c_error("This error is not expected to happen!");
        }

        return (inpoll_event_pair_t){
            .inpoll = inpoll,
            .event = event
        };
    }

#endif  // __linux__
