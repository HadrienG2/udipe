#include "memory.h"

#include <udipe/pointer.h>

#include "arch.h"
#include "bits.h"
#include "error.h"
#include "log.h"
#include "visibility.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <threads.h>

#ifdef __unix__
    #include <sys/mman.h>
    #include <sys/resource.h>
    #include <unistd.h>
#elif defined(_WIN32)
    #include <memoryapi.h>
    #include <processthreadsapi.h>
    #include <sysinfoapi.h>
#endif


DEFINE_PUBLIC pow2_t system_page_size_pow2 = { 0 };

/// Buffer size granularity of the system allocator, encoded as a power of two.
///
/// This variable is constant after initialization, but you must call
/// expect_system_config() before accessing it in order to ensure that it is
/// initialized in a thread-safe manner.
static pow2_t system_allocation_granularity_pow2 = { 0 };

#ifdef _WIN32
    /// Pseudo-handle to the current process
    ///
    /// This variable is constant after initialization, but you must call
    /// expect_system_config() before accessing it in order to ensure that it is
    /// initialized in a thread-safe manner.
    static HANDLE system_current_process = NULL;
#endif

/// Implementation of expect_system_config()
///
/// This is the call_once() callback that expect_system_config() uses in order
/// to ensure that the `system_` variables are initialized exactly once. It
/// should not be called directly as it is not thread-safe.
///
/// This function must be called within the scope of with_logger().
static void read_system_config() {
    debug("Reading OS configuration...");

    trace("Reading memory management properties...");
    uint32_t page_size, allocation_granularity;
    #ifdef __unix__
        const long page_size_l = sysconf(_SC_PAGE_SIZE);
        if (page_size_l < 1) {
            exit_after_c_error("Failed to query system page size!");
        }
        if (page_size_l > (long)UINT32_MAX) {
            exit_after_c_error("That's an unexpectedly big page size!");
        }
        page_size = (uint32_t)page_size_l;
        allocation_granularity = page_size;
    #elif defined(_WIN32)
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        if (info.dwPageSize > (DWORD)UINT32_MAX) {
            exit_after_c_error("That's an unexpectedly big page size!");
        }
        page_size = info.dwPageSize;
        if (info.dwAllocationGranularity > (DWORD)UINT32_MAX) {
            exit_after_c_error("That's an unexpectedly big allocation granularity!");
        }
        allocation_granularity = info.dwAllocationGranularity;

        trace("Reading current process pseudo handle...");
        system_current_process = GetCurrentProcess();
        assert(system_current_process);
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif

    infof("Will use memory pages of size %u (%#x) bytes.",
          page_size, page_size);
    assert(page_size >= MIN_PAGE_ALIGNMENT);
    system_page_size_pow2 = pow2_encode(page_size);
    infof("OS kernel allocates memory with a granularity of %u (%#x) bytes.",
          allocation_granularity, allocation_granularity);
    assert(allocation_granularity >= page_size);
    system_allocation_granularity_pow2 = pow2_encode(allocation_granularity);
}

DEFINE_PUBLIC void expect_system_config() {
    static once_flag config_was_read = ONCE_FLAG_INIT;
    call_once(&config_was_read, read_system_config);
}

/// Current system allocation granularity in bytes
///
/// This function must be called within the scope of with_logger().
static inline size_t get_allocation_granularity() {
    expect_system_config();
    return (size_t)pow2_decode(system_allocation_granularity_pow2);
}


/// Round an allocation size up to the next multiple of the OS kernel's memory
/// allocator granularity
///
/// The granularity is just the page size on Unix systems, but it can be larger
/// on other operating systems like Windows.
static size_t allocation_size(size_t size) {
    size_t allocation_granularity = get_allocation_granularity();
    const size_t trailing_bytes = size % allocation_granularity;
    if (trailing_bytes != 0) {
        size += allocation_granularity - trailing_bytes;
        tracef("Rounded allocation size up to %zu (%#zx) bytes.", size, size);
    }
    return size;
}


/// Mutex that protects the OS kernel's memory locking limit
///
/// Unfortunately, the kernel APIs that must be used to adjust this limit are
/// thread unsafe on both Linux or Windows, because they only expose read/write
/// transactions and not increment/decrement transactions. This mutex handles
/// the associated race condition hazard when multiple udipe threads allocate
/// locked memory, but it cannot help with race conditions from non-udipe
/// threads concurrently adjusting the budget.
///
/// To reduce the underlying race condition risk and improve memory allocation
/// performance, we increase the memory locking budget via exponential doubling
/// as long as the OS kernel will allow us to do so.
static mtx_t mlock_budget_mutex;

/// Initialize mlock_budget_mutex (implementation detail of try_increase_mlock_budget)
static void mlock_budget_mutex_initialize() {
    debug("Initializing mlock_budget_mutex");
    const int result = mtx_init(&mlock_budget_mutex, mtx_plain);
    if (result == thrd_success) return;
    exit_after_c_error("Failed to initialize mlock_budget_mutex!");
}

/// Increase the OS kernel's memory locking limit to accomodate a new allocation
/// of `size` bytes, if possible.
///
/// For performance and correctness reasons, the actual kernel memory locking
/// budget will be increased in a super-linear fashion, meaning that that this
/// function should not need to be called once per realtime_allocate() call.
///
/// \returns `true` if the operation succeeded, `false` if it failed. Underlying
///          OS errors (errno values etc) are logged as warnings since failure
///          to lock memory is not fatal in udipe.
static bool try_increase_mlock_budget(size_t size) {
    trace("Will now attempt to increase the memory locking limit to accomodate "
          "for %zu more locked bytes.");
    static once_flag mutex_initialized = ONCE_FLAG_INIT;
    call_once(&mutex_initialized, mlock_budget_mutex_initialize);
    mtx_lock(&mlock_budget_mutex);

    bool result = false;
    trace("Querying initial memory locking limit...");
    // RLIMIT_MEMLOCK is a Linux/BSD thing whose broader support is unknown, add
    // support for other OSes as needed after checking they do support it and
    // that they use the same errno logic.
    #ifdef __linux__
        struct rlimit mlock_limit;
        exit_on_negative(
            getrlimit(RLIMIT_MEMLOCK, &mlock_limit),
            "Failed to query the current locking limit for unknown reasons"
        );
        tracef("Current memory locking limit is %zu/%zu bytes",
               (size_t)mlock_limit.rlim_cur, (size_t)mlock_limit.rlim_max);

        const size_t initial_cur = mlock_limit.rlim_cur;
        const size_t initial_max = mlock_limit.rlim_max;
        while ((size_t)mlock_limit.rlim_cur < initial_cur + size) {
            // Normally we follow a simple limit doubling strategy for
            // performance and data race avoidance reasons.
            mlock_limit.rlim_cur *= 2;
        }
        if (mlock_limit.rlim_cur > mlock_limit.rlim_max) {
            if (mlock_limit.rlim_max - initial_cur >= size) {
                // If this gets us above the hard limit but the hard limit is
                // high enough, then better saturate at the hard limit.
                mlock_limit.rlim_cur = mlock_limit.rlim_max;
            } else {
                // Otherwise we have no choice but to try increasing the hard
                // limit (which will fail if the process is not privileged).
                mlock_limit.rlim_max = mlock_limit.rlim_cur;
            }
        }
        tracef("Will attempt to raise the limit to %zu/%zu bytes",
               (size_t)mlock_limit.rlim_cur, (size_t)mlock_limit.rlim_max);

        if (setrlimit(RLIMIT_MEMLOCK, &mlock_limit) == 0) {
            trace("Successfully raised the memory locking limit.");
            result = true;
            goto unlock_and_return;
        }

        switch (errno) {
        // A pointer argument points to a location outside the accessible
        // address space.
        case EFAULT:
        // The value specified in resource is not valid or rlim->rlim_cur was
        // greater than rlim->rlim_max.
        case EINVAL:
            exit_after_c_error("These cases should never be encountered!");
        case EPERM:
            errno = 0;
            assert((size_t)mlock_limit.rlim_max > initial_max);
            warn("Failed to raise the hard memory locking limit. Please "
                 "raise the memory locking limit for the calling user/group "
                 "or give this process the CAP_SYS_RESOURCE capability");
            result = false;
            goto unlock_and_return;
        default:
            warn_on_errno();
            warn("Failed to raise the memory locking limit for unknown reasons!");
            result = false;
            goto unlock_and_return;
        }
    #elif defined (_WIN32)
        expect_system_config();
        size_t min_working_set, max_working_set;
        win32_exit_on_zero(
            GetProcessWorkingSetSize(system_current_process,
                                     &min_working_set,
                                     &max_working_set),
            "Failed to retrieve the working set sizes of the current process!"
        );
        tracef("Current process working set size is %zu/%zu bytes.",
               min_working_set, max_working_set);

        const size_t initial_min = min_working_set;
        while (min_working_set < initial_min + size) {
            min_working_set *= 2;
        }
        max_working_set += min_working_set - initial_min;
        tracef("Will attempt to increase the working set to %zu/%zu bytes.",
               min_working_set, max_working_set);

        if (SetProcessWorkingSetSize(system_current_process,
                                     min_working_set,
                                     max_working_set)) {
            trace("Successfully increased the process working set.");
            result = true;
            goto unlock_and_return;
        }

        win32_warn_on_error();
        warn("Failed to increase the process working set!");
        result = false;
        goto unlock_and_return;
    #else
        #warning "Sorry, we don't fully support your operating system yet. Please file a bug report about it!"
        warn("Don't know how to increase the memory locking budget on this "
             "operating system, so won't do it...");
    #endif

unlock_and_return:
    mtx_unlock(&mlock_budget_mutex);
    return result;
}


UDIPE_NON_NULL_RESULT
REALTIME_ALLOCATE_ATTRIBUTES
void* realtime_allocate(size_t size) {
    ensure_gt(size, (size_t)0);

    const size_t page_size = get_page_size();

    debugf("Asked to allocate %zu bytes for realtime thread use.", size);
    size = allocation_size(size);
    assert(size % page_size == 0);

    void* result = NULL;
    const char* mlock_failure_msg =
        "Failed to lock memory in an unrecoverable manner. "
        "This isn't fatal but creates a new realtime performance hazard, "
        "namely the OS kernel taking bad swapping decisions.";
    #ifdef __unix__
        // Allocate virtual memory pages
        result = mmap(NULL,
                      size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON,
                      -1,
                      0);
        if (result == MAP_FAILED) exit_after_c_error("Failed to allocate memory!");
        tracef("Allocated memory pages at virtual location %p.", result);
        assert((size_t)result % page_size == 0);

        trace("Now, let's try to lock allocated pages into RAM...");
        if (mlock(result, size) == 0) {
            trace("mlock() succeeded on first try.");
            goto log_and_return;
        }
        switch (errno) {
        // Either addr+size overflows or addr is not aligned to the page size
        case EINVAL:
            exit_after_c_error("Cannot happen if mmap() works correctly!");
        // This can mean several different things:
        // - Not in process address space (impossible if mmap() works correctly)
        // - Maximal number of memory mappings exceeded
        // - RLIMIT_MEMLOCK soft limit exceeded
        case ENOMEM:
            errno = 0;
            trace("Failed to lock memory, but it may come from a soft limit. "
                  "Let's try to raise the limit before giving up...");
            break;
        // Some or all of the specified address range could not be locked for
        // unspecified reasons.
        case EAGAIN:
        // The caller is not privileged, but needs privilege (CAP_IPC_LOCK) to
        // lock memory pages.
        case EPERM:
        // An unknown error occured, most likely from a non-Linux unix host
        default:
            warn_on_errno();
            warn(mlock_failure_msg);
            goto prefault_and_return;;
        }

        // If the first mlock failed, try to increase the underlying rlimit
        if (!try_increase_mlock_budget(size)) goto prefault_and_return;

        // If mlock fails again after adjusting the rlimit, then give up
        if (mlock(result, size) == 0) {
            trace("mlock() succeeded after raising the rlimit.");
            goto log_and_return;
        }
        warn_on_errno();
        warn(mlock_failure_msg);
        goto prefault_and_return;
    #elif defined(_WIN32)
        // Allocate virtual memory pages
        result = VirtualAlloc(NULL,
                              size,
                              MEM_COMMIT | MEM_RESERVE,
                              PAGE_READWRITE);
        win32_exit_on_zero(result);
        tracef("Allocated memory pages at virtual location %p.", result);
        assert((size_t)result % page_size == 0);

        trace("Now, let's try to lock allocated pages into RAM...");
        if (VirtualLock(result, size) == 0) {
            trace("VirtualLock() succeeded on first try.");
            goto log_and_return;
        }
        win32_warn_on_error();
        trace("Failed to lock memory, but maybe it's just that the process "
              "working set is too low. Try to raise it before giving up...");

        // If the first mlock failed, try to increase the underlying rlimit
        if (!try_increase_mlock_budget(size)) goto prefault_and_return;

        // If mlock fails again after adjusting the rlimit, then give up
        if (VirtualLock(result, size) == 0) {
            trace("VirtualLock() succeeded after raising the working set.");
            goto log_and_return;
        }
        win32_warn_on_error();
        warn(mlock_failure_msg);
        goto prefault_and_return;
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
    debugf("Done allocating memory at address %p.", result);
    return result;
}


UDIPE_NON_NULL_ARGS
void realtime_liberate(void* buffer, size_t size) {
    debugf("Liberating %zu previously allocated byte(s) at address %p...",
           size, buffer);
    size = allocation_size(size);

    #ifndef NDEBUG
        debug("...after zeroing it to detect more bugs...");
        memset(buffer, 0, size);
    #endif

    // Note that neither code path decreases RLIMIT_MEMLOCK (Unix) or the
    // process working set size (Windows). While this is obviously meh from a
    // "telling the OS kernel the whole truth" perspective, it still feels like
    // the right move for the following reasons:
    //
    // - We don't increase the limit if we don't need to, so this is not a
    //   strict resource leak where the limit keeps increasing indefinitely.
    //   We're merely keeping the limit at our maximal resource usage so far,
    //   which is a (possibly bad) upper bound on our actual resource usage.
    // - If we decrease the limit when an allocation is liberated, then we need
    //   to increase it again when we allocate again, so every
    //   allocation/liberation call will come with extra limit adjustement
    //   syscalls, which is bad for runtime perf. In contrast, if we don't
    //   decrease the limit, it should eventually converge to a correct upper
    //   bound that doesn't need adjusting anymorre.
    // - By avoiding limit-setting syscalls in the long run, we also reduce the
    //   risk of associated race conditions, which is also good, though it would
    //   obviously be better if POSIX and Windows limit-adjustment syscalls
    //   weren't racy by design...
    #ifdef __unix__
        exit_on_negative(munmap(buffer, size),
                         "Failed to liberate memory");
    #elif defined(_WIN32)
        win32_exit_on_zero(VirtualFree(buffer, 0, MEM_RELEASE),
                           "Failed to liberate memory");
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
}


#ifdef UDIPE_BUILD_TESTS

    /// Run the unit tests for system configuration checks
    static void test_system_config() {
        info("Running system configuration unit tests...");
        with_log_level(UDIPE_DEBUG, {
            size_t page_size = get_page_size();
            size_t allocation_granularity = get_allocation_granularity();
            ensure_ge(page_size, MIN_PAGE_ALIGNMENT);
            ensure_ge(page_size, EXPECTED_MIN_PAGE_SIZE);
            ensure_eq(allocation_granularity % page_size, (size_t)0);
        });
    }

    /// Test memory allocation functions with a certain allocation size
    static void check_allocation_size(size_t size) {
        volatile unsigned char* alloc = (volatile unsigned char*)realtime_allocate(size);
        tracef("Allocated memory at address %p.", (void*)alloc);
        ensure_ne((size_t)alloc, (size_t)0);

        const size_t page_size = get_page_size();
        size_t min_size = size;
        if (size % page_size != 0) min_size += page_size - (size % page_size);
        tracef("Allocation should be at least %zu bytes large.", min_size);

        trace("Writing and checking each of the expected bytes...");
        for (size_t byte = 0; byte < min_size; ++byte) {
            unsigned char value = (unsigned char)(byte % 255 + 1);
            alloc[byte] = value;
            ensure_eq(alloc[byte], value);
        }

        trace("Liberating the allocation...");
        realtime_liberate((void*)alloc, size);
    }

    /// Run the unit tests for memory allocation functions
    static void test_allocator() {
        info("Running system memory allocator unit tests...");
        with_log_level(UDIPE_DEBUG, {
            const size_t page_size = get_page_size();
            const size_t alloc_sizes[] = {
                1,
                page_size - 1, page_size, page_size + 1,
                2*page_size - 1, 2*page_size, 2*page_size + 1
            };
            for (size_t i = 0; i < sizeof(alloc_sizes)/sizeof(size_t); ++i) {
                const size_t alloc_size = alloc_sizes[i];
                debugf("Exercising an allocation size of %zu bytes...", alloc_size);
                with_log_level(UDIPE_TRACE, {
                    check_allocation_size(alloc_size);
                });
            }
        });
    }

    void memory_unit_tests() {
        test_system_config();
        test_allocator();
    }

#endif  // UDIPE_BUILD_TESTS
