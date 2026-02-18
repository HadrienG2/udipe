#ifdef UDIPE_BUILD_BENCHMARKS

    #include "distribution_pool.h"

    #include "distribution.h"

    #include "../error.h"
    #include "../memory.h"
    #include "../log.h"

    #include <stddef.h>
    #include <stdlib.h>


    distribution_pool_t distribution_pool_initialize() {
        const size_t capacity = get_page_size() / sizeof(distribution_builder_t);
        ensure_ne(capacity, (size_t)0);
        void* const allocation = malloc(capacity * sizeof(distribution_builder_t));
        exit_on_null(allocation, "Failed to allocate distribution pool");
        debugf("Allocated distribution pool with %zu entries at location %p.",
               capacity, allocation);
        return (distribution_pool_t){
            .builders = (distribution_builder_t*)allocation,
            .capacity = capacity,
            .length = 0
        };
    }

    UDIPE_NON_NULL_ARGS
    distribution_builder_t distribution_pool_request(distribution_pool_t* pool) {
        assert(pool->builders);
        if (pool->length >= 1) {
            --(pool->length);
            distribution_builder_t builder = pool->builders[pool->length];
            debugf("Successfully reused previously recycled distribution @ %p.",
                   builder.inner.allocation);
            return builder;
        } else {
            debug("No recycled distribution available, allocating a new one...");
            return distribution_initialize();
        }
    }

    UDIPE_NON_NULL_ARGS
    void distribution_pool_recycle(distribution_pool_t* pool,
                                   distribution_t* dist) {
        debugf("Recycling distribution @ %p...", dist->allocation);
        assert(pool->builders);
        assert(pool->capacity > 0);
        assert(pool->length <= pool->capacity);
        if (pool->length == pool->capacity) {
            trace("Not enough pool capacity, reallocating...");
            void* old_builders = pool->builders;
            const size_t new_capacity = 2 * pool->capacity;
            const size_t new_size =
                new_capacity * sizeof(distribution_builder_t);
            pool->builders = realloc(pool->builders, new_size);
            exit_on_null(pool->builders, "Failed to grow distribution pool");
            pool->capacity = new_capacity;
            if (pool->builders == old_builders) {
                debugf("Grew distribution pool @ %p to %zu entries (%zu bytes).",
                       pool->builders, new_capacity, new_size);
            } else {
                debugf("Reallocated distribution pool to new location %p "
                       "with %zu entries (%zu bytes).",
                       pool->builders,
                       new_capacity, new_size);
            }
        }
        assert(pool->length < pool->capacity);
        pool->builders[pool->length] = distribution_reset(dist);
        ++(pool->length);
    }

    UDIPE_NON_NULL_ARGS
    void distribution_pool_finalize(distribution_pool_t* pool) {
        debug("Liberating inner builders...");
        for (size_t i = 0; i < pool->length; ++i) {
            distribution_discard(&pool->builders[i]);
        }

        debugf("Liberating distribution pool storage at location %p...",
               pool->builders);
        ensure(pool->builders);
        free(pool->builders);
        *pool = (distribution_pool_t){
            .builders = NULL,
            .capacity = 0,
            .length = 0
        };
    }

#endif  // UDIPE_BUILD_BENCHMARKS