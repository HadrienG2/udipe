#ifdef __linux__

    #include "inpoll.h"

    #include "error.h"

    #include <errno.h>
    #include <sys/epoll.h>


    UDIPE_NODISCARD
    inpoll_t inpoll_initialize() {
        int result = epoll_create1(EPOLL_CLOEXEC);
        if (result >= 0) return result;
        assert(result == -1);
        switch (result) {
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
        case EINVAL:  // Invalid value specified in flags.
        case ENOMEM:  // There was insufficient memory to create the kernel object.
        default:
            exit_after_c_error("This error is not expected to happen");
        }
    }

#endif  // __linux__
