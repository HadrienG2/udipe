#pragma once

//! \file
//! \brief Asynchronous operation management
//!
//! Asynchronous `libudipe` commands whose name starts with `udipe_start_`, such
//! as udipe_start_connect(), do not wait for the associated operation to
//! complete and return its result. Instead they return a pointer to a \ref
//! udipe_future_t object that can be used to wait for the result to come up
//! among other things.
//!
//! Adding this intermediary stage where the command has been submitted to
//! worker threads, but has not been awaited yet, provides a lot of flexibility
//! in the submission and scheduling of I/O-related work, as described in the
//! documentation of \ref udipe_future_t.

#include "pointer.h"
#include "result.h"
#include "time.h"
#include "visibility.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>


/// Asynchronous operation future
///
/// # Basic principles
///
/// `udipe_future_t` is the heart of the asynchronous API of udipe, which itself
/// is the recommended default API that you should consider using in all
/// circumstances where you don't precisely know the performance requirements of
/// your application and want to get a good balance between ergonomics and
/// flexibility/performance.
///
/// Every asynchronous `libudipe` command (those functions in the public API
/// whose name begins with `udipe_start_`) returns to its caller as quickly as
/// possible, usually before the associated operation has completed. As opposed
/// to returning the operation's result, like a synchronous function does, an
/// asynchronous function returns a pointer to a future object, which can be
/// used to interact with the associated asynchronous operation in the ways that
/// are described below.
///
/// During normal program execution, every future **must** eventually be passed
/// to udipe_finish(), which is the point at which the operation's result or
/// errors will be reported, and associated resources will be liberated. This
/// operation is blocking by nature, though we will later see that when the
/// situation demands it, it is possible to have extra flexibility in how the
/// associated waiting is carried out..
///
/// Once a future has been passed to udipe_finish(), the ressources associated
/// with it have been liberated, and it must not be used again.
///
/// The content of a future is an opaque implementation detail of `libudipe`
/// that you should not attempt to read or modify in any way.
///
/// # Collective operations
///
/// In situations where there are multiple asynchronous operations in flight, it
/// is possible to await the associated futures one by one via `udipe_finish()`.
/// But this future usage pattern comes with two important limitations, which
/// can be adressed by leveraging udipe's collective operations:
///
/// - Sometimes, your application only needs _one_ of the pending operations to
///   complete in order to make progress, and there is no need to await _all_
///   pending operations. When this happens, you would like to conceptually
///   await the operation that will complete the earliest, followed by the
///   operation that completes next, and so on. This need is covered by the
///   udipe_start_unordered() function, which lets you await multiple futures in
///   such a way that you are gradually notified as each future completes, with
///   a way to know which of the awaited futures have completed.
/// - Even when your application does need to await several futures before it
///   can make progress, your operating system may provide more efficient ways
///   to await multiple operations than to simply await said operations one by
///   one. This optimization potential is exposed by the udipe_join() collective
///   wait function, which also comes with a udipe_start_join() asynchronous
///   version whose use will be explained in the next section.
///
/// # Chaining and associated restrictions
///
/// Network commands come with a `udipe_future_t* after` option. By setting this
/// option to point to the future associated with another asynchronous
/// operation, you can schedule the command of interest to execute after that
/// asynchronous operation.
///
/// When combined with udipe_start_join(), this feature lets you depend on as
/// many futures as you want, enabling you to express arbitrarily complex
/// dependency graphs. Sometimes, this can reduce the need for application
/// threads to constantly block waiting for asynchronous operations.
///
/// For example, a basic network packet forwarding task can be expressed as a
/// chain of network receive and send commands operating on the same buffer,
/// where the send command is scheduled to execute after the receive command
/// completes, at which point the application thread can just wait on the final
/// receive commands in order to wait for the entire task to complete. And if
/// you have multiple packets to forward, you can just amortize waiting overhead
/// further by chaining a bunch of (recv, send) futures upfront.
///
/// But with that being said, users of other future-based asynchronous
/// programming frameworks will quickly notice that the asynchronous chaining
/// capabilities of udipe are less powerful than those found elsewhere, and
/// notably exclude the ability to post-process input data via a user-defined
/// callback, an operation known in computer science circles as "monadic map".
///
/// This omission is intentional and stems from the fact that in its
/// asynchronous API, udipe enforces maximal isolation between network threads
/// and application threads. On one side, network threads execute tightly
/// performance-tuned code under a strict soft realtime discipline to minimize
/// UDP packet loss. On the other side, application threads are free to receive
/// as much or little performance tuning as the use case demands. Alas, by
/// forcing network threads to execute arbitrary application code at the time
/// where they signal some I/O completion, monadic map pokes a hole in this
/// isolation, and thus has no place in the asynchronous udipe API.
///
/// Now, performance-conscious users may object that executing very simple data
/// post-processing callbacks in network threads can have performance benefits
/// with respect to offloading that processing to application threads via a
/// thread synchronization transaction. But those performance benefits are
/// largely voided by the fact that in the asynchronous udipe API, starting any
/// network command usually involves a thread synchronization transaction, as
/// does waiting for said command to finish executing, so this API is not
/// exactly light on synchronization transactions to begin with. It is just one
/// area where the asynchronous udipe API trades performance for ergonomics.
///
/// In scenarios where performance becomes the top concern and reduced
/// ergonomics is an acceptable price to pay for it, it is instead recommended
/// to switch to the more advanced callback-based API of udipe. By putting you
/// in the driver seat of network threads, this API lets you get to the optimum
/// of zero thread synchronization per UDP packet in basic operation, and is
/// therefore the recommendation for the most performance-demanding applications
/// for which the asynchronous API is not efficient enough. (TODO: point to more
/// resources once callback API is available).
///
/// # Cancelation
///
/// There are several situations in which an asynchronous command that seemed
/// sensible at the time where it was initiated, turns out to be unnecessary as
/// time passes and more information surfaces:
///
/// - Sometimes, faillible users ask you to do something, only to realize
///   something is wrong and change their mind before the work is over.
/// - Sometimes a command is scheduled to execute after an operation that
///   errored out, which means it will never be okay to execute it.
/// - Sometimes latency optimizations force you to speculatively initiate
///   network requests that you may or may not need later on.
///
/// To handle these situations correctly, some sort of cancelation support is
/// needed. Which is why the asynchronous API of udipe comes with the following
/// asynchronous operation cancelation support:
///
/// - Any asynchronous operation which has not been awaited via udipe_finish()
///   yet can be manually canceled via udipe_cancel(). Bear in mind that this
///   may or may not succeed depending on if the work was completed beforehand,
///   and even when it does succeeds, it is not guaranteed to actually save up
///   work if the operation was already being processed.
/// - Any asynchronous operation which errors out will implicitly trigger the
///   cancelation of other operations that were scheduled to execute after it.
///
/// # Time-based scheduling
///
/// Sometimes, networking commands need to execute not in relation to each
/// other, but in relation to external factors such as the passing of time.
/// Bearing this in mind, the asynchronous udipe API comes with utilities that
/// let you sync up with the system clock in various ways:
///
/// - Sometimes the unbounded waiting logic of udipe_finish() gets in the way
///   and you would rather cancel an operation if it takes suspiciously long, or
///   at least periodically interrupt your waiting to take care of other
///   background chores. For those situations, we provide udipe_wait(), which
///   supports bounded waiting with a timeout parameter.
/// - Sometimes you would like to schedule some work to start at a specific
///   point in real time. This is a job for timers, of which udipe provides the
///   two flavors that may be used to from experience with other APIs:
///   udipe_start_timer_once() for single-shot signaling and
///   udipe_start_timer_repeat() for repeated signaling.
///
/// # Custom futures
///
/// As anyone with asynchronous programming experience can attest, async
/// frameworks are all fun and games until the day where you need to compose
/// them with some awaitable event that the asynchronous framework designer did
/// not think about, and then the eternal suffering begins.
///
/// In an attempt to at least plan ahead for such unexpected use cases, without
/// over-engineering itself into the corner of supporting arbitrarily complex
/// and OS-specific operations beyond its intended scope, udipe is able to
/// interoperate with application-defined events via a purposely minimal API:
///
/// - With udipe_start_custom(), you can create a future that behaves for all
///   intents and purposes like an asynchronous operation that has not completed
///   yet. Unlike with other udipe-provided futures, however, the result and
///   completion signaling of this operation is under your control.
/// - With udipe_set_custom(), you can set a future previously created via
///   udipe_start_custom() to a result of your choosing, which will mark the
///   associated asynchronous operation as completed and trigger the execution
///   of any work that was previously scheduled to execute after it.
///
/// While custom futures may seem convenient on paper, prospective users should
/// be warned that their API is heavily constrained by limitations of the C
/// programming languages and udipe internals. Custom futures therefore trade
/// increased flexibility for poor type safety, an inconvenient API, and major
/// deadlock hazards, which is why they should only be used sparingly and in
/// situations where no native udipe abstraction fits.
///
/// If you find yourself reaching for custom futures often, the recommended
/// course of action is to contact the udipe developers so that we can figure
/// out together how to provide better support for your use case.
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
/// ...but the price to pay is that timeouts are a bit more cumbersome as they
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
/// - Does not wait more than `timeout` plus some small processing delay. These
///   delays are typically in the microsecond range, but sadly some OS
///   primitives only support timeouts with millisecond granularity which bumps
///   the minimal timeout to 1ms.
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
// TODO: Add attribute warn_unused_result on GCC/clang.
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
/// work remains obviously to not start the work until the point where you know
/// for sure you are going to need it.
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
///
/// \returns the truth that the operation was successfully canceled, i.e. it had
///          not already completed before udipe_cancel() was called. If this
///          function returns false and there were other asynchronous operations
///          scheduled after this operation, then you will need to cancel these
///          downstream operations too.
//
// TODO: Implement
// TODO: Add attribute warn_unused_result on GCC/clang.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
bool udipe_cancel(udipe_future_t* future, bool finish);

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

/// Eagerly wait for multiple asynchronous operations to all terminate
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

// TODO: Add udipe_start_timer_once(), udipe_start_timer_repeat(), and
//       udipe_start_custom()/udipe_set_custom(), warn about the deadlock
//       hazards associated with the latter (see future docs) and clarify which
//       clock is used for the former, it should ideally match some libc-exposed
//       clock. Handle cancelation in udipe_set_custom().
// TODO: For all of these, add attribute warn_unused_result on GCC/clang.
