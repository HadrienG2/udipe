#pragma once

//! \file
//! \brief Manipulation of Unix file descriptors
//!
//! This module provides generic utilities for working with file descriptors.

#ifdef __unix__

    #include <udipe/pointer.h>

    #include "error.h"
    #include "log.h"

    #include <errno.h>
    #include <stdint.h>
    #include <unistd.h>


    /// File descriptor (used for clarity over raw int)
    ///
    typedef int fd_t;

    /// Invalid value injected into closed file descriptors to detect errors
    ///
    #define FD_INVALID ((int)-1)

    /// Helper for exchanging 64-bit integers with special file descriptors like
    /// Linux eventfds and timerfds
    ///
    /// Special linux file descriptors commonly exchange data through reads of
    /// writes of 64-bit integers, which are done by exchanging the matching
    /// native-endian stream of bytes with the read() or write() syscalls. This
    /// union can be used to make this operation easier.
    typedef union u64_chars_u {
        uint64_t u64;  ///< Typed payload for high-level interpretation
        char chars[8];  ///< Untyped buffers for read/write syscalls
    } u64_chars_t;

    /// Close a "virtual" file descriptor which does not map into a file on disk
    ///
    /// \param fd must point to an unclosed file descriptor which is not
    ///           associated to an actual file on disk, such as an eventfd or an
    ///           epollfd. It will be set to \ref FD_INVALID after closing and
    ///           must not be used afterwards.
    UDIPE_NON_NULL_ARGS
    static inline
    void close_virtual_fd(fd_t* fd) {
        LOGGED_FUNCTION_START("&%d", *fd)
            ensure_ge(*fd, 0);

            debugf("Closing virtual file descriptor %d...", *fd);
            const int result = close(*fd);
            if (result == -1) switch(errno) {
            case EINTR:  // Interrupted by a signal
                // On Linux, man 2 close guarantees that fds will be closed even if
                // a signal occurs, on other OSes behavior is unspecified.
                errno = 0;
                #ifndef __linux__
                    warn("Interrupted by a signal, fd may not have been closed properly.");
                #endif
                break;
            case EBADF:   // Invalid file descriptor.
                exit_after_c_error("Called close_virtual_fd() on an invalid fd.");
                break;
            case EIO:     // I/O error occurred during file system access.
            case ENOSPC:  // NFS quota blown by previous write
            case EDQUOT:  // NFS quota blown by previous write
            default:
                exit_after_c_error("These error cases should not be encountered");
            }

            debug("Resetting the fd to ease use-after-close detection...");
            *fd = FD_INVALID;
        LOGGED_FUNCTION_END
    }

#else
    #error "This header should only be used on Unix operating systems."
#endif  // __unix__
