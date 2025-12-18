#pragma once

//! \file
//! \brief OS-independent atomic wait/notify primitives
//!
//! Where lock-free code needs to interact with blocking code, it is useful to
//! have access to "compare-and-wait" blocking synchronization primitives like
//! SYS_futex on Linux and WaitOnAddress/WakeByAddress on Windows.
//!
//! This module exposes the OS-independent subset of these primitives. As the
//! Windows primitives are much more limited than to the Linux ones, our subset
//! API lands much closer to the Windows API, which is why we borrowed the
//! Windows names for it.

#include <udipe/pointer.h>
#include <udipe/time.h>

#include <stdatomic.h>
#include <stdint.h>


/// Wait for a 32-bit integer to change, may wake up spuriously
///
/// ## Basic contract
///
/// This function begins by checking if `atom` currently has value `expected`.
/// If not, it returns `true` immediately without any further processing.
///
/// If `atom` does have the expected value, then the calling thread immediately
/// starts to wait until one of the following happens:
///
/// - wake_by_address_all() is called on the same address by another thread
///   within the same process.
/// - wake_by_address_single() is called and the OS scheduler decides to wake
///   up this thread among any other waiters.
/// - The specified `timeout` elapses without either of the above happening.
/// - The thread is spuriously awoken for an unrelated reason, for example on
///   Unices this can happen when the process receives a Unix signal.
///
/// Checking and waiting is performed as a single atomic transaction, in the
/// sense that value changes will be detected until the thread is ready to
/// receive notifications from `wake_by_address` methods, and cause the switch
/// to the waiting state to be aborted. This ensures absence of lost wakeup.
///
/// This function must be called within the scope of with_logger().
///
/// \param atom is the atomic variable used to synchronize threads
/// \param expected is the value that this variable is initially expected to
///                 have, if this is true the active thread will block.
/// \param timeout indicates after which duration the active thread should give
///                up on waiting. Beware that this duration may be rounded up to
///                the next multiple of the OS clock granularity.
///
/// \returns - `true` if the thread **could** have been awakened by to a value
///            change or notification from a `wake_by_address` function
///          - `false` if we know for sure that the thread woke up for another
///            reason (timeout, Unix signal...).
///
/// ## Usage guidance
///
/// In situations where blocking code must interact with lock-free code, this
/// function can be used to replace CPU-wasting spin loops with more efficient
/// blocking synchronization on the blocking thread side.
///
/// Here is a basic valid usage pattern:
///
/// - Thread Waiter is waiting for thread Notifier to do something, and they
///   both share `atom`, which is known to initially have value `expected`.
/// - Waiter enters a loop where it repeatedly loads the value of `atom`, exits
///   the loop once this value is not `expected` anymore (typically with a
///   `memory_order_acquire` thread fence), and otherwise calls
///   `wait_on_address()` and loops back.
/// - Notifier changes the value of `atom` once done, typically with
///   `memory_order_release`, then calls some variant of `wake_by_address` as
///   appropriate in order to wake up Waiter if it's waiting.
/// - Until Waiter has somehow acknowledged that it has observed the new value
///   of `atom`, no other thread is allowed to change the value of `atom` (in
///   this basic algorithm), move it around in memory or deallocate the storage
///   block that contains it.
///
/// For more advanced use cases, consider the following variations of the basic
/// algorithm outlined above:
///
/// - It is actually possible to change the value of `atom` again between the
///   moment where Notifier signals the event and the moment where Waiter
///   acknowledges that it has received Notifier's signal, provided that Waiter
///   is able to correctly interpret the new `atom` value even if it has not
///   observed the previous value. In other words...
///     * State machines with more than 2 states can go through as many state
///       changes as they like, as long as they don't go back to the initial
///       state until Waiter's acknowledgement is received.
///     * Counter-based algorithms can work as long as counter wraparound is
///       managed correctly. In particular the counter must not wrap back to its
///       initial value before Waiter has had the time to observe the switch
///       away from this initial value.
/// - If you expect Notifier to frequently outpace Waiter and finish its work
///   before Waiter has started waiting, you can spare Notifier some system
///   calls at the expense of performing more read-modify-write atomic
///   operations overall by applying the following tweaks to the basic
///   algorithm:
///     * `atom` can now have three states INITIAL, WAITING and FINISHED, where
///       INITIAL expectedly denotes its initial state.
///     * After the initial status load which checks if `atom` is FINISHED
///       already, Waiter begins its wait by using compare-and-swap to switch
///       `atom` from INITIAL to WAITING. Compare-and-swap lets it detect if a
///       concurrent switch to FINISHED occured, if not the usual loop will
///       begin in order to await a switch from WAITING to FINISHED.
///     * Notifier signals the end of its work by swapping the value of `atom`
///       with FINISHED, thus detecting if `atom` was INITIAL or WAITING. If it
///       was INITIAL, the notification syscall can be elided, because we know
///       that Waiter hasn't entered the waiting state and will not enter it as
///       `atom` is now FINISHED.
///
/// ## Choice of notification function
///
/// Generally speaking, wake_by_address_single() is harder to use correctly
/// than wake_by_address_all() because it creates several new avenues for
/// synchronization bugs :
///
/// - It is easy to write code that seems to work correctly under low
///   application load, where at most one thread waits on any particular atomic
///   variable at any point in time, but turns out to incorrectly leave threads
///   stuck in the waiting state under higher application load where multiple
///   threads are waiting for a particular variable.
/// - Even when such a bug is not present initially because only one thread is
///   waiting, a later code refactor can introduce multiple waiting threads and
///   thus create such a synchronization bug.
/// - It is also easy to accidentally form expectations about which of the
///   waiting threads will be awoken by wake_by_address_single(), e.g. expect
///   that it is that the first thread that started waiting, but those
///   expectations may only be valid on one particular operating system or only
///   be valid when particular conditions are true (e.g. all threads have the
///   same priority).
/// - On some platforms, wake_by_address_single() is just an alias to
///   wake_by_address_all(). If you are unlucky enough to do your regular
///   development tests on one of those, you may not notice the bugs until
///   fairly late in the development and deployment process.
///
/// Furthermore, it has been proved through benchmarking on common operating
/// systems that contrary to popular belief, wake_by_address_all() is no
/// slower than wake_by_address_single() when only one thread is waiting for
/// the atomic variable of interest. So that is not an argument for using one
/// over the other.
///
/// For all these reasons, wake_by_address_all() should be used by default,
/// and wake_by_address_single() should only be introduced as a performance
/// optimization in situations where releasing all threads at once creates a
/// "thundering herd" situation where all threads proceed to immediately put
/// pressure on a limited or serialized resource like a mutex or an I/O device.
UDIPE_NON_NULL_ARGS
bool wait_on_address(_Atomic uint32_t* atom,
                     uint32_t expected,
                     udipe_duration_ns_t timeout);

/// Notify all threads currently waiting for `atom`'s value to change
///
/// This function should be called after changing the value of `atom`, typically
/// with `memory_order_release`.
///
/// It is the notification function that you should use by default, unless you
/// know your performance can benefit from the finer-grained semantics of
/// wake_by_address_single(), and you have set up rigorous testing on multiple
/// operating systems and with varying load levels to ensure that your code is
/// still correct under those semantics.
///
/// See the documentation of wait_on_address() for a broader overview of
/// atomic wait synchronization and intended usage.
///
/// \param atom is the atomic variable used to synchronize threads
UDIPE_NON_NULL_ARGS
void wake_by_address_all(_Atomic uint32_t* atom);

/// Notify at least one of the threads currently waiting for `atom`'s value to
/// change
///
/// This function should be called after changing the value of `atom`, typically
/// with `memory_order_release`.
///
/// It can be used as an optimized version of wake_by_address_all() in
/// situations where waking up all the threads would result in a "thundering
/// herd" performance problem. But it may be implemented as an alias to
/// wake_by_address_all() on some platforms, therefore its "wake one thread"
/// semantics should not be relied on for correctness.
///
/// See the documentation of wait_on_address() for a broader overview of
/// atomic wait synchronization and intended usage.
///
/// \param atom is the atomic variable used to synchronize threads
UDIPE_NON_NULL_ARGS
void wake_by_address_single(_Atomic uint32_t* atom);


#ifdef UDIPE_BUILD_TESTS
    /// Unit tests
    ///
    /// This function runs all the unit tests for this module. It must be called
    /// within the scope of with_logger().
    void address_wait_unit_tests();
#endif
