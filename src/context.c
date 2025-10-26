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
        debug("Allocating a libudipe context...");
        context = malloc(sizeof(udipe_context_t));
        exit_on_null(context, "Failed to allocate libudipe context!");
        memset(context, 0, sizeof(udipe_context_t));
        context->logger = logger;

        debug("Setting up the hwloc topology...");
        exit_on_negative(hwloc_topology_init(&context->topology),
                         "Failed to allocate the hwloc hopology!");
        exit_on_negative(hwloc_topology_load(context->topology),
                         "Failed to build the hwloc hopology!");

        debug("Initializing the connection options allocator...");
        const size_t num_connect_options =
            sizeof(context->connect_options) / sizeof(shared_connect_options_t);
        assert(num_connect_options <= 32);
        const uint32_t initial_availability =
            num_connect_options == 32 ? UINT32_MAX
                                      : ((uint32_t)1 << num_connect_options) - 1;
        atomic_init(&context->connect_options_availability, initial_availability);
        for (size_t i = 0; i < num_connect_options; ++i) {
            atomic_init(&context->connect_options[i].reference_count, 0);
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
