#include "context.h"

#include <udipe/visibility.h>

#include "error.h"
#include "log.h"
#include "memory.h"
#include "visibility.h"

#include <errno.h>
#include <hwloc.h>
#include <stdlib.h>


DEFINE_PUBLIC
UDIPE_NON_NULL_RESULT
udipe_context_t* udipe_initialize(udipe_config_t config) {
    logger_t logger = logger_initialize(config.log);
    udipe_context_t* context = NULL;
    with_logger(&logger, {
        debug("Allocating a libudipe context...");
        context = realtime_allocate(sizeof(udipe_context_t));
        memset(context, 0, sizeof(udipe_context_t));
        context->logger = logger;

        debug("Setting up the hwloc topology...");
        exit_on_negative(hwloc_topology_init(&context->topology),
                         "Failed to allocate the hwloc hopology!");
        exit_on_negative(hwloc_topology_load(context->topology),
                         "Failed to build the hwloc hopology!");

        debug("Initializing the connection options allocator...");
        context->connect_options = connect_options_allocator_initialize();
    });
    return context;
}

DEFINE_PUBLIC
UDIPE_NON_NULL_ARGS
void udipe_finalize(udipe_context_t* context) {
    logger_t logger = context->logger;
    with_logger(&logger, {
        debug("Finalizing the connection options allocator...");
        connect_options_allocator_finalize(&context->connect_options);

        debug("Destroying and poisoning the hwloc topology...");
        hwloc_topology_destroy(context->topology);
        context->topology = NULL;

        debug("Freeing the udipe_context_t...");
        realtime_liberate(context, sizeof(udipe_context_t));
    });
    logger_finalize(&logger);
}
