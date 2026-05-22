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
    #include <unistd.h>


    /// File descriptor (used for clarity over raw int)
    ///
    typedef int fd_t;

    /// Invalid value injected into closed file descriptors to detect errors
    ///
    #define FD_INVALID ((int)-1)

    /// Close a "virtual" file descriptor which does not map into a file on disk
    ///
    /// \param fd must point to an unclosed file descriptor which is not
    ///           associated to an actual file on disk. It will be set to \ref
    ///           FD_INVALID after closing and must not be used afterwards.
    UDIPE_NON_NULL_ARGS
    static inline
    void close_virtual_fd(fd_t* fd) {
        ensure_ge(*fd, 0);
        debugf("Closing virtual file descriptor %d...", *fd);
        const int result = close(*fd);
        if (result == -1) switch(errno) {
        case EINTR:  // Interrupted by a signal
            // On Linux, man 2 close guarantees that fds will be closed even if
            // a signal occurs, on other OSes behavior is unspecified.
            #ifndef __linux__
                warn("Interrupted by a signal, fd may not have been closed properly.");
            #endif
            break;
        case EBADF:   // Invalid file descriptor.
        case EIO:     // I/O error occurred during file system access.
        case ENOSPC:  // NFS quota blown by previous write
        case EDQUOT:  // NFS quota blown by previous write
        default:
            exit_after_c_error("These error cases should not be encountered");
        }
        *fd = FD_INVALID;
    }

#else
    #error "This header should only be used on Unix operating systems."
#endif  // __unix__
