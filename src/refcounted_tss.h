#pragma once

//! \file
//! \brief Reference-counted `tss_t`
//!
//! This code module implements \ref refcounted_tss_t, a wrapper around `tss_t`
//! that aims to work around the dubious ergonomics of the tracherous
//! `tss_delete()` function.

#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>


/// Reference-counted `tss_t`
///
/// In the C11 concurrency support library, `tss_t` is provided as a portable
/// equivalent of `pthread_key_` functions, enabling you to dynamically allocate
/// thread-local storage slots whenever a thread first accesses a thread-local
/// variable and schedule the automatic execution of a destructor function when
/// said thread later exits.
///
/// What `tss_t` does not provide, unfortunately, is a satisfactory answer to
/// the question of when it is safe to call `tss_delete()`. As Unix
/// implementations are likely to be based on `pthread_key_delete()`, it seems
/// safe to assume that they may have the same limitations, and on the POSIX
/// side it is clearly specified that if you call `pthread_key_delete()` before
/// all destructors have started running, the remaining destructors won't run.
///
/// This is a problem because it means that when the time comes to destroy a
/// particular `tss_t`, a choice must seemingly be made between two bad options:
///
/// 1. Destroy the `tss_t` immediately, accepting the fact that some
///    thread-local storage slots may not be liberated. This will leak some
///    thread-local resources, and even if we consider that fine assuming the OS
///    will clean up after ourselves after process exit, resource leak detection
///    tooling will rightfully yell at us.
/// 2. Do not destroy the `tss_t`, ensuring that the destructors of individual
///    thread-local storage slots will all run at the expense of leaking the
///    `tss_t` object itself.
///
/// Thankfully, there is a third way, which the POSIX specification mentions in
/// passing while leaving implementation as an exercise to the reader: track a
/// reference count of how many threads have allocated a storage slot associated
/// with a particular `tss_t` key, decrement this refcount whenever a thread's
/// destructor runs, and destroy the `tss_t` once the refcount has reached 0
/// **and** we are at a point where no further uses of the `tss_t` by other
/// threads should occur.
///
/// This struct implements the associated logic in a reusable package.
typedef struct refcounted_tss_s {
    /// Thread-specific storage key
    ///
    /// Should not be accessed directly, instead use the
    /// refcounted_tss_acquire() function.
    tss_t key;

    /// Reference count and reachability tracking
    ///
    /// The ability for threads to acquire new references to `key` is tracked by
    /// the high-order bit of this integer word (\ref REFCOUNTED_TSS_REACHABLE).
    /// Once this bit is cleared and the reference count has reached 0, `key`
    /// can safely be destroyed with `tss_delete()`.
    atomic_size_t refcount_and_reachability;
} refcounted_tss_t;

/// Bit of \ref refcounted_tss_t::refcount_and_reachability that tracks whether
/// new references to the underlying `tss_t` can still be acquired
///
/// Once this bit gets cleared, \ref refcounted_tss_t will wait for all
/// remaining scheduled thread destructors to run, then destroy the `tss_t`.
#define REFCOUNTED_TSS_REACHABLE ((size_t)1 << (sizeof(size_t) * 8 - 1))

/// Set up a reference-counted `tss_t`
///
/// As with `tss_create()`, a destructor function must be specified, which will
/// be called whenever a thread that had an entry associated with this `tss_t`
/// exits. This destructor must call refcounted_tss_release() on this \ref
/// refcounted_tss_t object at the end of its execution, which implies that
/// 1/the thread must somehow retain a reference to it, and 2/unlike in raw
/// `tss_create()` API calls, specifying a destructor function is mandatory.
///
/// This function may be used within the logger implementation and will
/// therefore not perform any logging.
///
/// \param destructor will be called whenever a thread-local storage slot is
///                   destroyed on thread exit, and is responsible for calling
///                   back refcounted_tss_release() on this \ref
///                   refcounted_tss_t object at the end. As this function is
///                   called when an arbitrary thread exits, it must not assume
///                   the availability of udipe logging facilities. If it wants
///                   logging, it must set it up itself via a previously
///                   retained reference to a still-valid udipe context.
///
/// \returns a reference-counted `tss_t` on which refcounted_tss_acquire() can
///          subsequently be called to set up or access new TLS slots, and
///          refcounted_tss_mark_unreachable() must be eventually called at a
///          point where it is expected that no thread will ever call this
///          function again.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
refcounted_tss_t refcounted_tss_initialize(tss_dtor_t destructor);

/// Constructor for one storage slot of a \ref refcounted_tss_t
///
/// This constructor will be called when a thread calls refcounted_tss_acquire()
/// for the first time on a particular \ref refcounted_tss_t. It is in charge of
/// somehow allocating the associated thread-local data structure and returning
/// a pointer to it.
///
/// Its argument is a user-chosen context struct pointer, which can be used to
/// forward context information to the constructor such as a stable pointer to
/// the \ref refcounted_tss_t.
typedef void*(*refcounted_tss_ctor_t)(void* /* context */);

/// Create a thread-local storage slot for the current thread
///
/// This function is the cold path of refcounted_tss_acquire() where a storage
/// slot is created. It must not be called directly. See
/// refcounted_tss_acquire() for more info on the parameters and return value.
UDIPE_NODISCARD
UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2)
void* refcounted_tss_create_slot(refcounted_tss_t* tss,
                                 refcounted_tss_ctor_t constructor,
                                 void* constructor_context);

/// Access a thread-local storage slot, creating it if it does not exist
///
/// If the current thread already has a thread-local storage slot associated
/// with this `tss_t`, then this will return the pointer to the associated data
/// structure. Otherwise it will call the user-specified `constructor` with the
/// associated `constructor_context` in order to build such a data structure,
/// then eventually return the associated pointer after installing it in TLS.
///
/// Multiple threads can safely call this function concurrently with itself and
/// with refcounted_tss_release(), however it is not safe to call this function
/// and refcounted_tss_mark_unreachable() concurrently.
///
/// This function is used within the logger implementation and will therefore
/// not perform any logging.
///
/// \param tss must be a \ref refcounted_tss_t that was set up with
///            refcounted_tss_initialize() and on which
///            refcounted_tss_mark_unreachable() has not been called yet.
/// \param constructor is the constructor function that will be called if this
///                    is the first time refcounted_tss_acquire() is called on
///                    this `tss` by this `thread`. It takes `context` as a
///                    parameter, and returns a pointer to the associated
///                    thread-local data structure, that will eventually be
///                    destroyed by the previously specified `destructor`. It
///                    must not make any logging calls unless this function is
///                    called within the scope of with_logger().
/// \param constructor_context can be used to pass arbitrary context information
///                            (e.g. a pointer to the `tss`) to the storage slot
///                            constructor. If no such context information is
///                            needed, it can also be set to `NULL`.
///
/// \returns a pointer to the freshly or previously initialized thread-local
///          data structure.
static inline
UDIPE_NODISCARD
UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2)
void* refcounted_tss_acquire(refcounted_tss_t* tss,
                             refcounted_tss_ctor_t constructor,
                             void* constructor_context) {
    // WARNING: This function is called by the logger implementation and must
    //          therefore not perform any logging. Normal events and non-fatal
    //          errors should not be signaled at all, fatal errors should be
    //          signalled on stderr before exiting.

    // Make sure the `tss_t` has not been marked as unreachable in debug builds
    assert(("Should not be called on an unreachable refounted_tss_t",
            atomic_load_explicit(&tss->refcount_and_reachability,
                                 memory_order_relaxed)
            | REFCOUNTED_TSS_REACHABLE));

    // Attempt to retrieve a pre-existing thread-local variable
    void* const result = tss_get(tss->key);
    if (result) {
        return result;
    } else {
        return refcounted_tss_create_slot(tss,
                                          constructor,
                                          constructor_context);
    }
}

/// Overwrite a pre-existing thread-local storage slot
///
/// This function can only be called after calling refcounted_tss_acquire() on
/// the same thread to ensure the existence of a TLS storage slot.
///
/// It will replace the pointer held within said storage slot with the specified
/// new pointer. The TLS destructor will not be called, you are responsible for
/// ensuring that any former allocation targeted by the previous pointer has
/// been liberated, which is another reason why you need to call
/// refcounted_tss_acquire() first.
///
/// This function is used within the logger implementation and will therefore
/// not perform any logging.
///
/// \param tss must be a \ref refcounted_tss_t that was set up with
///            refcounted_tss_initialize(), within which the current thread has
///            set up a storage slot with refcounted_tss_acquire(), and on which
///            refcounted_tss_mark_unreachable() has not been called yet.
/// \param new is the new value that will be stored inside of the active
///            thread's storage slot, overwriting any previous value without
///            liberating associated storage. It cannot be `NULL`.
static inline
UDIPE_NON_NULL_ARGS
void refcounted_tss_write(refcounted_tss_t* tss, void* new) {
    // WARNING: This function is called by the logger implementation and must
    //          therefore not perform any logging. Normal events and non-fatal
    //          errors should not be signaled at all, fatal errors should be
    //          signalled on stderr before exiting.

    // Make sure the `tss_t` has not been marked as unreachable in debug builds
    assert(("Should not be called on an unreachable refounted_tss_t",
            atomic_load_explicit(&tss->refcount_and_reachability,
                                 memory_order_relaxed)
            | REFCOUNTED_TSS_REACHABLE));

    // Make sure refcounted_tss_acquire() has been called in debug builds
    assert(("Should have called refcounted_tss_acquire() first",
            tss_get(tss->key)));

    // Replace the current TSS contents
    if (tss_set(tss->key, new) != thrd_success) {
        fprintf(stderr,
                "libudipe: Failed to modify a TLS slot for this thread!\n");
        exit(EXIT_FAILURE);
    }
}

/// Release a thread-local storage slot
///
/// This function **must** be called at the end of the `destructor` that was
/// specified at recounted_tss_initialize() time. It will take care of ensuring
/// that the associated `tss_t` will be destroyed once it has been made
/// unreachable and all threads that have previously acquired a thread-local
/// storage slot have exited.
///
/// This function may be called at a time where no logger is available and will
/// therefore not perform any logging.
///
/// \param tss must be a \ref refcounted_tss_t that was set up with
///            refcounted_tss_initialize().
///
/// \returns the truth that this operation destroyed the last thread-local
///          storage slot of `tss`, which triggered the automatic destruction of
///          the associated `tss_t`. When this happens, if the \ref
///          refcounted_tss_t is heap-allocated, either on its own or as a
///          member of a larger struct, it becomes safe to liberate this
///          allocation as no other thread should reference it anymore.
UDIPE_NON_NULL_ARGS
bool refcounted_tss_release(refcounted_tss_t* tss);

/// Mark a reference-counted `tss_t` as unreachable, preventing the creation of
/// new thread-local storage slots.
///
/// This starts the reference countdown that will eventually result in the inner
/// `tss_t` being liberated. If it turns out that the underlying `tss_t` had no
/// allocated thread-local storage slot at the time where this function is
/// called, then it will be liberated immediately.
///
/// It is safe to call this function and refcounted_tss_release() concurrently,
/// but it is not safe to call it concurrently with refcounted_tss_acquire().
///
/// This function may be called at a time where no logger is available and will
/// therefore not perform any logging.
///
/// \param tss must be a \ref refcounted_tss_t that was set up with
///            refcounted_tss_initialize() and on which
///            refcounted_tss_mark_unreachable() has not been called yet.
///
/// \returns the truth that this operation destroyed the last thread-local
///          storage slot of `tss`, which triggered the automatic destruction of
///          the associated `tss_t`. When this happens, if the \ref
///          refcounted_tss_t is heap-allocated, either on its own or as a
///          member of a larger struct, it becomes safe to liberate this
///          allocation as no other thread should reference it anymore.
UDIPE_NON_NULL_ARGS
bool refcounted_tss_discard(refcounted_tss_t* tss);


// TODO unit tests
