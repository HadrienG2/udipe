#include "context.h"

#include "error.h"
#include "log.h"

#include <errno.h>
#include <stdlib.h>


UDIPE_PUBLIC
UDIPE_NON_NULL_RESULT
udipe_context_t* udipe_initialize(udipe_config_t config) {
    // Set up logging
    logger_t logger = log_initialize(config.log);
    udipe_context_t* context = NULL;
    with_logger(&logger, {
        // Allocate context struct and put logger into it
        context = malloc(sizeof(udipe_context_t));
        if(!context) {
            warn_on_errno();
            error("Failed to allocate output udipe_context_t");
            exit(EXIT_FAILURE);
        }
        context->logger = logger;

        // TODO: Rest of the context setup
    });
    return context;
}


UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
void udipe_finalize(udipe_context_t* context) {
    with_logger(&context->logger, {
        // TODO: Rest of the context finalization, log new configuration
    });
    free(context);
}
