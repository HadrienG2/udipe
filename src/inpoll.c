#ifdef __linux__

    #include "inpoll.h"

    #include "error.h"
    #include "fd.h"
    #include "log.h"

    #include <assert.h>
    #include <errno.h>
    #include <stdint.h>
    #include <sys/epoll.h>


    UDIPE_NODISCARD
    inpoll_t inpoll_initialize() {
        const int result = epoll_create1(EPOLL_CLOEXEC);
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

    UDIPE_NODISCARD
    inpoll_attach_result_t inpoll_attach(inpoll_t poll,
                                         fd_t upstream_fd,
                                         uint64_t identifier) {
        struct epoll_event event = (struct epoll_event){
            // Review of input flags as of June 2026:
            // - EPOLLET is not appropriate because we don't want to read from
            //   signaling-oriented fds like eventfds, only to know when the
            //   event of interest has occured. So behavior that stops signaling
            //   until the next read is undesirable.
            // - EPOLLONESHOT is not appropriate because some futures may need
            //   to be polled multiple times before they reach completion,
            //   specifically join futures.
            // - EPOLLWAKEUP is not appropriate because we don't intend to put
            //   the DAQ machine to sleep.
            // - EPOLLEXCLUSIVE is not appropriate because we do want a future
            //   readiness notification to propagate to all waiting threads,
            //   whether they are waiting via epoll or another mechanism.
            .events = EPOLLIN,
            .data = (union epoll_data){ .u64 = identifier },
        };
        const int result = epoll_ctl(poll,
                                     EPOLL_CTL_ADD,
                                     upstream_fd,
                                     &event);
        if (result == 0) return INPOLL_ATTACH_SUCCESS;
        assert(result == -1);
        switch (errno) {
        case ELOOP:  // fd is an epollfd and this attachment would result in an
                     // epoll loop or in an epoll nesting depth greater than 5
            warn("Encountered an epoll loop or excessive epoll nesting, "
                 "will assume it is excessive nesting...");
            return INPOLL_ATTACH_TOO_NESTED;
        case ENOSPC: // Reached max_user_watches kernel limit
            exit_after_c_error(
                "The number of epoll attachments in the system reached the limit. "
                "Consider increasing the limit if possible."
            );
        case EBADF:  // epfd or fd is not a valid file descriptor.
        case EEXIST:  // op was EPOLL_CTL_ADD, and fd is already registered.
        case EINVAL:  // - epfd is not an epoll file descriptor.
                      // - fd is the same as epfd.
                      // - requested operation (ADD) is not supported.
                      // - (other errors can only happen with EPOLLEXCLUSIVE)
        case ENOENT:  // (can only happen with op of MOD or DEL)
        case ENOMEM:  // Not enough memory to perform this operation.
        case EPERM:  // The target file fd does not support epoll.
        default:
            exit_after_c_error("This error is not expected to happen!");
        }
    }

#endif  // __linux__
