#pragma once

//! \file
//! \brief Future status change synchronization

#include "../event.h"
#include "../timer.h"
#ifdef __linux__
    #include "../fd.h"
    #include "../inpoll.h"
#endif

#include <stdint.h>


#ifdef __linux__
    /// Inpoll identifier that is used when \ref status_sync_t::latched_inpoll
    /// is attached to a single fd besides the latch event
    ///
    /// This pattern is used when \ref inpoll_t is not needed for its ability to
    /// simultaneously await the readiness of multiple file descriptors, but
    /// rather for its ability to hide the true file descriptor that is at the
    /// heart of a future, by way of masking it behind another file descriptor
    /// that's ready whenever the upstream file descriptor is.
    #define INPOLL_SINGLE_UPSTREAM_ID ((uint64_t)0)
#endif  // __linux__

/// Synchronization object signaling future status changes
///
/// On Linux, this is a file descriptor, and it is possible to await future
/// status changes by adding this file descriptor to an \ref inpoll_t's interest
/// list. Before you do so, however, "eager" future types which support more
/// efficient waiting methods will require you to first enable fd-based status
/// notifications via the `notify_event` status flag.
///
/// To make the inpoll-based waiting described above possible and
/// straightforward, the associated file descriptor number is guaranteed to
/// remain constant for the entire lifetime of the future.
//
// TODO: Windows version, based on thread poll waits for NT synchronization
//       objects leading to eventual signaling of an `event`?
///
/// Check the future's \ref future_type_t via \ref future_status_t::type to
/// know more about which variant of this union you are dealing with, then
/// read the associated description for more info.
typedef union status_sync_u {
    /// Event object, used for eager futures
    ///
    /// This synchronization style is used for future types where the
    /// asynchronous operation is processed by a dedicated thread, namely
    /// network and custom operation futures.
    ///
    /// Because these futures can also signal completion via
    /// wake_by_address_all() or even via a mere atomic RMW operation when
    /// no one is waiting for completion yet, event signaling is optional
    /// for these future types and must be explicitly requested by setting
    /// the `notify_event` status flag before awaiting this event object
    /// (and checking that the status word did not switch to a ready state
    /// concurrently).
    ///
    /// When the task's outcome has been filed into the status word, if
    /// `notify_event` is set, the event object will be signaled, which will
    /// mark it as ready for all threads that are monitoring it. These
    /// threads will then proceed to read the outcome in the status word,
    /// completing the synchronization transaction.
    ///
    /// Must be reset and recycled when the future is liberated.
    event_t event;

    /// Timer object, used for \ref TYPE_TIMER_ONCE
    ///
    /// This synchronization object is used for single-shot "timer" futures that
    /// become ready at a certain time.
    ///
    /// On Linux, waiting for readiness must be done using a nondestructive
    /// method that doesn't read from the timerfd (like e.g. select, poll,
    /// epoll/inpoll, io_uring...). This way, the timerfd will stay in the
    /// readable state and thus remain an effective downstream readiness signal.
    ///
    /// On Windows, the timer object is configured in manual reset mode to
    /// achieve the same effect without special precautions.
    ///
    /// When this object becomes ready, \ref OUTCOME_SUCCESS must be signaled by
    /// one of the threads which observes this status to be unset.
    ///
    /// The object must be destroyed when the future is liberated, for now. We
    /// may switch to disarming and recycling if timerfd creation/destruction
    /// ever becomes a bottleneck, but that seems unlikely under correct udipe
    /// API usage given that recuring timerfds seem to cover the main use case
    /// for which one might want to create lots of single-shot timers.
    //
    // TODO: Prove the above assertion through benchmarking and
    //       profiling of real-world workloads.
    udipe_timer_t timer_once;

    #ifdef __linux__
        /// \ref inpoll_t with an attached \ref inpoll_latch_event_t, used for
        /// most lazy future types
        ///
        /// This synchronization style is used for "collective" future types
        /// that await several other futures and for "chained" futures that emit
        /// a chain of successor futures. It uses an \ref inpoll_t that monitors
        /// the readiness of some file descriptors of interest, along with an
        /// additional \ref inpoll_latch_event_t eventfd which signals when this
        /// future has reached its final state.
        ///
        /// How exactly dependencies are awaited depends on the kind of
        /// future that you are dealing with:
        ///
        /// - Joined futures directly attach all upstream fds to
        ///   `latched_inpoll`, using their index in the array of upstream
        ///   futures as an inpoll identifier, while the \ref
        ///   inpoll_latch_event_t uses an identifier of \ref INPOLL_LATCH_ID.
        /// - Unordered futures use a cascaded pair of inpolls. Upstream fds are
        ///   attached to an "inner" `specific.unordered.upstream_inpoll` with
        ///   index-based signaling as before, but no accompanying latch
        ///   eventfd. This "inner" inpoll is in turn attached with identifier
        ///   \ref INPOLL_SINGLE_UPSTREAM_ID to this "outer" `latched_inpoll`,
        ///   which is additionally attached to the \ref inpoll_latch_event_t
        ///   with identifier \ref INPOLL_LATCH_ID as before. This cascading
        ///   inpoll structure makes it possible to later detach the inner
        ///   inpoll and migrate it to the next future in the unordered chain,
        ///   while leaving the original future's `latched_inpoll` with the same
        ///   fd number and in a perpetually signaled state.
        /// - Repeating timers produce a chain of output futures using the same
        ///   trick, except instead of cascading inpolls they simply have one
        ///   outer `latched_inpoll` which is connected to an inner timerfd
        ///   (that performs time-based signaling and can be detached and
        ///   migrated to the next future in the chain) and the usual \ref
        ///   inpoll_latch_event_t (that eventually remains attached to the
        ///   inpoll in a perpetually signaled state to broadcast the
        ///   information that the final outcome is available).
        ///
        /// Because \ref inpoll_t's API design (which is inherited from epoll's)
        /// is not very friendly to multi-threaded use, `inpoll_wait()` on the
        /// inner inpoll requires `lazy_lock` protection, which acts as a mutex
        /// to control access to the \ref inpoll_t and associated future state.
        ///
        /// Whenever `inpoll_wait()` output indicates that a particular upstream
        /// future has underwent a status change or this future has been
        /// canceled, the status word of the upstream future (if any) must be
        /// checked, then the fields of this collective future must be modified
        /// accordingly, and finally any other thread which registered to be
        /// notified of state changes while we were probing `inpoll_wait()` must
        /// be notified via `wake_by_address_`. Once the future outcome is
        /// known, whether successful or unsuccessful, its availability must be
        /// signaled via the dedicated \ref inpoll_latch_event_t if at least one
        /// other future awaited this future, as indicated by
        /// `downstream_count`.
        ///
        /// At least for joined futures, this \ref inpoll_t must be destroyed
        /// along with the host future. There seems to be little point in trying
        /// to recycle inpolls for these futures, because resetting their \ref
        /// inpoll_t requires an arbitrary amount of inpoll_detach() calls, each
        /// of which requires a syscall, then setting up the next inpoll for
        /// another futures also requires an arbitrary amount of inpoll_attach()
        /// calls, each of which also requires a syscall. It is therefore
        /// dubious that \ref inpoll_t allocation/liberation will ever be such a
        /// bottleneck that the extra overhead of recycling (which is high in
        /// the case of inpolls) becomes worthwhile.
        ///
        /// For unordered and periodic timer futures, however, the \ref inpoll_t
        /// only has a small amount of futures attached to it (1 eventfd +
        /// either one \ref inpoll_t for unordered or one timerfd for periodic
        /// timers), and many such inpoll+eventfd pairs may be needed by the
        /// arbitrary many continuation futures that will follow in the chain.
        /// In this case, it is expected that recycling this inpoll along with
        /// its (still-attached) associated \ref inpoll_latch_event_t could be
        /// worthwhile.
        //
        // TODO: Prove the above assertions through benchmarking and
        //       profiling of real-world workloads.
        // TODO: Find an inpoll replacement for Windows. Will most likely be
        //       based on the Win32 thread pool driving an event object
        //       after they receive the right signals from thread pool
        //       timers or NT synchronization objects.
        inpoll_t latched_inpoll;

        /// Catch-all file descriptor type
        ///
        /// Use this union variant in situations where the active file
        /// descriptors doesn't matter, such as when managing attachment of
        /// upstream fds to collective future inpolls.
        //
        // TODO: Figure out if Windows can have this convenience too, I
        //       think that is the case if we use `HANDLE` as the catch-all
        //       type for all Win32 synchronization objects. In that case,
        //       we just need to make the wording less file
        //       descriptor-specific.
        fd_t any;
    #endif
} status_sync_t;
