#include "allocator.h"

#include "error.h"
#include "log.h"

#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>


/// Determine the smallest cache capacity available at a certain cache level
/// to a thread with a certain CPU binding.
///
/// \returns a fair share of the smallest capacity available at the specified
///          layer of the cache hierarchy, excluding the use of hyperthreading.
UDIPE_NON_NULL_ARGS
static size_t smallest_cache_capacity(hwloc_topology_t topology,
                                      hwloc_cpuset_t thread_cpuset,
                                      hwloc_obj_type_t cache_type) {
    assert(hwloc_obj_type_is_dcache(cache_type));

    debug("Computing minimal cache capacity within thread_cpuset...");
    size_t min_size = SIZE_MAX;
    unsigned os_cpu;
    hwloc_bitmap_foreach_begin(os_cpu, thread_cpuset)
        tracef("Finding the PU object associated with CPU %d...", os_cpu);
        hwloc_obj_t pu = hwloc_get_pu_obj_by_os_index(topology, os_cpu);
        exit_on_null(pu, "Failed to find PU from thread cpuset!");

        trace("Finding the cache capacity of this PU...");
        hwloc_obj_t cache = hwloc_get_ancestor_obj_by_type(topology, cache_type, pu);
        exit_on_null(cache, "Failed to find cache from thread PU!");
        assert(("Caches should have attributes", cache->attr));
        assert(cache->attr->cache.size < (uint64_t)SIZE_MAX);
        size_t cache_size = (size_t)cache->attr->cache.size;
        tracef("Requested cache can hold %zu bytes.", cache_size)

        trace("Determining cache cpuset...");
        assert(("Caches should have a cpuset", cache->cpuset));
        hwloc_cpuset_t cache_cpuset = hwloc_bitmap_dup(cache->cpuset);
        exit_on_null(cache_cpuset, "Failed to duplicate cache cpuset!");
        if (log_enabled(UDIPE_LOG_TRACE)) {
            char* cpuset_str;
            exit_on_negative(hwloc_bitmap_list_asprintf(&cpuset_str, cache_cpuset),
                             "Failed to display cache cpuset!");
            tracef("Cache is attached to CPU(s) %s.", cpuset_str);
            free(cpuset_str);
        }

        trace("Removing hyperthreads...");
        int result = hwloc_bitmap_singlify_per_core(topology, cache_cpuset, 0);
        assert(result == 0);
        if (log_enabled(UDIPE_LOG_TRACE)) {
            char* cpuset_str;
            exit_on_negative(hwloc_bitmap_list_asprintf(&cpuset_str, cache_cpuset),
                             "Failed to display cache cpuset!");
            tracef("That leaves CPU(s) %s.", cpuset_str);
            free(cpuset_str);
        }

        trace("Computing fair share of cache across attached CPU(s)...");
        int weight = hwloc_bitmap_weight(cache_cpuset);
        assert(weight >= 1);
        cache_size /= (size_t)weight;
        hwloc_bitmap_free(cache_cpuset);
        tracef("Each CPU can safely use %zu bytes from this cache.", cache_size);

        trace("Updating minimum cache capacity...");
        if (cache_size < min_size) min_size = cache_size;
    hwloc_bitmap_foreach_end();
    assert(("Thread cpuset should contain at least one PU", min_size < SIZE_MAX));

    debugf("Minimal cache capacity is %zu, "
           "will apply an 80%% safety factor on top of that...",
           min_size);
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
    if (page_size_l < 1) exit_after_c_error("Failed to query system page size!");
    size_t page_size = (size_t)page_size_l;
    debugf("System page size is %1$zu (%1$#zx) bytes.", page_size);

    hwloc_cpuset_t thread_cpuset = NULL;
    if ((config->buffer_size == 0) || (config->buffer_count == 0)) {
        debug("Allocating thread cpuset...");
        thread_cpuset = hwloc_bitmap_alloc();
        exit_on_null(thread_cpuset, "Failed to allocate thread cpuset!");

        debug("Querying thread CPU binding...");
        exit_on_negative(hwloc_get_cpubind(topology,
                                           thread_cpuset,
                                           HWLOC_CPUBIND_THREAD),
                         "Failed to query thread CPU binding!");

        if (log_enabled(UDIPE_LOG_DEBUG)) {
            char* cpuset_str;
            exit_on_negative(hwloc_bitmap_list_asprintf(&cpuset_str, thread_cpuset),
                             "Failed to display thread CPU binding!");
            debugf("Thread is bound to CPU(s) %s.", cpuset_str);
            free(cpuset_str);
        }
    }

    if (config->buffer_size == 0) {
        debug("Auto-tuning buffer size for L1 locality...");
        config->buffer_size = smallest_cache_capacity(topology,
                                                      thread_cpuset,
                                                      HWLOC_OBJ_L1CACHE);
        debugf("Optimal buffer size for L1 locality is %1$zu (%1$#zx) bytes.",
               config->buffer_size);
    }

    debug("Rounding up buffer size to a multiple of the page size...");
    size_t page_remainder = config->buffer_size % page_size;
    if (page_remainder != 0) {
        config->buffer_size += page_size - page_remainder;
    }
    debugf("Selected a buffer size of %1$zu (%1$#zx) bytes.",
           config->buffer_size);

    if (config->buffer_count == 0) {
        debug("Auto-tuning buffer count for L2 locality...");
        size_t pool_size = smallest_cache_capacity(topology,
                                                   thread_cpuset,
                                                   HWLOC_OBJ_L2CACHE);
        debugf("Optimal memory pool size for L2 locality is %1$zu (%1$#zx) bytes.",
               pool_size);
        config->buffer_count = pool_size / config->buffer_size;
        if ((pool_size % config->buffer_size) != 0) {
            config->buffer_count += 1;
        }
        if (config->buffer_count <= UDIPE_MAX_BUFFERS) {
            debugf("Will allocate a pool of %zu buffers.", config->buffer_count);
        } else {
            warningf("Auto-configuration suggests a pool of %zu buffers, but "
                     "implementation only supports %zu. UDIPE_MAX_BUFFERS "
                     "should be raised. Will stick with the maximum for now...",
                     config->buffer_count, UDIPE_MAX_BUFFERS);
            config->buffer_count = UDIPE_MAX_BUFFERS;
        }
    } else if (config->buffer_count > UDIPE_MAX_BUFFERS) {
        exit_with_error("Cannot have more than UDIPE_MAX_BUFFERS buffers!");
    }

    if (thread_cpuset) hwloc_bitmap_free(thread_cpuset);
}


UDIPE_NON_NULL_ARGS
allocator_t allocator_initialize(udipe_allocator_config_t global_config,
                                 hwloc_topology_t topology) {
    allocator_t allocator;
    if (global_config.callback) {
        debug("Obtaining configuration from user callback...");
        allocator.config = (global_config.callback)(global_config.context);
        debugf("User requested buffer_size %zu "
               "and buffer_count %zu (0 = default)",
               allocator.config.buffer_size,
               allocator.config.buffer_count);
    } else {
        debug("No user callback specified, will use default configuration.");
        if (global_config.context) {
            exit_with_error("Cannot set udipe_allocator_config_t::context "
                            "without also setting the callback field!");
        }
        memset(&allocator.config, 0, sizeof(udipe_thread_allocator_config_t));
    }

    debug("Applying defaults and page rounding...");
    finish_configuration(&allocator.config, topology);

    debug("Allocating the memory pool...");
    size_t pool_size = allocator.config.buffer_size * allocator.config.buffer_count;
    allocator.memory_pool = mmap(NULL,
                                 pool_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS,
                                 -1,
                                 0);
    exit_on_null(allocator.memory_pool, "Failed to allocate memory pool!");

    debug("Locking memory pages into RAM...");
    exit_on_negative(mlock(allocator.memory_pool, pool_size),
                     "Failed to lock memory pages into RAM!");

    debug("Initializing the availability bitmap...");
    bitmap_fill(allocator.buffer_availability, UDIPE_MAX_BUFFERS, true);
    return allocator;
}


void allocator_finalize(allocator_t allocator) {
    assert(bitmap_all(allocator.buffer_availability, UDIPE_MAX_BUFFERS, true));
    munmap(allocator.memory_pool, allocator.config.buffer_size * allocator.config.buffer_count);
}

// TODO: Implement remaining functions
