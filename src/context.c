#include "context.h"

#include <udipe/visibility.h>

#include "error.h"
#include "log.h"

#include <errno.h>
#include <stdlib.h>


UDIPE_PUBLIC
UDIPE_NON_NULL_RESULT
udipe_context_t* udipe_initialize(udipe_config_t config) {
    logger_t logger = log_initialize(config.log);
    udipe_context_t* context = NULL;
    with_logger(&logger, {
        debug("Allocating the udipe_context_t...");
        context = malloc(sizeof(udipe_context_t));
        if (!context) {
            warn_on_errno();
            error("Failed to allocate the libudipe context");
            exit(EXIT_FAILURE);
        }
        context->logger = logger;

        debug("Setting up the hwloc topology...");
        if (hwloc_topology_init(&context->topology) < 0) {
            warn_on_errno();
            error("Failed to allocate the hwloc hopology");
            exit(EXIT_FAILURE);
        }
        if (hwloc_topology_load(context->topology) < 0) {
            warn_on_errno();
            error("Failed to build the hwloc hopology");
            hwloc_topology_destroy(context->topology);
            exit(EXIT_FAILURE);
        }
    });
    return context;
}


UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
void udipe_finalize(udipe_context_t* context) {
    with_logger(&context->logger, {
        debug("Destroying the hwloc topology...");
        hwloc_topology_destroy(context->topology);

        debug("Freeing the udipe_context_t...");
    });
    free(context);
}
