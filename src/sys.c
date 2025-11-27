#include "sys.h"

#include "error.h"
#include "log.h"

#include <errno.h>
#include <threads.h>

#ifdef __unix__
    #include <sys/mman.h>
    #include <unistd.h>
#elif defined(_WIN32)
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
    #elif defined(_WIN32)
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        system_page_size = info.dwPageSize;
        system_allocation_granularity = info.dwAllocationGranularity;
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
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

/// Round an allocation size up to the next multiple of the OS granularity
static size_t allocation_size(size_t size) {
    expect_system_config();
    const size_t trailing_bytes = size % system_allocation_granularity;
    if (trailing_bytes != 0) {
        size += system_allocation_granularity - trailing_bytes;
        tracef("Rounded allocation size up to %zu (%#zx) bytes.", size, size);
    }
    return size;
}

UDIPE_NON_NULL_RESULT
REALTIME_ALLOCATE_ATTRIBUTES
void* realtime_allocate(size_t size) {
    expect_system_config();
    const size_t page_size = system_page_size;

    debugf("Asked to allocate %zu bytes for realtime thread use.", size);
    size = allocation_size(size);
    assert(size % page_size == 0);

    void* result;
    const char* mlock_failure_msg =
        "Failed to lock memory in an unrecoverable manner. "
        "This isn't fatal but creates a new realtime performance hazard, "
        "namely the OS kernel taking bad swapping decisions.";
    #ifdef __unix__
        result = mmap(NULL,
                      size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS,
                      -1,
                      0);
        if (result == MAP_FAILED) exit_after_c_error("Failed to allocate memory");
        tracef("Allocated memory pages at virtual location %p.", result);
        assert((size_t)result % page_size == 0);

        trace("First attempt to lock memory pages into RAM...");
        if (mlock(result, size) == 0) goto log_and_return;
        switch (errno) {
        // Either addr+size overflows or addr is not aligned to the page size
        case EINVAL:
            exit_after_c_error("That's impossible if mmap() works correctly");
        // This can mean several different things:
        // - Not in process address space (impossible if mmap() works correctly)
        // - Maximal number of memory mappings exceeded
        // - RLIMIT_MEMLOCK soft limit exceeded
        case ENOMEM:
            errno = 0;
            trace("Failed to lock memory, but there may be a soft limit. "
                  "Let's try to raise the limit...");
            break;
        // Some or all of the specified address range could not be locked for
        // unspecified reasons.
        case EAGAIN:
        // The caller is not privileged, but needs privilege (CAP_IPC_LOCK) to
        // lock memory pages.
        case EPERM:
        // An unknown error occured, most likely from a non-Linux *nix host
        default:
            warn_on_errno();
            warning(mlock_failure_msg);
            goto prefault_and_return;;
        }

        // TODO: The following logic for expanding RLIMIT_MEMLOCK should be
        //       extracted into a separate function.
        // TODO: Under protection of a mutex, with a warning about how this
        //       doesn't protect from other threads manipulating the limit, use
        //       a getrlimit/setrlimit of RLIMIT_MEMLOCK to double the soft
        //       limit, raise it to the hard limit if lower than this double, or
        //       fail if the soft limit is already at the hard limit. Then
        //       attempt mlock again, and this time treat any failure as fatal
        //       with a warning and goto prefault_and_return.
    #elif defined(_WIN32)
        // TODO: Basically translate the above Unix logic to windows vocabulary:
        //       - First, add zero return and GetLastError() support to error.h
        //       - mmap() becomes VirtualAlloc() with MEM_COMMIT | MEM_RESERVE and PAGE_READWRITE
        //       - mlock() becomes VirtualLock()
        //       - getrlimit()/setrlimit() of RLIMIT_MEMLOCK becomes
        //         GetProcessWorkingSetSize()/SetProcessWorkingSetSize() with a
        //         bump to both the min and max working set size.
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif

prefault_and_return:
    trace("If we can't lock our memory, at least pre-fault it...");
    const size_t end_addr = (size_t)result + size;
    for (size_t addr = (size_t)result; addr < end_addr; addr += page_size) {
        volatile char* const ptr = (volatile char*)addr;
        *ptr = 0;
    }

log_and_return:
    debugf("Done allocating memory at virtual location %p.", result);
    return result;
}

UDIPE_NON_NULL_ARGS
void realtime_liberate(void* buffer, size_t size) {
    debugf("Liberating %zu previously allocated bytes at address %p...",
           size, buffer);
    size = allocation_size(size);

    // Note that none of these code paths decrease RLIMIT_MEMLOCK or the process
    // working set size. While somewhat meh from a "telling the kernel the whole
    // truth" perspective, this still seems like the right move for the
    // following reasons:
    //
    // - We don't increase the limit on the allocation size if we don't need to
    //   so this is not a strict resource leak, merely giving the kernel a bad
    //   upper bound on our actual resource usage.
    // - If we decrease the limit when an allocation is liberated, then we need
    //   to increase it again when we allocate again, so every
    //   allocation/liberation call will come with extra limit adjustement
    //   syscalls, which is bad for runtime perf. In contrast, if we don't
    //   decrease the limit, it should eventually converge to a correct upper
    //   bound that doesn't need tweaking.
    // - By avoiding limit-setting syscalls in the long run, we also avoid the
    //   intrinsic race condition that comes with it, which is also good.
    #ifdef __unix__
        exit_on_negative(munmap(buffer, size), "Failed to liberate memory");
    #elif defined(_WIN32)
        // TODO: Implement using VirtualFree()
        // TODO: Add GetLastError() support to error.h
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
}
