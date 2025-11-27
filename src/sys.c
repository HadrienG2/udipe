#include "sys.h"

#include "error.h"

#include <threads.h>

#ifdef __unix__
    #include <unistd.h>
#elif defined(_MSC_VER)
    #include <sysinfoapi.h>
#endif


/// Memory page size used for realtime allocations
///
/// This variable is constant after initialization, but you must call
/// expect_system_config() before accessing it in order to ensure that it is
/// initialized in a thread-safe manner.
static size_t system_page_size;

/// Buffer size granularity of the system allocator
///
/// This variable is constant after initialization, but you must call
/// expect_system_config() before accessing it in order to ensure that it is
/// initialized in a thread-safe manner.
static size_t system_allocation_granularity;

/// Implementation of expect_system_config()
///
/// This is the call_once() callback that expect_system_config() uses in order
/// to ensure that the `system_` variables are initialized exactly once. It
/// should not be called directly as it is not thread-safe.
///
/// This function must be called within the scope of with_logger().
static void read_system_config() {
    #ifdef __unix__
        const long page_size_l = sysconf(_SC_PAGE_SIZE);
        if (page_size_l < 1) exit_after_c_error("Failed to query system page size!");
        system_page_size = (size_t)page_size_l;
        system_allocation_granularity = system_page_size;
    #elif defined(_MSC_VER)
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        system_page_size = info.dwPageSize;
        system_allocation_granularity = info.dwAllocationGranularity;
    #else
        #error "Sorry, we don't support your compiler yet. Please file a bug report about it!"
    #endif
    infof("Will use memory pages of size %zu (%#zx) bytes.",
          system_page_size, system_page_size);
    assert(system_page_size >= MIN_PAGE_ALIGNMENT);
    infof("OS kernel allocates memory with a granularity of %zu (%#zx) bytes.",
          system_allocation_granularity, system_allocation_granularity);
    assert(system_allocation_granularity >= system_page_size);
}

/// Prepare to read the `system_` variables
///
/// This function must be called before accessing the `system_` variables. It
/// ensures that said variables are initialized in a thread-safe manner.
///
/// This function must be called within the scope of with_logger().
static void expect_system_config() {
    static once_flag config_was_read = ONCE_FLAG_INIT;
    call_once(&config_was_read, read_system_config);
}

size_t get_page_size() {
    expect_system_config();
    return system_page_size;
}
