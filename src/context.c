#include "context.h"

#include "log.h"

#include <errno.h>
#include <stdlib.h>


UDIPE_PUBLIC udipe_context_t* udipe_initialize(udipe_config_t config) {
    // Set up logging
    logger_t logger = log_initialize(config.log);
    udipe_context_t* context = NULL;
    with_logger(&logger, {
        // Allocate context struct and put logger in it
        context = malloc(sizeof(udipe_context_t));
        if(!context) {
            int prev_errno = errno;
            error("Failed to allocate udipe_context_t struct");
            errno = prev_errno;
            return NULL;
        }
        context->logger = logger;

        // TODO: Rest of the context setup
    });
    return context;
}


UDIPE_PUBLIC void udipe_finalize(udipe_context_t* context) {
    with_logger(&context->logger, {
        // TODO: Rest of the context finalization, log new configuration

        free(context);
    });
}
