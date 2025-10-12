#include "context.h"

#include "log.h"

#include <errno.h>
#include <stdlib.h>


UDIPE_PUBLIC udipe_context_t* udipe_initialize(udipe_config_t config) {
    // Set up logging
    logger_t logger = log_initialize(config.log);
    udipe_context_t* context = NULL;
    with_logger(&logger, {
        info("Logger initialized, initializing the rest of the context...");

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

        info("Context is initialized and ready to accept user commands");
    });
    return context;
}


UDIPE_PUBLIC void udipe_finalize(udipe_context_t* context) {
    with_logger(&context->logger, {
        info("Beginning context finalization...");

        // TODO: Rest of the context setup

        info("Context is finalized and ready to be destroyed");
        free(context);
    });
}
