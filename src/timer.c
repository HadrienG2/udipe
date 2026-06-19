#include "timer.h"

#include <udipe/nodiscard.h>

#include "error.h"
#include "log.h"
#ifdef __linux__
    #include "fd.h"
#endif

#include <assert.h>
#include <errno.h>
#include <time.h>

#ifdef __linux__
    #include <sys/timerfd.h>
#elif defined(_WIN32)
    // Must be included first
    #include <windows.h>

    #include <handleapi.h>
    #include <synchapi.h>
#endif


#ifdef __linux__
    UDIPE_NODISCARD
    timerfd_t timerfd_initialize(struct timespec initial,
                                 udipe_duration_ns_t interval) {
        int maybe_timerfd = timerfd_create(
            CLOCK_REALTIME,
            // No need for TFD_NONBLOCK as these fds should never be read from
            // until the associated future is destroyed and they are liberated.
            TFD_CLOEXEC
        );
        if (maybe_timerfd == -1) switch(errno) {
        case EMFILE:  // Reached process fd limit
            exit_after_c_error(
                "The number of fds in current process reached the limit. "
                "Consider increasing the limit if possible."
            );
        case ENFILE:  // Reached system fd limit
            exit_after_c_error(
                "The number of fds in the system reached the limit. "
                "Consider increasing the limit if possible."
            );
        case EINVAL:  // clockid or flags is invalid.
        case ENODEV:  // Could not mount (internal) anonymous inode device.
        case ENOMEM:  // Not enough memory to create a new timerfd.
            exit_after_c_error("This error is not expected to happen");
        }
        ensure_ge(maybe_timerfd, 0);
        const fd_t timerfd = maybe_timerfd;
        debugf("Created a timer object with fd %d.", timerfd);

        struct itimerspec spec = { 0 };
        spec.it_value = initial;
        assert(initial.tv_nsec < 1000 * 1000 * 1000);
        spec.it_interval.tv_sec = interval / (1000 * 1000 * 1000);
        spec.it_interval.tv_nsec = interval % (1000 * 1000 * 1000);
        exit_on_negative(
            timerfd_settime(
                timerfd,
                // Don't want to expose TFD_TIMER_CANCEL_ON_SET as it has no
                // clean equivalent on Windows.
                TFD_TIMER_ABSTIME,
                &spec,
                NULL
            ),
            "Failed to configure a timerfd's deadline and period!"
        );
        return timerfd;
    }
#endif  // __linux__

UDIPE_NODISCARD
timer_once_t timer_once_initialize(struct timespec deadline) {
    #ifdef __linux__
        return timerfd_initialize(deadline, 0);
    #elif defined(_WIN32)
        HANDLE handle = CreateWaitableTimerExW(
            NULL,
            NULL,
            // Needed to allow N threads to wait on one future
            CREATE_WAITABLE_TIMER_MANUAL_RESET
                // Needed because otherwise we get ~16ms resolution which is
                // miserable by the standards of high-performance UDP.
                | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            DELETE | SYNCHRONIZE | TIMER_MODIFY_STATE
        );
        win32_exit_on_null(handle,
                           "Failed to create an anonymous timer object!");
        debugf("Set up a timer object with handle %p.", handle);

        const LARGE_INTEGER due = win32_filetime_from_timespec(due, false);
        win32_exit_on_zero(
            SetWaitableTimer(
                handle,
                &due,
                0,
                NULL,
                NULL,
                false
            ),
            "Failed to configure a one-shot timer deadline!"
        );
        return handle;
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif
}
