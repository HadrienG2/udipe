#pragma once

//! \file
//! \brief "Input polling" file descriptor (currently Linux-only)
//!
//! This code module implements \ref inpoll_t, a highly simplified interface
//! over the `epoll` Linux syscall family which retains just the core features
//! of statefully polling and propagating file descriptor readability signals.
//! See the documentation of this type for more information.

#ifdef __linux__

    #include <udipe/duration.h>
    #include <udipe/nodiscard.h>
    #include <udipe/pointer.h>

    #include "fd.h"

    #include <stddef.h>
    #include <stdint.h>


    /// "Input polling" file descriptor (currently Linux-only)
    ///
    /// This is a special file descriptor which can be dynamically attached to
    /// and detached from a set of other "upstream" file descriptors, each of
    /// which is given a user-defined numerical identifier. It ensures that...
    ///
    /// - When any of the upstream file descriptors is marked as readable, this
    ///   file descriptor is marked as readable too, otherwise it is not marked
    ///   as readable.
    /// - You can wait for at least one of the upstream file descriptors to
    ///   become readable and get back a bounded list of upstream file
    ///   descriptors that are currently readable, designated using the
    ///   aforementioned numerical identifiers.
    ///
    /// It is currently only implemented on Linux, using the `epoll` system call
    /// family designed for this purpose. While it could be emulated on other
    /// operating systems, that would come at the expense of significant
    /// overhead, so the preferred strategy is to instead use the host OS'
    /// idiomatic way to multiplex several synchronization objects into one
    /// (such as WaitForMultipleObjects() and RegisterWaitForSingleObject() on
    /// Windows, kqueue on BSDs and macOS...).
    typedef fd_t inpoll_t;

    /// Set up an input polling file descriptor
    ///
    /// After initial setup, you must attach one or more upstream file
    /// descriptors with inpoll_attach(). The input polling file descriptor will
    /// then be marked as readable (as can be observed by running `poll()` in
    /// `POLLIN` mode) when any upstream descriptor is marked as such.
    ///
    /// You may then wait for one or more of the upstream file descriptors to
    /// become readable, and grab a bounded-length list of descriptors that did
    /// become readable, using inpoll_wait().
    ///
    /// Eventually, you must destroy the input polling file descriptor using
    /// inpoll_finalize().
    ///
    /// \returns an input polling file descriptor that must later be destroyed
    ///          using inpoll_finalize().
    UDIPE_NODISCARD
    inpoll_t inpoll_initialize();

    /// Result of inpoll_attach(), describing success or a non-fatal error
    ///
    /// At the time of writing, the following errors are considered fatal
    /// because they are either blatant usage errors or very hard to recover
    /// from. When encountered, these errors will therefore lead to program exit
    /// instead of returning an error code.
    ///
    /// - `poll` or `upstream_fd` is not a valid file descriptor
    /// - `poll` is not an `epollfd`
    /// - `upstream_fd` does not support `epoll` (e.g. it is a file descriptor
    ///   associated with a regular file or a directory)
    /// - `upstream_fd` was registered twice on the same `poll`
    /// - `upstream_fd` designates the same `epollfd` as `poll`
    /// - The system does not have enough memory to process this information
    /// - The global /proc/sys/fs/epoll/max_user_watches limit on epoll watches
    ///   (see man 7 epoll) was reached
    typedef enum inpoll_attach_result_e {
        /// Successfully attached `upstream_fd` to `poll`
        ///
        INPOLL_ATTACH_SUCCESS = 0,

        /// Failed to attach `upstream_fd` due to excessive `epollfd` nesting
        ///
        /// If `upstream_fd` is an `epollfd`, it can only be attached to another
        /// `epollfd` like those wrapped by \ref inpoll_t if...
        ///
        /// - It does not result in cyclic attachments where two `epollfd`s end
        ///   up monitoring each other.
        /// - It does not result in an `epollfd` nesting depth greater than 5.
        ///
        /// We unfortunately cannot differentiate these two cases, but since
        /// inpoll is currently only used to implement futures and futures
        /// cannot be dynamically attached to other futures after creation, we
        /// can assume it is the latter problem.
        ///
        /// This error is non-fatal because the intent is to eventually handle
        /// it by switching to an slower thread-based join/unordered future
        /// implementation, taking inspiration from the Windows version.
        INPOLL_ATTACH_TOO_NESTED,

        // WARNING: As this is an internal API, more variants may be added
        //          without advance notice and code that uses inpoll_attach()
        //          will need be adapted accordingly.
    } inpoll_attach_result_t;

    /// Attach an extra upstream file descriptor to an \ref inpoll_t.
    ///
    /// If this operation completes successfully, the input polling descriptor
    /// will start being marked as readable once `upstream_fd` is marked as
    /// such, and inpoll_wait() will start reporting when it is marked as
    /// readable using the specified `identifier`. This will continue until
    /// `upstream_fd` is detached using inpoll_detach() or the inpoll file
    /// descriptor is destroyed with inpoll_finalize().
    ///
    /// This operation can fail for many reasons. See \ref
    /// inpoll_attach_result_t for more information about all possible failure
    /// modes including those that are non-fatal and must be handled on your
    /// side (the other ones will lead the host process to exit instantly).
    ///
    /// If this operation succeeds, the file descriptor `upstream_fd` will be
    /// associated with the user-chosen `identifier`, which will be used to
    /// report its readiness in inpoll_wait(). This identifier can be used to
    /// add a layer of indirection, like an index into a table of structs
    /// wrapping file descriptors with extra information. But if you don't need
    /// this you can just use `upstream_fd` as an identifier. Be sure not to use
    /// the same `identifier` for two different `upstream_fd`s in any case, as
    /// otherwise you will not be able to differentiate them in the output of
    /// inpoll_wait().
    ///
    /// \param poll must be an input polling file descriptor that was set up
    ///             with inpoll_initialize() and wasn't destroyed with
    ///             inpoll_finalize() yet.
    /// \param upstream_fd must be a valid file descriptor which supports epoll
    ///                    in `EPOLLIN` mode. Prominent examples at the time of
    ///                    writing include `eventfd`, `timerfd` and `epollfd`,
    ///                    the latter being subjected to the limitations
    ///                    discussed in the docs of \ref inpoll_attach_result_t.
    /// \param identifier is a numerical identifier of your choosing that is
    ///                   used to designate `upstream_fd` in the output of
    ///                   inpoll_wait().
    ///
    /// \returns an \ref inpoll_attach_result_t that indicates whether the
    ///          operation succeeded or failed for a non-fatal reason. You must
    ///          handle all the non-fatal failure modes, which is why this
    ///          function is annotated with \ref UDIPE_NODISCARD.
    UDIPE_NODISCARD
    inpoll_attach_result_t inpoll_attach(inpoll_t poll,
                                         fd_t upstream_fd,
                                         uint64_t identifier);

    /// Detach an upstream file descriptor from an \ref inpoll_t
    ///
    /// After calling this function, the input polling descriptor will stop
    /// being marked as readable once `upstream_fd` is, and inpoll_wait() will
    /// stop reporting the readability of this file descriptor.
    ///
    /// \param poll must be an input polling file descriptor that was set up
    ///             with inpoll_initialize() and wasn't destroyed with
    ///             inpoll_finalize() yet.
    /// \param upstream_fd must be a valid file descriptor that was previously
    ///                    attached to this \ref inpoll_t with inpoll_attach()
    ///                    and wasn't detached since.
    void inpoll_detach(inpoll_t poll, fd_t upstream_fd);

    /// Wait for at least one of the previously attached upstream file
    /// descriptors to become readable, and report a bounded list of them
    ///
    /// \param poll must be an input polling file descriptor that was set up
    ///             with inpoll_initialize() and wasn't destroyed with
    ///             inpoll_finalize() yet.
    /// \param identifiers is a user-provided array that will be filled up with
    ///                    the identifiers of the file descriptors that did
    ///                    become readable, as specified by previous calls to
    ///                    inpoll_attach().
    /// \param num_identifiers provides a lower bound on the storage capacity of
    ///                        `identifiers`, which should be at least 1. If
    ///                        more file descriptors turn out to be readable,
    ///                        this function will only report an arbitrarily
    ///                        subset of `num_identifiers` of them.
    /// \param timeout indicates after how much time the active thread should
    ///                stop waiting and report zero readable file descriptors.
    ///                Beware that the actually applied timeout will be slightly
    ///                longer due to scheduling delays and OS clock granularity.
    ///
    /// \returns the number of file descriptors that became readable during the
    ///          wait (or were already readable to begin with), matching the
    ///          number of entries that were filled in the `identifiers` array.
    ///          If `timeout` is \ref UDIPE_DURATION_MAX, this number is
    ///          guaranteed to be at least one.
    UDIPE_NODISCARD
    size_t inpoll_wait(inpoll_t poll,
                       uint64_t identifiers[],
                       size_t num_identifiers,
                       udipe_duration_ns_t timeout);

    /// Destroy an input polling file descriptor
    ///
    /// \param poll must point to an input polling file descriptor that was set
    ///             up with inpoll_initialize() and wasn't destroyed with
    ///             inpoll_finalize() yet. It cannot be used again after calling
    ///             this function, and will be set to FD_INVALID accordingly.
    UDIPE_NON_NULL_ARGS
    void inpoll_finalize(inpoll_t* poll);


    // TODO: Unit tests

#else
    #error "This header is currently only implemented on Linux."
#endif  // __linux__