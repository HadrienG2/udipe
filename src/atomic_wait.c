#include "atomic_wait.h"

#include "assert.h"
#include "errno.h"
#include "error.h"
#include "log.h"

#ifdef __linux__
    #include <linux/futex.h>
    #include <sys/syscall.h>
    #include <unistd.h>
#elif defined(_WIN32)
    #include <synchapi.h>
#endif


UDIPE_NON_NULL_ARGS
void udipe_atomic_wait(_Atomic uint32_t* atom, uint32_t expected) {
    tracef("Waiting for the value at address %p to change value from %#x...",
           (void*)atom,
           expected);
    #ifdef __linux__
        long result = syscall(SYS_futex, atom, FUTEX_WAIT, expected, NULL);
        switch (result) {
        case 0:
            trace("Got woken up after waiting (maybe from futex recycling).");
            break;
        case -1:
            switch (errno) {
            case EAGAIN:
                errno = 0;
                trace("Value already differed from expectation, didn't wait.");
                break;
            case EINTR:
                errno = 0;
                trace("Started to wait, but was interrupted by a signal.");
                break;
            // timeout did not point to a valid user-space address.
            case EFAULT:
            // The supplied timeout argument was invalid (tv_sec was less than
            // zero, or tv_nsec was not less than 1,000,000,000).
            case EINVAL:
            // The timeout expired before the operation completed.
            case ETIMEDOUT:
                exit_after_c_error("Shouldn't happen without a timeout!");
            default:
                exit_after_c_error("FUTEX_WAIT errno doesn't match manpage!");
            }
            break;
        default:
            exit_after_c_error("FUTEX_WAIT result doesn't match manpage!");
        }
    #elif defined(_WIN32)
        bool result = WaitOnAddress((volatile VOID*)atom,
                                    (PVOID)(&expected),
                                    4,
                                    INFINITE);
        win32_exit_on_zero(result, "No error expected as there is no timeout");
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
}

UDIPE_NON_NULL_ARGS
void udipe_atomic_notify_all(_Atomic uint32_t* atom) {
    tracef("Signaling all waiters that the value at address %p has changed...",
           (void*)atom);
    #ifdef __linux__
        long result = syscall(SYS_futex, atom, FUTEX_WAIT, UINT32_MAX);
        assert(result == 0 || result == -1);
        exit_on_negative((int)result, "No error expected here");
    #elif defined(_WIN32)
        WakeByAddressAll((PVOID)atom);
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
}

UDIPE_NON_NULL_ARGS
void udipe_atomic_notify_one(_Atomic uint32_t* atom) {
    tracef("Signaling one waiter that the value at address %p has changed...",
           (void*)atom);
    #ifdef __linux__
        long result = syscall(SYS_futex, atom, FUTEX_WAIT, 1);
        assert(result == 0 || result == -1);
        exit_on_negative((int)result, "No error expected here");
    #elif defined(_WIN32)
        WakeByAddressSingle((PVOID)atom);
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
}


#ifdef UDIPE_BUILD_TESTS

    void atomic_wait_unit_tests() {
        // TODO: Write the tests
    }

#endif  // UDIPE_BUILD_TESTS
