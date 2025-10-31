#include "buffer.h"

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
        const hwloc_obj_t pu = hwloc_get_pu_obj_by_os_index(topology, os_cpu);
        exit_on_null(pu, "Failed to find PU from thread cpuset!");

        trace("Finding the cache capacity of this PU...");
        const hwloc_obj_t cache = hwloc_get_ancestor_obj_by_type(topology,
                                                                 cache_type,
                                                                 pu);
        exit_on_null(cache, "Failed to find cache from thread PU!");
        assert(("Caches should have attributes", cache->attr));
        assert(cache->attr->cache.size < (uint64_t)SIZE_MAX);
        size_t cache_size = (size_t)cache->attr->cache.size;
        tracef("Requested cache can hold %zu bytes.", cache_size)

        trace("Determining cache cpuset...");
        assert(("Caches should have a cpuset", cache->cpuset));
        const hwloc_cpuset_t cache_cpuset = hwloc_bitmap_dup(cache->cpuset);
        exit_on_null(cache_cpuset, "Failed to duplicate cache cpuset!");
        if (log_enabled(UDIPE_TRACE)) {
            char* cpuset_str;
            exit_on_negative(hwloc_bitmap_list_asprintf(&cpuset_str, cache_cpuset),
                             "Failed to display cache cpuset!");
            tracef("Cache is attached to CPU(s) %s.", cpuset_str);
            free(cpuset_str);
        }

        trace("Removing hyperthreads...");
        int result = hwloc_bitmap_singlify_per_core(topology, cache_cpuset, 0);
        assert(result == 0);
        if (log_enabled(UDIPE_TRACE)) {
            char* cpuset_str;
            exit_on_negative(hwloc_bitmap_list_asprintf(&cpuset_str, cache_cpuset),
                             "Failed to display cache cpuset!");
            tracef("That leaves CPU(s) %s.", cpuset_str);
            free(cpuset_str);
        }

        trace("Computing fair share of cache across attached CPU(s)...");
        const int weight = hwloc_bitmap_weight(cache_cpuset);
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

/// Get the system page size
static size_t get_page_size() {
    const long page_size_l = sysconf(_SC_PAGE_SIZE);
    if (page_size_l < 1) exit_after_c_error("Failed to query system page size!");
    return (size_t)page_size_l;
}

/// Apply defaults and page rounding to a \ref udipe_buffer_config_t
///
/// This prepares the config struct for use within the actual allocator by
/// replacing placeholder zeroes with actual default values and rounding up the
/// buffer size to the next multiple of the system page size.
UDIPE_NON_NULL_ARGS
static void finish_configuration(udipe_buffer_config_t* config,
                                 hwloc_topology_t topology) {
    debug("Querying system page size...");
    const size_t page_size = get_page_size();
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

        if (log_enabled(UDIPE_DEBUG)) {
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
    const size_t page_remainder = config->buffer_size % page_size;
    if (page_remainder != 0) {
        config->buffer_size += page_size - page_remainder;
    }
    infof("Selected a buffer size of %1$zu (%1$#zx) bytes.",
           config->buffer_size);

    if (config->buffer_count == 0) {
        debug("Auto-tuning buffer count for L2 locality...");
        const size_t pool_size = smallest_cache_capacity(topology,
                                                         thread_cpuset,
                                                         HWLOC_OBJ_L2CACHE);
        debugf("Optimal memory pool size for L2 locality is %1$zu (%1$#zx) bytes.",
               pool_size);
        config->buffer_count = pool_size / config->buffer_size;
        if ((pool_size % config->buffer_size) != 0) {
            config->buffer_count += 1;
        }
        if (config->buffer_count <= UDIPE_MAX_BUFFERS) {
            infof("Will allocate a pool of %zu buffers.", config->buffer_count);
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
buffer_allocator_t
buffer_allocator_initialize(udipe_buffer_configurator_t configurator,
                            hwloc_topology_t topology) {
    buffer_allocator_t allocator;
    if (configurator.callback) {
        debug("Obtaining configuration from user callback...");
        allocator.config = (configurator.callback)(configurator.context);
        debugf("User requested buffer_size %zu "
               "and buffer_count %zu (0 = default)",
               allocator.config.buffer_size,
               allocator.config.buffer_count);
    } else {
        debug("No user callback specified, will use default configuration.");
        if (configurator.context) {
            exit_with_error("Do not set udipe_buffer_configurator_t::context "
                            "without also setting the callback field!");
        }
        memset(&allocator.config, 0, sizeof(udipe_buffer_config_t));
    }

    debug("Applying defaults and page rounding...");
    finish_configuration(&allocator.config, topology);

    debug("Allocating the memory pool...");
    const size_t pool_size =
        allocator.config.buffer_size * allocator.config.buffer_count;
    allocator.memory_pool = mmap(NULL,
                                 pool_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS,
                                 -1,
                                 0);
    exit_on_null(allocator.memory_pool, "Failed to allocate memory pool!");
    tracef("Allocated memory pool at location %p.", allocator.memory_pool);

    debug("Locking memory pages into RAM...");
    exit_on_negative(mlock(allocator.memory_pool, pool_size),
                     "Failed to lock memory pages into RAM!");

    debug("Initializing the availability bit array...");
    const bit_pos_t buffers_end = index_to_bit_pos(allocator.config.buffer_count);
    bit_array_range_set(allocator.buffer_availability,
                        UDIPE_MAX_BUFFERS,
                        BIT_ARRAY_START,
                        buffers_end,
                        true);
    bit_array_range_set(allocator.buffer_availability,
                        UDIPE_MAX_BUFFERS,
                        buffers_end,
                        bit_array_end(UDIPE_MAX_BUFFERS),
                        false);
    return allocator;
}

void buffer_allocator_finalize(buffer_allocator_t allocator) {
    debug("Finalizing the buffer allocator");
    assert(
        bit_array_range_alleq(allocator.buffer_availability,
                              UDIPE_MAX_BUFFERS,
                              BIT_ARRAY_START,
                              index_to_bit_pos(allocator.config.buffer_count),
                              true)
    );
    munmap(allocator.memory_pool,
           allocator.config.buffer_size * allocator.config.buffer_count);
}

UDIPE_NON_NULL_ARGS
void buffer_liberate(buffer_allocator_t* allocator, void* buffer) {
    assert(allocator);
    assert(buffer);

    tracef("Liberating buffer with address %p...", buffer);
    assert(buffer >= allocator->memory_pool);
    const size_t buffer_offset = (char*)buffer - (char*)allocator->memory_pool;
    assert(buffer_offset % allocator->config.buffer_size == 0);
    const size_t buffer_idx = buffer_offset / allocator->config.buffer_size;
    assert(buffer_idx < allocator->config.buffer_count);
    const bit_pos_t buffer_bit = index_to_bit_pos(buffer_idx);
    assert(!bit_array_get(allocator->buffer_availability,
                          UDIPE_MAX_BUFFERS,
                          buffer_bit));
    bit_array_set(allocator->buffer_availability,
                  UDIPE_MAX_BUFFERS,
                  buffer_bit,
                  true);
}

UDIPE_NON_NULL_ARGS
BUFFER_ALLOCATE_ATTRIBUTES
void* buffer_allocate(buffer_allocator_t* allocator) {
    assert(allocator);

    trace("Starting buffer allocation...");
    const bit_pos_t buffer_bit =
        bit_array_find_first(allocator->buffer_availability,
                             UDIPE_MAX_BUFFERS,
                             true);

    if (buffer_bit.word == SIZE_MAX) {
        trace("Allocation rejected because no buffer is currently available.");
        return NULL;
    }

    bit_array_set(allocator->buffer_availability,
                  UDIPE_MAX_BUFFERS,
                  buffer_bit,
                  false);
    const size_t buffer_idx = bit_pos_to_index(buffer_bit);
    const size_t buffer_offset = buffer_idx * allocator->config.buffer_size;
    void* buffer = (void*)((char*)allocator->memory_pool + buffer_offset);
    tracef("Allocated buffer #%zu with address %p.", buffer_idx, buffer);
    return buffer;
}


#ifdef UDIPE_BUILD_TESTS

    /// Make sure that an allocator, which has been configured in a certain
    /// way, meets expected requirements. Then finalize it.
    static void check_and_finalize(
        buffer_allocator_t allocator,
        udipe_buffer_config_t config,
        size_t page_size
    ) {
        trace("Checking default allocator configuration...");
        size_t min_size = config.buffer_size;
        // Default configuration should be able to hold any possible UDP packet,
        // and 9216 bytes is a common MTU limit for Ethernet equipment
        if (!min_size) min_size = 9216;
        ensure_ge(allocator.config.buffer_size, min_size);
        ensure_eq(allocator.config.buffer_size % page_size, (size_t)0);
        size_t min_count = config.buffer_count;
        if (!min_count) min_count = 1;
        ensure_ge(allocator.config.buffer_count, min_count);

        trace("Backing up initial allocator configuration...");
        config = allocator.config;
        const void* const memory_pool = allocator.memory_pool;

        trace("Checking memory pool pointer...");
        ensure((bool)allocator.memory_pool);
        ensure_eq((size_t)allocator.memory_pool % page_size, (size_t)0);

        trace("Checking initial buffer availability...");
        const bit_pos_t buffers_end = index_to_bit_pos(config.buffer_count);
        ensure(bit_array_range_alleq(allocator.buffer_availability,
                                     UDIPE_MAX_BUFFERS,
                                     BIT_ARRAY_START,
                                     buffers_end,
                                     true)
        );
        ensure(bit_array_range_alleq(allocator.buffer_availability,
                                     UDIPE_MAX_BUFFERS,
                                     buffers_end,
                                     bit_array_end(UDIPE_MAX_BUFFERS),
                                     false));

        trace("Allocating all the buffers...");
        void* buffers[UDIPE_MAX_BUFFERS];
        for (size_t buf = 0; buf < UDIPE_MAX_BUFFERS; ++buf) {
            tracef("Allocating buffer #%zu...", buf);
            buffers[buf] = buffer_allocate(&allocator);

            trace("Checking invariant fields...");
            ensure_eq(allocator.config.buffer_size, config.buffer_size);
            ensure_eq(allocator.config.buffer_count, config.buffer_count);
            ensure_eq(allocator.memory_pool, memory_pool);

            trace("Handling allocation failure...");
            if (!buffers[buf]) {
                ensure_ge(buf, config.buffer_count);
                ensure_eq(bit_array_count(allocator.buffer_availability,
                                          UDIPE_MAX_BUFFERS,
                                          true),
                          (size_t)0);
                continue;
            }

            trace("Handling allocation success...");
            ensure_lt(buf, config.buffer_count);
            ensure_eq(bit_array_count(allocator.buffer_availability,
                                      UDIPE_MAX_BUFFERS,
                                      true),
                      config.buffer_count - buf - 1);
            const size_t offset = (char*)buffers[buf] - (char*)memory_pool;
            ensure_eq(offset % config.buffer_size, (size_t)0);
            ensure_lt(offset / config.buffer_size, config.buffer_count);

            trace("Checking allocation unicity...");
            for (size_t other = 0; other < buf; ++other) {
                ensure_ne(buffers[buf], buffers[other]);
            }
        }

        trace("Liberating all the buffers...");
        for (size_t buf = 0; buf < config.buffer_count; ++buf) {
            tracef("Liberating buffer #%zu...", buf);
            buffer_liberate(&allocator, buffers[buf]);

            trace("Checking invariant fields...");
            ensure_eq(allocator.config.buffer_size, config.buffer_size);
            ensure_eq(allocator.config.buffer_count, config.buffer_count);
            ensure_eq(allocator.memory_pool, memory_pool);

            trace("Checking availability bit array...");
            ensure_eq(bit_array_count(allocator.buffer_availability,
                                      UDIPE_MAX_BUFFERS,
                                      true),
                      buf + 1);
        }

        trace("Finalizing the allocator...");
        buffer_allocator_finalize(allocator);
    }

    /// Configuration callback that applies a predefined configuration
    /// without thread-specific adjustments
    udipe_buffer_config_t apply_shared_configuration(void* context) {
        const udipe_buffer_config_t* config = (udipe_buffer_config_t*)context;
        return *config;
    }

    void buffer_unit_tests() {
        info("Running allocator unit tests...");
        with_log_level(UDIPE_DEBUG, {
            debug("Setting up an hwloc topology...");
            hwloc_topology_t topology;
            exit_on_negative(hwloc_topology_init(&topology),
                             "Failed to allocate the hwloc hopology!");
            exit_on_negative(hwloc_topology_load(topology),
                             "Failed to build the hwloc hopology!");

            debug("Checking system page size...");
            const size_t page_size = get_page_size();
            debugf("System page size is %1$zu (%1$#zx) bytes.", page_size);

            debug("Testing the default configuration...");
            udipe_buffer_configurator_t configurator;
            udipe_buffer_config_t config;
            buffer_allocator_t allocator;
            with_log_level(UDIPE_TRACE, {
                memset(&configurator, 0, sizeof(udipe_buffer_configurator_t));
                memset(&config, 0, sizeof(udipe_buffer_config_t));
                allocator = buffer_allocator_initialize(configurator, topology);
                check_and_finalize(allocator,
                                   config,
                                   page_size);
            });

            debug("Preparing for manual configurations...");
            configurator.callback = apply_shared_configuration;
            configurator.context = (void*)&config;

            debug("Testing a minimal configuration (1 x 1500B)...");
            with_log_level(UDIPE_TRACE, {
                config = (udipe_buffer_config_t){
                    .buffer_size = 1500,
                    .buffer_count = 1
                };
                allocator = buffer_allocator_initialize(configurator, topology);
                check_and_finalize(allocator,
                                   config,
                                   page_size);
            });

            debug("Testing a bigger configuration (MAX x 9216B)...");
            with_log_level(UDIPE_TRACE, {
                config = (udipe_buffer_config_t){
                    .buffer_size = 9216,
                    .buffer_count = UDIPE_MAX_BUFFERS
                };
                allocator = buffer_allocator_initialize(configurator, topology);
                check_and_finalize(allocator,
                                   config,
                                   page_size);
            });
        });
    }

#endif  // UDIPE_BUILD_TESTS
