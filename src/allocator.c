#include "allocator.h"

#include "error.h"

#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>


/// Default allocator configuration callback
///
/// It simply configures all parameters automatically.
static udipe_thread_allocator_config_t default_config_callback(void* /* context */) {
    udipe_thread_allocator_config_t config;
    memset(&config, 0, sizeof(udipe_thread_allocator_config_t));
    return config;
}


/// Determine the smallest cache capacity available at a certain cache level
/// to a thread with a certain CPU binding.
///
/// \returns a fair share of the smallest capacity available at the specified
///          layer of the cache hierarchy, excluding the use of hyperthreading.
UDIPE_NON_NULL_ARGS
size_t smallest_cache_capacity(hwloc_topology_t topology,
                               hwloc_cpuset_t thread_cpuset,
                               hwloc_obj_type_t cache_type) {
    assert(hwloc_obj_type_is_dcache(cache_type));

    debug("Computing minimal cache capacity within thread_cpuset...");
    size_t min_size = SIZE_MAX;
    unsigned os_cpu;
    hwloc_bitmap_foreach_begin(os_cpu, thread_cpuset)
        trace("Finding a PU from the thread's cpuset...");
        hwloc_obj_t pu = hwloc_get_pu_obj_by_os_index(topology, os_cpu);
        exit_on_null(pu, "Failed to find PU from thread cpuset");

        trace("Finding the cache capacity associated with this PU...");
        hwloc_obj_t cache = hwloc_get_ancestor_obj_by_type(topology, cache_type, pu);
        exit_on_null(cache, "Failed to find cache from thread PU");
        assert(("Caches should have attributes", cache->attr));
        assert(cache->attr->cache.size < (uint64_t)SIZE_MAX);
        size_t cache_size = (size_t)cache->attr->cache.size;

        trace("Assuming fair cache sharing w/o hyperthreading...");
        assert(("Caches should have a cpuset", cache->cpuset));
        hwloc_cpuset_t cache_cpuset = hwloc_bitmap_dup(cache->cpuset);
        exit_on_null(cache_cpuset, "Failed to duplicate cache cpuset");
        int result = hwloc_bitmap_singlify_per_core(topology, cache_cpuset, 0);
        assert(result == 0);
        int weight = hwloc_bitmap_weight(cache_cpuset);
        assert(weight >= 1);
        cache_size /= (size_t)weight;
        hwloc_bitmap_free(cache_cpuset);

        trace("Updating minimum cache capacity...");
        if (cache_size < min_size) min_size = cache_size;
    hwloc_bitmap_foreach_end();
    assert(("Thread cpuset should contain at least one PU", min_size < SIZE_MAX));

    debug("Applying a security margin to the returned cache capacity...");
    return (8 * min_size) / 10;
}


/// Apply defaults and page rounding to a \ref udipe_thread_allocator_config_t
///
/// This prepares the config struct for use within the actual allocator by
/// replacing placeholder zeroes with actual default values and rounding up the
/// buffer size to the next multiple of the system page size.
UDIPE_NON_NULL_ARGS
static void finish_configuration(udipe_thread_allocator_config_t* config,
                                 hwloc_topology_t topology) {
    debug("Querying system page size...");
    long page_size_l = sysconf(_SC_PAGE_SIZE);
    if (page_size_l < 1) exit_after_c_error("Failed to query system page size");
    size_t page_size = (size_t)page_size_l;

    hwloc_cpuset_t thread_cpuset = NULL;
    if ((config->buffer_size == 0) || (config->buffer_count == 0)) {
        debug("Allocating thread cpuset...");
        thread_cpuset = hwloc_bitmap_alloc();
        exit_on_null(thread_cpuset, "Failed to allocate thread cpuset");

        debug("Querying thread CPU binding...");
        exit_on_negative(hwloc_get_cpubind(topology,
                                           thread_cpuset,
                                           HWLOC_CPUBIND_THREAD),
                         "Failed to query thread CPU binding");
    }

    if (config->buffer_size == 0) {
        debug("Auto-tuning buffer size for L1 locality...");
        config->buffer_size = smallest_cache_capacity(topology,
                                                      thread_cpuset,
                                                      HWLOC_OBJ_L1CACHE);
    }

    debug("Rounding up buffer size to a page size multiple...");
    size_t page_remainder = config->buffer_size % page_size;
    if (page_remainder != 0) {
        config->buffer_size += page_size - page_remainder;
    }

    if (config->buffer_count == 0) {
        debug("Auto-tuning buffer count for L2 locality...");
        size_t pool_size = smallest_cache_capacity(topology,
                                                   thread_cpuset,
                                                   HWLOC_OBJ_L2CACHE);
        config->buffer_count = pool_size / config->buffer_size;
        if ((pool_size % config->buffer_size) != 0) {
            config->buffer_count += 1;
        }
    }

    if (thread_cpuset) {
        debug("Done with thread cpuset, liberating it...");
        hwloc_bitmap_free(thread_cpuset);
    }
}


UDIPE_NON_NULL_ARGS
allocator_t allocator_initialize(udipe_allocator_config_t global_config,
                                 hwloc_topology_t topology) {
    debug("Obtaining configuration from callback...");
    if (!global_config.callback) {
        global_config.callback = default_config_callback;
        if (global_config.context) exit_with_error("Cannot set context without setting callback");
    }
    allocator_t allocator;
    allocator.config = (global_config.callback)(global_config.context);

    debug("Applying defaults and page rounding...");
    finish_configuration(&allocator.config, topology);

    // TODO: Extract this pattern into a more generic macro
    if (log_enabled(UDIPE_LOG_INFO)) {
        #define call_snprintf(buf, size)  \
            snprintf(buf, size,  \
                     "Configured memory allocator with %zu buffers of %zu (0x%zx) bytes",  \
                     allocator.config.buffer_count, allocator.config.buffer_size, allocator.config.buffer_size)
        int result = call_snprintf(NULL, 0);
        exit_on_negative(result, "Failed to evaluate size requirements for log");
        size_t buf_size = 1 + (size_t)result;
        char* buf = alloca(buf_size);
        exit_on_null(buf, "Failed to allocate log buffer");
        result = call_snprintf(buf, buf_size);
        exit_on_negative(result, "Failed to compute log string");
        info(buf);
        #undef call_snprintf
    }

    debug("Allocating the memory pool...");
    size_t pool_size = allocator.config.buffer_size * allocator.config.buffer_count;
    allocator.memory_pool = mmap(NULL,
                                 pool_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS,
                                 -1,
                                 0);
    exit_on_null(allocator.memory_pool, "Failed to allocate memory pool");

    debug("Locking memory pages into RAM...");
    exit_on_negative(mlock(allocator.memory_pool, pool_size),
                     "Failed to lock memory pages into RAM");

    debug("Initializing the availability bitmap");
    for (size_t buf = 0; buf < allocator.config.buffer_count; ++buf) {
        allocator.buffer_availability[buf / UDIPE_BUFFERS_PER_USAGE_WORD]
            |= (1 << (buf % UDIPE_BUFFERS_PER_USAGE_WORD));
    }
    return allocator;
}

// TODO: Implement remaining functions
