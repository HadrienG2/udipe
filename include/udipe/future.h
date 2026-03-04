#pragma once

//! \file
//! \brief Asynchronous operation management
//!
//! Asynchronous `libudipe` commands such as udipe_start_connect() do not
//! directly return a result, but instead return a pointer to a \ref
//! udipe_future_t proxy that is later used to wait for the result to come up.
//!
//! Adding this intermediary stage where the command has been submitted to
//! worker threads, but has not been awaited yet, allows you to schedule more
//! commands before you wait for the result of the initial command to come up,
//! and to flexibly and efficiently wait for multiple operations using
//! collective operations such as udipe_join().

#include "pointer.h"
#include "result.h"
#include "time.h"
#include "visibility.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>


/// Asynchronous operation future
///
/// Every asynchronous `libudipe` command (function whose name begins with
/// `udipe_start_`) returns a pointer to a future, which acts as a proxy for the
/// associated asynchronous operation.
///
/// This future **must** later be awaited using udipe_wait() or a collective
/// version thereof, which is the point at which the operation's result or
/// errors will be reported, and associated resources will be liberated.
///
/// After a future has been awaited to completion, the ressources associated
/// with it have been liberated, and it must not be used again.
///
/// The content of a future is an opaque implementation detail of `libudipe`
/// that you should not attempt to read or modify in any way.
//
// TODO: Bring new future docs from mockup.h
typedef struct udipe_future_s udipe_future_t;

/// Wait for the end of an asynchronous operation, collect its result and
/// schedule the liberation of associated resources
///
/// From the point where any thread enters this function, `future` should be
/// treated as liberated and not used in any manner anymore.
///
/// By the time this function returns, it is guaranteed that...
///
/// - The result of the asynchronous operation is known (it is, after all, the
///   return value of this function).
/// - Any input parameter provided via a pointer at the time where the
///   asynchronous operation was started will not be accessed anymore, meaning
///   the memory targeted by such pointers can safely be modified, liberated,
///   etc.
/// - The liberation of any state associated with the asynchronous operation has
///   been at least scheduled (though it may not have happened yet).
///
/// By coupling wait-for-result with resource liberation in this fashion, the
/// API design of udipe_finish() ensures that...
///
/// - Any operation whose result is awaited (which must always be done in order
///   to handle unexpected errors) will also get its associated state liberated.
///   There is no need for a separate liberation step that can easily be
///   forgotten, leading to resource leaks.
/// - It is purposely difficult to read the result of an asynchronous operation
///   from multiple threads, which is discouraged as it is a recipe for data
///   races, complicated resource liberation procedures, and other thread
///   synchronization problems like cache contention.
///
/// ...the price to pay being that timeouts are a bit more cumbersome as they
/// require the use of a separate entry point called udipe_wait().
///
/// \param future must be a future that was returned by an asynchronous function
///               (those whose name begins with `udipe_start_`) and has not been
///               liberated by udipe_finish() or udipe_cancel() since. It will
///               be destroyed by this function and must not be used afterwards.
///
/// \returns the result of the asynchronous operation
//
// TODO: Implement, should probably start with
//       udipe_wait(future, UDIPE_DURATION_MAX)
// TODO: Make sure that even if some input futures have been canceled, this
//       function does not return until the pointer-based inputs of the original
//       async operation are known to be safe from any access and can therefore
//       be modified or liberated by the user.
// TODO: Add attribute warn_unused_result on GCC/clang.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
udipe_result_t udipe_finish(udipe_future_t* future);

/// Wait up to a certain duration for the end of an asynchronous operation
///
/// In contrast with udipe_finish(), which also waits for asynchronous
/// operations to terminate, this function...
///
/// - Does not wait more than `timeout` plus some small processing delay
///   (typically in the microsecond range).
/// - Does not fetch the future's result if the operation does complete.
/// - Does not liberate the resources associated with the future.
/// - Can be concurrently called by several threads, at the expense of reduced
///   performance due to CPU cache contention.
///
/// A typical use case for udipe_wait() is when you want to wait a bit for an
/// asynchronous operation to complete, then either 1/give up and cancel the
/// operation with udipe_cancel() or 2/take care of some periodical background
/// chores then go back to waiting, repeating the process until the wait
/// succeeds and you can finally fetch the result using udipe_finish().
///
/// \param future must be a future that was returned by an asynchronous function
///               (those whose name begins with `udipe_start_`) and has not been
///               liberated by udipe_finish() or udipe_cancel() since.
/// \param timeout must be a timeout in nanoseconds, after which this function
///                will give up and return `false` if the asynchronous operation
///                has not completed yet. Special value `UDIPE_DURATION_MIN` can
///                be used if you just want to non-blockingly check if the
///                operation has completed.
///
/// \returns the truth that the operation associated with `future` has
///          completed. If this function returns `true`, calling udipe_finish()
///          on the same future is guaranteed to return a result immediately
///          without blocking.
//
// TODO: Implement
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
bool udipe_wait(udipe_future_t* future, udipe_duration_ns_t timeout);

/// Cancel an asynchronous operation
///
/// This function notifies the udipe infrastructure that the asynchronous
/// operation associated with a certain future is not desired anymore and should
/// terminate as quickly as possible, avoiding any work that had not begun yet
/// including dependent work scheduled after the target future.
///
/// How much work can be saved in this manner depends on the specifics of the
/// operation you're canceling and how far along it has progressed at the time
/// udipe_cancel() has been called. Though latency and UX considerations will
/// sometimes dictate otherwise, the most resource-efficient way to cancel some
/// work remains obviously to not start the work until you know for sure you are
/// going to need it...
///
/// If the `finish` flag is set, then after canceling the operation, this
/// function additionally waits for the operation to terminate then liberates
/// associated resources as if udipe_finish() were called. If `finish` is not
/// set, then udipe_cancel() returns immediately and must be followed by an
/// udipe_finish() call (possibly preceded by some udipe_wait() if
/// bounded-duration waits are desired) to finish resource cleanup.
///
/// \param future must be a future that was returned by an asynchronous function
///               (those whose name begins with `udipe_start_`) and has not been
///               liberated by udipe_finish() or udipe_cancel() since. If
///               `finish` is set, then the future will be destroyed by this
///               function and must not be used afterwards.
/// \param finish indicates whether udipe_cancel() should wait for the operation
///               to terminate and clean up associated resources, as if
///               udipe_finish() was automatically called afterwards, or solely
///               send a cancelation notification and leave waiting and cleanup
///               to a later manual call to udipe_finish().
//
// TODO: Implement
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
void udipe_cancel(udipe_future_t* future, bool finish);

/// Start waiting for multiple asynchronous operations to terminate, returning a
/// future that will be marked as successfully completed or canceled once all
/// upstream operations have terminated or been canceled.
///
/// This asynchronous operation serves two purposes:
///
/// - It simplifies the API of other asynchronous operations by ensuring that
///   they only need to support being scheduled after one future, rather than an
///   arbitrary number of upstream futures.
/// - It improves the efficiency of scheduling multiple downstream operations
///   after a certain common set of upstream operations.
/// - On some platforms, the implementation of udipe_finish() on the output
///   future may more efficient than that of simply calling udipe_finish() on
///   each input future.
///
/// For related use cases, you may also consider using...
///
/// - udipe_join(), the synchronous version of this function, which is the most
///   efficient way to await multiple asynchronous operations provided by the
///   udipe API.
/// - udipe_start_unordered(), the fine-grained version of this operation which
///   lets you do something as soon as any of the input futures terminates, at
///   the expense of increased overhead.
///
/// \param context must point to an \ref udipe_context_t that has been set up
///                via udipe_initialize() and hasn't been liberated via
///                udipe_finalize() since.
/// \param futures must point to an array of futures that were all returned by
///                an asynchronous function (those whose name begins with
///                `udipe_start_`) and have not been liberated by udipe_finish()
///                or udipe_cancel() since. The output future will retain a
///                pointer to this array, which must therefore not be modified
///                or liberated until the completion of the output future has
///                been awaited via udipe_finish() or udipe_cancel().
/// \param num_futures must match the length of the `futures` array.
///
/// \returns a future that will terminate with an empty result once all input
///          futures have terminated. When this happens, results of `futures`
///          can then be fetched in a non-blocking manner by simply calling
///          udipe_finish() on each of them in a sequence.
//
// TODO: Implement.
// TODO: Make sure that even if some input futures have been canceled, this
//       future is not marked as completed until all input futures have
//       terminated in some fashion. This is needed so that clients know when
//       they can safely modify or liberate the `futures` array.
// TODO: Add attribute warn_unused_result on GCC/clang.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* udipe_start_join(udipe_context_t* context,
                                 udipe_future_t* const futures[],
                                 size_t num_futures);

/// Eagerly wait for multiple asynchronous operations to terminate
///
/// This is the synchronous version of udipe_start_join(), which can be used
/// when you want to wait for multiple asynchronous operations to finish and
/// have nothing else to do meanwhile.
///
/// On some platforms, the implementation of udipe_join() may be more efficient
/// than that of simply calling udipe_finish() sequentially for each input
/// future. But bear in mind that you WILL need to call udipe_finish() on each
/// of the input futures eventually in order to check out their results and
/// liberate associated state. All udipe_join() guarantees is that these calls
/// will be nonblocking, which should greatly reduce their individual overhead.
///
/// \param context must point to an \ref udipe_context_t that has been set up
///                via udipe_initialize() and hasn't been liberated via
///                udipe_finalize() since.
/// \param futures must point to an array of futures that were all returned by
///                an asynchronous function (those whose name begins with
///                `udipe_start_`) and have not been liberated by udipe_finish()
///                or udipe_cancel() since.
/// \param num_futures must match the length of the `futures` array.
static inline
UDIPE_NON_NULL_ARGS
void udipe_join(udipe_context_t* context,
                udipe_future_t* const futures[],
                size_t num_futures) {
    // TODO: Benchmark on various platforms, use a udipe_wait() loop if it is
    //       faster on selected platforms.
    udipe_future_t* future = udipe_start_join(context, futures, num_futures);
    assert(future);
    udipe_result_t result = udipe_finish(future);
    assert(result.command_id == UDIPE_JOIN);
}

// TODO: Add udipe_start_unordered(), don't provide a synchronous version.
// TODO: Add attribute warn_unused_result on GCC/clang.
