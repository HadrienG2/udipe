#include "thread_name.h"

#include "error.h"
#include "log.h"

#include <assert.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>

#ifdef __linux__
    #include <linux/prctl.h>
    #include <sys/prctl.h>
#endif

#ifdef _WIN32
    #include <errhandlingapi.h>
    #include <processthreadsapi.h>
    #include <stringapiset.h>
    #include <winerror.h>
#endif


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

/// First printable ASCII char
#define FIRST_PRINTABLE_ASCII (char)0x21

/// Last printable ASCII char
#define LAST_PRINTABLE_ASCII (char)0x7e

UDIPE_NON_NULL_ARGS
void set_thread_name(const char* name) {
    debugf("Asked to rename current thread to %s.", name);

    trace("Validating that name is printable ASCII and under maximum length...");
    size_t name_len = strlen(name);
    ensure_gt(name_len, (size_t)0);
    ensure_le(name_len, MAX_THREAD_NAME_LEN);
    for (size_t i = 0; i < name_len; ++i) {
        ensure_ge((uint8_t)name[i], (uint8_t)FIRST_PRINTABLE_ASCII);
        ensure_le((uint8_t)name[i], (uint8_t)LAST_PRINTABLE_ASCII);
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

    void thread_name_unit_tests() {
        // Since get_thread_name() is used by the logger, sanity-check it before
        // the first log instead of starting with a log as usual.
        fprintf(stderr, "Checking initial thread name before first log...\n");
        const char* actual_thread_name = get_thread_name();
        ensure(actual_thread_name);
        ensure_gt(strlen(actual_thread_name), (size_t)0);
        char* initial_thread_name = strdup(actual_thread_name);

        info("Running thread name manipulation unit tests...");
        with_log_level(UDIPE_DEBUG, {
            char expected_thread_name[MAX_THREAD_NAME_SIZE];
            for (size_t len = 1; len <= MAX_THREAD_NAME_LEN; ++len) {
                for (size_t i = 0; i < len; ++i) {
                    int printable_range = (int)(LAST_PRINTABLE_ASCII - FIRST_PRINTABLE_ASCII) + 1;
                    int printable_start = (int)FIRST_PRINTABLE_ASCII;
                    expected_thread_name[i] =
                        (char)(rand() % printable_range + printable_start);
                }
                expected_thread_name[len] = '\0';
                debugf("Testing name of length %zu: %s", len, expected_thread_name);

                with_log_level(UDIPE_TRACE, {
                    trace("Setting thread name...");
                    set_thread_name(expected_thread_name);

                    trace("Checking thread name...");
                    actual_thread_name = get_thread_name();
                    ensure(actual_thread_name);

                    tracef("Got name %s", actual_thread_name);
                    ensure_eq(strcmp(expected_thread_name, actual_thread_name), 0);
                });
            }
        });

        debugf("Resetting thread name to %s", initial_thread_name);
        with_log_level(UDIPE_TRACE, {
            set_thread_name(initial_thread_name);
            free(initial_thread_name);
            initial_thread_name = NULL;
        });
    }

#endif  // UDIPE_BUILD_TESTS
