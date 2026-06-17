#ifdef __linux__

    #include "inpoll.h"

    #include "duration.h"
    #include "error.h"
    #include "fd.h"
    #include "log.h"
    #include "stopwatch.h"

    #include <alloca.h>
    #include <assert.h>
    #include <errno.h>
    #include <limits.h>
    #include <stddef.h>
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
        case EEXIST:  // (op is EPOLL_CTL_ADD) fd is already registered.
        case EINVAL:  // - epfd is not an epoll file descriptor.
                      // - fd is the same as epfd.
                      // - requested operation (ADD) is not supported.
                      // - (other errors can only happen with EPOLLEXCLUSIVE)
        case ENOENT:  // (can only happen when op is MOD or DEL)
        case ENOMEM:  // Not enough memory to perform this operation.
        case EPERM:  // The target file fd does not support epoll.
        default:
            exit_after_c_error("This error is not expected to happen!");
        }
    }

    void inpoll_detach(inpoll_t poll, fd_t upstream_fd) {
        const int result = epoll_ctl(poll,
                                     EPOLL_CTL_DEL,
                                     upstream_fd,
                                     NULL);
        if (result == 0) return;
        assert(result == -1);
        switch (errno) {
        case EBADF:  // epfd or fd is not a valid file descriptor.
        case EEXIST:  // (can only oppen when op is ADD).
        case EINVAL:  // - epfd is not an epoll file descriptor.
                      // - fd is the same as epfd.
                      // - requested operation (DEL) is not supported.
                      // - (other errors can only happen with EPOLLEXCLUSIVE)
        case ELOOP:  // (can only happen when op is ADD)
        case ENOENT:  // (op is DEL) fd is not registered with this epollfd.
        case ENOMEM:  // Not enough memory to perform this operation.
        case ENOSPC:  // (can only happen when op is ADD)
        case EPERM:  // The target file fd does not support epoll.
        default:
            exit_after_c_error("This error is not expected to happen!");
        }
    }

    UDIPE_NODISCARD
    size_t inpoll_wait(inpoll_t poll,
                       uint64_t identifiers[],
                       size_t num_identifiers,
                       udipe_duration_ns_t timeout) {
        stopwatch_t stopwatch = stopwatch_initialize();
        ensure_ge(num_identifiers, (size_t)0);
        ensure_le(num_identifiers, (size_t)INT_MAX);

        trace("Setting up events storage...");
        struct epoll_event* events = (struct epoll_event*)alloca(
            num_identifiers * sizeof(struct epoll_event)
        );
        assert(events);

        size_t num_valid_identifiers;
        do {
            struct timespec delay;
            struct timespec* pdelay = make_unix_timeout(&delay, timeout);

            trace("Waiting for upstream fds to become readable...");
            int result = epoll_pwait2(poll,
                                      events,
                                      num_identifiers,
                                      pdelay,
                                      NULL);

            if (result > 0) {
                const size_t num_valid_identifiers = (size_t)result;
                tracef("epoll_pwait2() reported %zu readability events on "
                       "attached upstream fds. Will now translate them into a "
                       "simplified identifier list...",
                       num_valid_identifiers);
                ensure_le(num_valid_identifiers, num_identifiers);
                for (size_t i = 0; i < num_valid_identifiers; ++i) {
                    assert(events[i].events == EPOLLIN);
                    identifiers[i] = events[i].data.u64;
                }
                return num_valid_identifiers;
            }
            assert(result == -1);

            switch(errno) {
            case EINTR:  // The call was interrupted by a signal
                trace("Interrupted by a signal, updating timeout...");
                const udipe_duration_ns_t elapsed_time =
                    stopwatch_measure(&stopwatch);
                if (elapsed_time >= timeout) {
                    trace("Reached timeout before the epollfd became readable!");
                    return (size_t)0;
                } else {
                    timeout -= elapsed_time;
                    continue;
                }
            case EBADF:  // epfd is not a valid file descriptor.
            case EFAULT:  // events is not writable.
            case EINVAL:  // epfd is not an epollfd or n <= 0.
            default:
                exit_after_c_error("This error is not expected to happen!");
            }
        } while (true);
    }

    UDIPE_NON_NULL_ARGS
    void inpoll_finalize(inpoll_t* poll) {
        close_virtual_fd(poll);
    }

#endif  // __linux__
