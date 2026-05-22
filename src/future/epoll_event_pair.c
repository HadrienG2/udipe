#ifdef __linux__

    #define _GNU_SOURCE

    #include "epoll_event_pair.h"

    #include <udipe/nodiscard.h>

    #include "../error.h"
    #include "../event.h"
    #include "../log.h"

    #include <errno.h>
    #include <stdint.h>
    #include <sys/epoll.h>


    UDIPE_NODISCARD
    epoll_event_pair_t epoll_event_pair_initialize() {
        debug("Setting up a coupled epollfd+eventfd pair...");

        debug("Allocating eventfd...");
        const event_t eventfd = event_initialize(false);

        debug("Allocating epollfd...");
        const int maybe_epollfd = epoll_create1(0);
        if (maybe_epollfd == -1) switch(errno) {
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
        case ENOMEM:  // Not enough memory to create a new eventfd.
        default:
            exit_after_c_error("This error is not expected to happen");
        }
        ensure_ge(maybe_epollfd, 0);
        epoll_event_pair_t result = (epoll_event_pair_t){
            .epoll = maybe_epollfd,
            .event = eventfd
        };

        debugf("Binding allocated eventfd %d to allocated epollfd %d...",
               result.event, result.epoll);
        struct epoll_event epoll_event = (struct epoll_event){
            .events = EPOLLIN,
            .data = (epoll_data_t){ .u64 = UINT64_MAX }
        };
        const int ctl_result = epoll_ctl(result.epoll,
                                         EPOLL_CTL_ADD,
                                         eventfd,
                                         &epoll_event);
        if (ctl_result == -1) switch(ctl_result) {
        case ENOSPC:  // Reached /proc/sys/fs/epoll/max_user_watches limit
            exit_after_c_error(
                "Reached the /proc/sys/fs/epoll/max_user_watches limit. "
                "See man 7 epoll. Consider increasing the limit if possible."
            );
        case EBADF:   // epfd or fd is not a valid file descriptor.
        case EEXIST:  // (op is ADD) fd is already registered with this epfd.
        case EINVAL:  // * epfd is not an epollfd.
                      // * fd is the same as epfd.
                      // * Requested op (ADD) is not supported by.
                      // * Invalid event type for EPOLLEXCLUSIVE.
                      // * op is MOD (false) with EPOLLEXCLUSIVE in events.
                      // * op is MOD (false) and EPOLLEXCLUSIVE was already set.
                      // * EPOLLEXCLUSIVE use with an fd that is an epollfd.
        case ELOOP:   // fd is an epollfd and this would result in circular
                      // epoll monitoring or epollfd nesting >5 instances.
        case ENOENT:  // MOD or DEL but fd is not registered.
        case ENOMEM:  // Not enough memory for this control operation.
        case EPERM:   // Target fd does not support epoll.
        default:
            exit_after_c_error("This error is not expected to happen");
        }
        return result;
    }

#endif  // __linux__
