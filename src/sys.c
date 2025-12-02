#include "sys.h"

#include "arch.h"
#include "error.h"
#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>

#ifdef __unix__
    #include <sys/mman.h>
    #include <sys/resource.h>
    #include <unistd.h>
#elif defined(_WIN32)
    #include <errhandlingapi.h>
    #include <memoryapi.h>
    #include <processthreadsapi.h>
    #include <stringapiset.h>
    #include <sysinfoapi.h>
    #include <winerror.h>
#endif

#ifdef __linux__
    #include <linux/prctl.h>
    #include <sys/prctl.h>
#endif


/// Memory page size used for realtime allocations
///
/// This variable is constant after initialization, but you must call
/// expect_system_config() before accessing it in order to ensure that it is
/// initialized in a thread-safe manner.
static size_t system_page_size = 0;

/// Buffer size granularity of the system allocator
///
/// This variable is constant after initialization, but you must call
/// expect_system_config() before accessing it in order to ensure that it is
/// initialized in a thread-safe manner.
static size_t system_allocation_granularity = 0;

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

        trace("Reading current process pseudo handle...");
        system_current_process = GetCurrentProcess();
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


/// Round an allocation size up to the next multiple of the OS kernel's memory
/// allocator granularity
///
/// The granularity is just the page size on Unix systems, but it can be larger
/// on other operating systems like Windows.
static size_t allocation_size(size_t size) {
    expect_system_config();
    const size_t trailing_bytes = size % system_allocation_granularity;
    if (trailing_bytes != 0) {
        size += system_allocation_granularity - trailing_bytes;
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
            warning("Failed to raise the hard memory locking limit. Please "
                    "raise the memory locking limit for the calling user/group "
                    "or give this process the CAP_SYS_RESOURCE capability");
            result = false;
            goto unlock_and_return;
        default:
            warn_on_errno();
            warning("Failed to raise the memory locking limit for unknown reasons!");
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
        warning("Failed to increase the process working set!");
        result = false;
        goto unlock_and_return;
    #else
        #warning "Sorry, we don't fully support your operating system yet. Please file a bug report about it!"
        warning("Don't know how to increase the memory locking budget on this "
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

    expect_system_config();
    const size_t page_size = system_page_size;

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
        // An unknown error occured, most likely from a non-Linux *nix host
        default:
            warn_on_errno();
            warning(mlock_failure_msg);
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
        warning(mlock_failure_msg);
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
        warning(mlock_failure_msg);
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


/// Thread-local buffer for thread name related queries
///
/// This struct manages a buffer that can be used for purposes such as...
///
/// - Receiving the thread name from a syscall that writes it to a buffer, like
///   `prctl(PR_GET_NAME, buf)` on Linux.
/// - Holding different versions of the thread name when the charset used by
///   syscalls is not the same as that used by the C application, as is the case
///   on Windows where syscalls use UTF-16.
/// - Keeping the thread name around on operating systems that don't have a
///   standard way to give a persistent name to threads, as is the case on older
///   versions of Windows.
typedef struct thread_name_s {
    /// Size of the `bytes` member
    ///
    /// Guaranteed to be large enough to store an empty string (single trailing
    /// NUL) in any format used during thread name operations.
    size_t capacity;

    /// Bytes of storage
    ///
    /// Aligned enough to store strings in any reasonable format.
    alignas(max_align_t) char bytes[];
} thread_name_t;

/// Thread-local storage key used to retrieve this thread's \ref thread_name_t
///
/// The low-level `tss_t` API must be used here because the underlying storage
/// buffer must be freed when this thread stops executing.
static tss_t thread_name_key;

/// Destroy a thread's name storage buffer (\ref thread_name_t)
static void thread_name_finalize(void* thread_name) {
    // WARNING: This function is called at a time where no logger is active and
    //          must therefore not perform any logging. Normal events and
    //          non-fatal errors should not be signaled at all, fatal errors
    //          should be signalled on stderr before exiting.

    thread_name_t* const name = (thread_name_t*)thread_name;
    assert(("C11 guarantees that NULL pointers won't be destroyed", name));

    if (name->capacity == 0) {
        fprintf(stderr, "Asked to destroy invalid buffer with zero capacity!\n");
        exit(EXIT_FAILURE);
    }
    name->capacity = 0;

    free(name);
}

/// Set up thread-local storage for thread names
///
/// Note that this only sets up a TLS pointer, and actual storage allocatin will
/// happen during set_thread_name() and/or get_thread_name().
static void thread_name_initialize() {
    // WARNING: This function is called by the logger implementation and must
    //          therefore not perform any logging. Normal events and non-fatal
    //          errors should not be signaled at all, fatal errors should be
    //          signalled on stderr before exiting.

    if (tss_create(&thread_name_key, thread_name_finalize) != thrd_success) {
        fprintf(stderr, "Failed to set up thread name storage!\n");
        exit(EXIT_FAILURE);
    }
}

/// `once_flag` ensuring that thread_name_initialize() is only called once
static once_flag thread_name_init = ONCE_FLAG_INIT;

/// Maximum number of bytes within a thread name, including trailing NUL
#define MAX_THREAD_NAME_SIZE (MAX_THREAD_NAME_LEN+1)

/// Ensure that the thread name buffer is allocated with a certain capacity
thread_name_t* ensure_thread_name_capacity(size_t capacity) {
    // WARNING: This function is called by the logger implementation and must
    //          therefore not perform any logging. Normal events and non-fatal
    //          errors should not be signaled at all, fatal errors should be
    //          signalled on stderr before exiting.

    // Enforce a minimum capacity so that in the common case where thread names
    // are only set via set_thread_name, the thread name buffer will only need
    // to be allocated once without any future reallocation.
    if (capacity < MAX_THREAD_NAME_SIZE) capacity = MAX_THREAD_NAME_SIZE;

    // Grab the current buffer, if any
    thread_name_t* thread_name = (thread_name_t*)tss_get(thread_name_key);

    // Check if a buffer needs to be allocated or reallocated
    bool needs_realloc = false;
    if (!thread_name) needs_realloc = true;
    if (thread_name && thread_name->capacity < capacity) needs_realloc = true;

    // If so, reallocate the buffer...
    if (needs_realloc) {
        void* const new_thread_name =
            realloc(thread_name, offsetof(thread_name_t, bytes) + capacity);
        if (!new_thread_name) {
            // No need to free old buffer, we're exiting anyway
            // Can't log, this is used in the logger implementation.
            fprintf(stderr, "Failed to allocate thread name buffer!\n");
            exit(EXIT_FAILURE);
        }

        // ...and update its address in TLS if it changed
        if (new_thread_name != thread_name) {
            if (tss_set(thread_name_key, new_thread_name) != thrd_success) {
                // Can't log, this is used in the logger implementation.
                fprintf(stderr, "Failed to save thread name buffer to TLS!\n");
                exit(EXIT_FAILURE);
            }
            thread_name = (thread_name_t*)new_thread_name;
        }

        // Don't forget to update the capacity metadata too
        thread_name->capacity = capacity;
    }
    return thread_name;
}

UDIPE_NON_NULL_ARGS
void set_thread_name(const char* name) {
    debugf("Asked to rename current thread to %s.", name);

    trace("Validating that name is printable ASCII and under maximum length...");
    size_t name_len = strlen(name);
    ensure_le(name_len, MAX_THREAD_NAME_LEN);
    for (size_t i = 0; i < name_len; ++i) {
        ensure_ge((uint8_t)name[i], 0x21);
        ensure_le((uint8_t)name[i], 0x7e);
    }

    trace("Setting the thread name...");
    #ifdef __linux__
        exit_on_negative(prctl(PR_SET_NAME, name),
                         "Failed to set thread name!");
    #elif defined(_WIN32)
        // This is large enough because...
        //
        // - We have checked that name is ASCII, which contains 1 code point per
        //   byte + the trailing NUL.
        // - Every ASCII code point has a matching UTF-16 code point without any
        //   need for surrogate pairs.
        wchar_t name_utf16[MAX_THREAD_NAME_SIZE];
        trace("- Converting thread name to UTF-16");
        const int result = MultiByteToWideChar(CP_UTF8,
                                               MB_ERR_INVALID_CHARS,
                                               name,
                                               name_len + 1,
                                               name_utf16,
                                               MAX_THREAD_NAME_SIZE);
        win32_exit_on_zero(result, "Failed to convert thread name to UTF-16!");

        trace("- Setting the thread description to this UTF-16 string");
        const HRESULT hr = SetThreadDescription(GetCurrentThread(), name_utf16);
        win32_exit_on_failed_hresult(hr, "Failed to set thread description!");
    #else
        #warning "Sorry, we don't fully support your operating system yet. Please file a bug report about it!"

        trace("- Setting up thread name storage...");
        call_once(&thread_name_init, thread_name_initialize);

        trace("- Allocating or reusing thread name buffer...");
        thread_name_t* thread_name =
            ensure_thread_name_capacity(MAX_THREAD_NAME_SIZE);
        assert(thread_name);
        assert(thread_name->capacity >= MAX_THREAD_NAME_SIZE);

        trace("- Copying the new name into the thread name buffer...");
        assert(("Guaranteed to be true because thread_name is allocated to be "
                "at least MAX_THREAD_NAME_LEN bytes long",
                thread_name->capacity >= name_len + 1));
        strncpy(&thread_name->bytes, name, name_len + 1);
    #endif
}

UDIPE_NON_NULL_RESULT
const char* get_thread_name() {
    // WARNING: This function is called by the logger implementation and must
    //          therefore not perform any logging. Normal events and non-fatal
    //          errors should not be signaled at all, fatal errors should be
    //          signalled on stderr before exiting.

    // Set up thread-local name buffering. Even on platforms where thread name
    // lengths are bounded, we cannot use a stack-allocated fixed-size buffer
    // because we must return a pointer to the buffer from this function.
    call_once(&thread_name_init, thread_name_initialize);

    // Query the current thread name
    thread_name_t* thread_name = NULL;
    #ifdef __linux__
        thread_name = ensure_thread_name_capacity(MAX_THREAD_NAME_SIZE);
        assert(thread_name);
        assert(thread_name->capacity >= MAX_THREAD_NAME_SIZE);
        if (prctl(PR_GET_NAME, &thread_name->bytes) < 0) {
            // Can't log, this is used in the logger implementation.
            fprintf(stderr, "Failed to query thread name!\n");
            exit(EXIT_FAILURE);
        }
    #elif defined(_WIN32)
        // On Windows, we first get a UTF-16 thread description...
        PWSTR name_utf16 = NULL;
        HRESULT hr = GetThreadDescription(GetCurrentThread(), &PWSTR);
        if (FAILED(hr)) {
            // Can't log, this is used in the logger implementation.
            fprintf(stderr,
                    "Failed to query thread description with HRESULT %u!\n",
                    hr);
            exit(EXIT_FAILURE);
        }
        assert(name_utf16);

        // ...then we evaluate how large its UTF-8 representation is...
        int size = WideCharToMultiByte(CP_UTF8,
                                       WC_ERR_INVALID_CHARS,
                                       name_utf16,
                                       -1,
                                       NULL,
                                       0,
                                       NULL,
                                       NULL);
        if (size == 0) {
            // Can't log, this is used in the logger implementation.
            fprintf(stderr,
                    "Failed to evaluate UTF-8 size with error code %u!\n",
                    GetLastError());
            exit(EXIT_FAILURE);
        }
        assert(size > 0);

        // ...we allocate a thread name buffer of the right size...
        thread_name = ensure_thread_name_capacity(size);
        assert(thread_name);
        assert(thread_name->capacity >= size);

        // ...and we perform the conversion.
        size = WideCharToMultiByte(CP_UTF8,
                                   WC_ERR_INVALID_CHARS,
                                   name_utf16,
                                   size,
                                   &thread_name->bytes,
                                   thread_name->capacity,
                                   NULL,
                                   NULL);
        if (size == 0) {
            // Can't log, this is used in the logger implementation.
            fprintf(stderr,
                    "Failed to convert to UTF-8 with error code %u!\n",
                    GetLastError());
            exit(EXIT_FAILURE);
        }
        assert(size > 0);

        // Finally we can liberate the thread description
        if (LocalFree(name_utf16)) {
            // Can't log, this is used in the logger implementation.
            fprintf(stderr,
                    "Failed to liberate UTF-16 string with error code %u!\n",
                    GetLastError());
            exit(EXIT_FAILURE);
        }
        name_utf16 = (PWSTR)NULL;
    #else
        #warning "Sorry, we don't fully support your operating system yet. Please file a bug report about it!"

        // Grab the current thread name buffer, if any
        thread_name = (thread_name_t*)tss_get(thread_name_key);

        // Otherwise, we generate a thread name from the bytes of pthread_self()
        // so that each thread at least gets a unique identifier.
        if (!thread_name) {
            // Allocate storage
            const char* const name_header = "pthread_";
            const size_t header_len = strlen(name_header);
            const size_t pthread_hex_size = sizeof(pthread_t) * 2;
            const size_t name_size = header_len + pthread_hex_size + 1;
            thread_name = ensure_thread_name_capacity(name_size);
            assert(thread_name);
            assert(thread_name->capacity >= name_size);

            // Set the thread name based on a stringified pthread_self()
            char* name = &thread_name->bytes;
            strncpy(name, name_header, header_len);
            name += header_len;
            pthread_t thread = pthread_self();
            const unsigned char* thread_bytes = &thread;
            for (size_t b = 0; b < sizeof(pthread_t); b += 1) {
                sprintf(name, "%.2hhX", thread_bytes[b]);
                name += 2;
            }
            // sprintf adds a trailing NUL so we don't need to add one
        }
    #endif
    return thread_name->bytes;
}


#ifdef UDIPE_BUILD_TESTS

    /// Run the unit tests for system configuration checks
    static void test_system_config() {
        info("Testing system configuration readout & consistency...");
        with_log_level(UDIPE_DEBUG, {
            expect_system_config();
            ensure_eq(get_page_size(), system_page_size);
            ensure_ge(system_page_size, MIN_PAGE_ALIGNMENT);
            ensure_ge(system_page_size, EXPECTED_MIN_PAGE_SIZE);
            ensure_eq(system_allocation_granularity % system_page_size, (size_t)0);
        });
    }

    /// Test memory management functions with a certain allocation size
    static void check_allocation_size(size_t size) {
        volatile unsigned char* alloc = (volatile unsigned char*)realtime_allocate(size);
        tracef("Allocated memory at address %p", (void*)alloc);
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

        trace("Liberating the allocation");
        realtime_liberate((void*)alloc, size);
    }

    /// Run the unit tests for memory management functions
    static void test_memory_management() {
        info("Testing memory management functions...");
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

    void sys_unit_tests() {
        test_system_config();
        test_memory_management();
        // TODO: Add unit test for thread name functionality
        // TODO: Test more functionality as it comes
    }

#endif  // UDIPE_BUILD_TESTS
