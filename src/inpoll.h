#pragma once

//! \file
//! \brief "Input polling" special file descriptor
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


    /// "Input polling" special file descriptor (currently Linux-only)
    ///
    /// This is a special file descriptor which can be dynamically attached to
    /// and detached from a set of other "upstream" file descriptors, each of
    /// which gets an associated numerical identifier. It ensures that...
    ///
    /// - When any of the upstream file descriptors is marked as readable, this
    ///   file descriptor is marked as readable too, otherwise it is not marked
    ///   as readable.
    /// - You can wait for at least one of the upstream file descriptors to
    ///   become readable and get back a bounded list of upstream file
    ///   descriptors that are currently readable, identified using the
    ///   aforementioned numbers.
    ///
    /// It is currently only implemented on Linux, using the `epoll` system call
    /// family designed for this purpose. While it could be emulated on other
    /// operating systems, that could come at the expense of significant
    /// overhead, so using the host OS' idiomatic way to multiplex several
    /// synchronization objects into one is more advisable.
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
    // TODO implement, epoll_create1 with EPOLL_CLOEXEC
    UDIPE_NODISCARD
    inpoll_t inpoll_initialize();

    /// Attach an extra upstream file descriptor to an \ref inpoll_t.
    ///
    /// The file descriptor `upstream_fd` will be associated with the
    /// user-chosen `identifier`, which will be used to report its readiness in
    /// `inpoll_wait`. This identifier can be used to add a layer of
    /// indirection, like an index into a table of struct wrapping file
    /// descriptors with extra information. But if you don't need that you can
    /// just use the positive upstream fd number as an identifier.
    ///
    /// After this operation successfully completes, the input polling
    /// descriptor will start being marked as readable once `upstream_fd` is
    /// marked as such, and inpoll_wait() will start reporting when it is marked
    /// as readable using the specified `identifier`.
    ///
    /// This operation can fail for many reasons however:
    ///
    /// - `poll` must be a valid \ref inpoll_t that was created with
    ///   inpoll_initialize() and wasn't destroyed with inpoll_finalize() yet.
    /// - `upstream_fd` must be a valid file descriptor of a type that supports
    ///   epoll. See https://darkcoding.net/software/linux-what-can-you-epoll/
    ///   for a fairly exhaustive list of those as of 2023.
    /// - `upstream_fd` cannot be attached twice to the same `poll` and should
    ///   not be attached with the same `identifier` as another upstream file
    ///   descriptor (but this last error may not be detected).
    /// - If `upstream_fd` is itself an `inpoll_t`, then it must not be the same
    ///   as `poll`, create a polling loop (A is attached to B which is directly
    ///   or indirectly attached to A), or create an attachment chain longer
    ///   than five `inpoll_t`s.
    /// - The limit on epoll watches given by
    ///   /proc/sys/fs/epoll/max_user_watches has been exceeded.
    ///
    /// These errors are currently all handled by exiting the process, but the
    /// hardcoded Linux attachment chain limit is annoying enough that we may
    /// introduce a fallback based on background threads that signal an eventfd
    /// at some point. When that happens, we'll need to make (at least) this
    /// error nonfatal and thus change the API of this function.
    ///
    /// \param poll must be an input polling file descriptor that was set up
    ///             with inpoll_initialize() and wasn't destroyed with
    ///             inpoll_finalize() yet.
    /// TODO finish docs
    //
    // TODO implement, epoll_ctl with EPOLL_CTL_ADD, EPOLLIN, and
    //      none of EPOLLET, EPOLLONESHOT, EPOLLWAKEUP, EPOLLEXCLUSIVE
    UDIPE_NON_NULL_ARGS
    void inpoll_attach(inpoll_t poll, fd_t upstream_fd, uint64_t identifier);

    // TODO docs+implement, epoll_ctl with EPOLL_CTL_DEL and NULL
    UDIPE_NON_NULL_ARGS
    void inpoll_detach(inpoll_t poll, fd_t upstream_fd);

    // TODO docs+implement, epoll_pwait2 with sigmask=NULL and events in an alloca
    UDIPE_NON_NULL_ARGS
    size_t inpoll_wait(inpoll_t poll,
                       uint64_t identifiers[],
                       size_t num_identifiers,
                       udipe_duration_t timeout);

    // TODO docs+implement, close and set to FD_INVALID
    UDIPE_NON_NULL_ARGS
    void inpoll_finalize(inpoll_t* poll);


    // TODO: Unit tests

#else
    #error "This header is currently only implemented on Linux."
#endif  // __linux__