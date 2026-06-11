#include "context.h"

#include <udipe/nodiscard.h>
#include <udipe/visibility.h>

#include "error.h"
#include "future/allocator/context_cache.h"
#include "future/allocator/thread_cache.h"
#include "log.h"
#include "refcounted_tss.h"
#include "visibility.h"

#include <errno.h>
#include <hwloc.h>
#include <stdlib.h>
#include <string.h>


/// thread_future_cache_destructor() wrapper that has the `tss_dtor_t` signature
///
static void thread_future_cache_destructor(void* thread_cache) {
    future_thread_cache_t* cache = (future_thread_cache_t*)thread_cache;
    future_thread_cache_finalize_from_context(&cache);
}

DEFINE_PUBLIC
UDIPE_NODISCARD
UDIPE_NON_NULL_RESULT
udipe_context_t* udipe_initialize(udipe_config_t config) {
    logger_t logger = logger_initialize(config.log);
    udipe_context_t* context = NULL;
    with_logger(&logger, {
        debug("Allocating a libudipe context...");
        context = malloc(sizeof(udipe_context_t));
        memset(context, 0, sizeof(udipe_context_t));
        context->logger = logger;

        debug("Setting up the hwloc topology...");
        exit_on_negative(hwloc_topology_init(&context->topology),
                         "Failed to allocate the hwloc hopology!");
        exit_on_negative(hwloc_topology_load(context->topology),
                         "Failed to build the hwloc hopology!");

        debug("Initializing the connection options allocator...");
        context->connect_options = connect_options_allocator_initialize();

        debug("Initializing the context-global future allocator cache...");
        context->global_future_cache = future_context_cache_initialize();

        debug("Initializing the thread-local future allocator cache...");
        context->thread_future_cache = refcounted_tss_initialize(
            thread_future_cache_destructor
        );
    });
    return context;
}

DEFINE_PUBLIC
UDIPE_NON_NULL_ARGS
void udipe_finalize(udipe_context_t* context) {
    with_logger(&context->logger, {
        debug("Liberating all the future allocator caches...");
        future_context_cache_finalize(&context->global_future_cache);

        debug("Finalizing the connection options allocator...");
        connect_options_allocator_finalize(&context->connect_options);

        debug("Destroying and poisoning the hwloc topology...");
        hwloc_topology_destroy(context->topology);
        context->topology = NULL;

        debug("Destroying the logger...");
    });
    logger_finalize(&context->logger);
    // WARNING: No logging or logger-based functionality like ensure_xyz()
    //          allowed starting from this point.

    // Mark thread_future_cache as unreachable, and destroy the context if this
    // is the last reference to it (otherwise some TSS destructors must still
    // run and access the context in the process, so context destruction will be
    // deferred until the last of these destructors is done).
    if (refcounted_tss_discard(&context->thread_future_cache)) {
        free((void*)context);
    }
}
