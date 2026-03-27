#pragma once

//! \file
//! \brief Asynchronous operation management
//!
//! Asynchronous `libudipe` commands, whose name starts with `udipe_start_`,
//! such as udipe_start_connect(), do not wait for the associated operation to
//! complete and return its result. Instead they return a pointer to a \ref
//! udipe_future_t object that can be used to wait for the result to come up
//! among other things.
//!
//! Adding this intermediary stage where the command has been submitted to
//! worker threads, but has not been awaited yet, provides a lot of flexibility
//! in the submission and scheduling of I/O-related work. See the documentation
//! of \ref udipe_future_t for more information.

#include "context.h"
#include "duration.h"
#include "nodiscard.h"
#include "pointer.h"
#include "result.h"
#include "visibility.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>


/// Asynchronous operation future
///
/// # Basic lifecycle
///
/// `udipe_future_t` is the heart of the asynchronous API of udipe, which itself
/// is the recommended "default" API for situations where you don't precisely
/// know the performance requirements of your application and want to get a good
/// balance between ergonomics, flexibility and performance.
///
/// Every `libudipe` function whose name starts with `udipe_start_` is an
/// asynchronous function. It returns to its caller as quickly as possible,
/// usually before the associated operation has completed. As opposed to
/// returning the operation's result, like a synchronous function would, an
/// asynchronous function instead returns a pointer to a future object, which
/// can be used to interact with the associated asynchronous operation in the
/// ways that are described below.
///
/// During normal program execution, each of these futures **must** eventually
/// be passed to udipe_finish(), which is the point at which the operation's
/// result or errors will be reported, and associated resources will be
/// liberated. This operation is blocking by nature, though we will later see
/// that when the situation demands it, it is possible to have extra flexibility
/// in how the associated waiting is carried out.
///
/// Once a future has been passed to udipe_finish(), the ressources associated
/// with it should be considered liberated (even though the actual liberation
/// may not occur immediately), and it must not be used again.
///
/// As a general rule, multithreaded programs must be careful not to call
/// udipe_finish() at a time where other threads might still be using the future
/// in any manner. This is true of all uses of a future including waiting for
/// its completion with udipe_wait().
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
///   version whose uses will be explained below.
///
/// # Operation chaining
///
/// Network commands come with a `udipe_future_t* after` option. By setting this
/// option to point to the future associated with another asynchronous
/// operation, you can schedule the network transaction of interest to only
/// execute after that asynchronous operation has successfully completed.
///
/// When combined with udipe_start_join(), this feature lets you depend on as
/// many futures as you want, enabling you to express arbitrarily complex
/// dependency graphs.
///
/// Sometimes, this can reduce the need for application threads to constantly
/// block waiting for asynchronous operations. For example, a basic network
/// packet forwarding task can be expressed as a chain of network receive and
/// send commands operating on the same buffer, where the send command is
/// scheduled to execute after the receive command completes, at which point the
/// application thread can just wait on the final receive commands in order to
/// wait for the entire task to complete. And if you have multiple packets to
/// forward, you can just amortize waiting overhead further by chaining a bunch
/// of (recv, send) futures upfront.
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
/// as much or little performance tuning as the use case demands. By forcing
/// network threads to execute arbitrary application code at the time where they
/// signal some I/O completion, monadic map pokes a hole in this isolation, and
/// thus has no place in the asynchronous udipe API.
///
/// Now, performance-conscious users may object that executing very simple data
/// post-processing callbacks in network threads can have performance benefits
/// with respect to offloading that processing to application threads via a
/// thread synchronization transaction. But those performance benefits are
/// largely voided by the fact that in the asynchronous udipe API, starting any
/// network command usually involves a thread synchronization transaction, as
/// does waiting for said command to finish executing, so this API is not
/// exactly light on synchronization transactions to begin with. It is one area
/// where the asynchronous udipe API trades performance for ergonomics.
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
/// - Sometimes, users ask you to do something, only to realize something is
///   wrong and change their mind before the work is over.
/// - Sometimes a command is scheduled to execute after an operation that
///   errored out, which means it will never be okay to execute it.
/// - Sometimes latency optimizations force you to speculatively initiate
///   network requests that you may or may not need later on.
///
/// To handle these situations correctly, some sort of cancelation support is
/// needed. Which is why the asynchronous API of udipe comes with the following
/// asynchronous operation cancelation support:
///
/// - Any asynchronous operation which is not being awaited via udipe_finish()
///   yet can be manually canceled via udipe_cancel(). Like all asynchronous
///   cancelation APIs, this operation comes with a lot of caveats and you
///   should read its documentation very carefully before using it.
/// - Any asynchronous operation which is canceled or errors out will
///   automatically trigger the cancelation of any other operations that were
///   scheduled to execute after it.
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
/// In an attempt to at least plan ahead for such use cases, without
/// over-engineering itself into the corner of supporting arbitrarily complex
/// and OS-specific operations beyond its intended scope, udipe is able to
/// interoperate with application-defined events via a purposely minimal API:
///
/// - With udipe_start_custom(), you can create a future that behaves for all
///   intents and purposes like an asynchronous operation that has not completed
///   yet. Unlike with other udipe-provided futures, however, the result and
///   completion signaling of this operation is under your control.
/// - With udipe_custom_try_set_result(), you can attempt to mark a future
///   previously created via udipe_start_custom() as completed, with a result of
///   your choosing. This function will fail if the future was concurrently
///   canceled, keeping it in the canceled state instead.
/// - With udipe_custom_canceled(), you can tell if this future was canceled by
///   the user before you are ready to call udipe_custom_try_set_result(). This
///   is useful for custom tasks that support being interrupted early before
///   they are done running to completion. In this case, upon receiving the
///   udipe_custom_canceled() signal, you can just stop doing what you are
///   currently doing as quickly as possible, then acknowledge that you are done
///   cleaning up with udipe_custom_finish_cancel().
///
/// By exception to the normal udipe future lifetime rules, custom futures can
/// be acted upon via `udipe_custom_` functions after they are passed to
/// udipe_finish(), but before they are passed to either
/// udipe_custom_try_set_result() or udipe_custom_finish_cancel(). However, in
/// the interest of interface consistency with other future types, it remains an
/// error to pass them to any other future-based function after they have been
/// passed to udipe_finish().
///
/// While custom futures may seem convenient on paper, prospective users should
/// be warned that their API is heavily constrained by limitations of the C
/// programming languages and udipe internals. Custom futures therefore trade
/// increased flexibility for poor type safety, an inconvenient API, and
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
/// treated as liberated. In other words, udipe_finish() cannot be called until
/// all previous calls to the udipe API that involve this future have returned,
/// and this future cannot be passed to any other udipe function by any thread
/// anymore after the point where udipe_finish() has started being called.
///
/// One consequence of this rule is that a future which has been passed to
/// udipe_finish() cannot be passed to udipe_cancel(), and thus the implicit
/// wait of udipe_finish() cannot be canceled. To achieve cancelable wait or any
/// kind of concurrent waiting by multiple threads, you will need to use
/// udipe_wait() first instead of calling udipe_finish() directly.
///
/// By the time this function returns, it is guaranteed that...
///
/// - The result of the asynchronous operation is known (it is, after all, the
///   return value of this function).
/// - Any input parameter provided via a pointer at the time where the
///   asynchronous operation was started will not be accessed anymore. So the
///   memory targeted by such pointers can safely be modified, liberated, etc.
/// - The liberation of any udipe state associated with the asynchronous
///   operation has been scheduled (though it may not have happened yet).
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
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_PUBLIC
udipe_result_t udipe_finish(udipe_future_t* future);

/// Wait up to a certain duration for the end of an asynchronous operation
///
/// In contrast with udipe_finish(), which also waits for asynchronous
/// operations to terminate, this function **only** waits for a result to be
/// available, it does not fetch that result and liberates resources. In other
/// words, `udipe_wait()`...
///
/// - Does not fetch the future's result if the operation does complete. It only
///   tells you when the result is ready to fetch via udipe_finish(), which
///   becomes nonblocking at this point.
/// - Supports cancelation and other kinds of concurrent operation on the same
///   future by multiple threads, at the expense of a performance hit caused by
///   CPU cache contention and thread synchronization overhead.
/// - Does not wait more than `timeout` plus some extra delay. This extra delay
///   is normally a small processing overhead in the microsecond range, but
///   sadly some underlying OS APIs only support timeouts with millisecond
///   granularity, which increases the minimal timeout to 1ms.
/// - Does not liberate the resources associated with the future, you must still
///   call udipe_finish() at some later time to achieve this.
///
/// Here are some examples of use cases that you can handle by calling
/// `udipe_wait()` first instead of calling udipe_finish() directly:
///
/// - You need a timeout feature where requests are abandoned after a while if
///   they take abnormally long. This is achieved by using udipe_wait() with a
///   timeout, followed by udipe_cancel() if the timeout is indeed reached.
/// - You must periodically take some background action, such as refreshing a
///   display or snapshoting your app's state, and therefore cannot afford to
///   wait indefinitely for a udipe operation to complete. This can be done with
///   repeated timeout waits, but unordered and timer futures may be a better
///   building block for this by virtue of being based on absolute rather than
///   relative durations and thus avoiding "clock drift".
/// - You need multiple application threads to wait for a udipe operation to
///   complete. This can be done with `udipe_wait()`, but bear in mind that you
///   will need to be extra careful as you still need to call udipe_finish() on
///   one thread eventually and it can only be done after all other threads have
///   returned from their wait. For this use case, it is normally better to have
///   one thread that is responsible for awaiting, fetching and broadcasting the
///   result, which other threads will await via a broadcast synchronization
///   primitive under your control such as a condition variable.
///
/// \param future must be a future that was returned by an asynchronous function
///               (those whose name begins with `udipe_start_`) and has not been
///               liberated by udipe_finish() or udipe_cancel() since.
/// \param timeout must be a timeout in nanoseconds, after which this function
///                will give up and return `false` if the asynchronous operation
///                has not completed yet. Special value \ref UDIPE_DURATION_MIN
///                can be used if you just want to non-blockingly check if the
///                operation has completed.
///
/// \returns the truth that the operation associated with `future` has
///          completed and its result is ready to be fetched. If this function
///          returns `true` (which is guaranteed when `timeout` is \ref
///          UDIPE_DURATION_MAX), calling udipe_finish() on the same future is
///          guaranteed to return a result immediately without blocking.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_PUBLIC
bool udipe_wait(udipe_future_t* future, udipe_duration_ns_t timeout);

/// Cancel an asynchronous operation
///
/// This function notifies the udipe infrastructure that the asynchronous
/// operation associated with a certain future is not desired anymore and should
/// terminate as quickly as possible, avoiding any work that had not begun yet
/// including dependent work scheduled after the target future.
///
/// Like any other asynchronous cancelation APIs, this operation is inherently
/// racy and therefore comes with a lot of caveats that must be kept in mind
/// while using it:
///
/// - Canceling an operation that has already completed is ineffective, in
///   particular it will not cancel downstream operations scheduled after the
///   targeted operation. If that is what you are after, you are responsible for
///   keeping track of the dependency tree downstream of the future of interest
///   and walking down that tree as much as necessary to cancel everything.
/// - Even if the operation has not already completed, there is no guarantee
///   that cancelation will save any work or meaningfully reduce the remaining
///   wait delay. Cancelation is just a hint to the implementation of the
///   underlying asynchronous operation, and not all implementations are capable
///   of taking this hint at all times, especially when they have already
///   started and progressed quite far along their execution path.
/// - An operation that is being awaited via udipe_finish() cannot be canceled.
///   If you are interested in cancelation, you need a more involved workflow
///   where you call udipe_wait(), wait for other threads that could have
///   canceled the operation to notify that they have observed the end of the
///   wait, and finally call udipe_finish().
///
/// In other words, though latency and UX considerations will sometimes dictate
/// otherwise, the most resource-efficient way to cancel some work remains
/// obviously to not start the work until the point where you know for sure you
/// are going to need it.
///
/// ---
///
/// If the `finish` flag is set, then after canceling the operation, this
/// function additionally waits for the operation to terminate then liberates
/// associated resources as if udipe_finish() were called. If `finish` is not
/// set, then udipe_cancel() returns immediately and must be followed by an
/// udipe_finish() call (possibly preceded by some udipe_wait() if
/// bounded-duration waits are desired) to finish resource cleanup.
///
/// It must be understood that the `finish` flag is only safe to set in
/// situations where you know that no other thread is waiting or could start
/// waiting for the future. A typical use case for it is single-threaded
/// workflows when you have started directly or indirectly waiting for a future,
/// then realize you don't need to wait for this particular operation after all.
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
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_PUBLIC
bool udipe_cancel(udipe_future_t* future, bool finish);

/// Start waiting for multiple asynchronous operations to terminate.
///
/// This function returns a future that will complete with an empty result once
/// **all** upstream operations have completed, or become canceled if **at least
/// one** upstream operation is canceled or errors out.
///
/// This operation serves several purposes:
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
/// \param futures must point to an array of at least one futures that were all
///                returned by an asynchronous function (those whose name begins
///                with `udipe_start_`) and have not been liberated by
///                udipe_finish() or udipe_cancel() since. The output future
///                will retain a pointer to this array, which must therefore not
///                be modified or liberated until the completion of the output
///                future has been awaited via udipe_finish() or udipe_cancel().
/// \param num_futures must match the length of the `futures` array, and thus be
///                    at least one.
///
/// \returns a future that will terminate with an empty result once all input
///          futures have terminated. When this happens, results of `futures`
///          can then be fetched in a non-blocking manner by simply calling
///          udipe_finish() on each of them in a sequence.
//
// TODO: Implement.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
UDIPE_PUBLIC
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
/// than that of simply calling udipe_wait() sequentially for each input future.
/// But bear in mind that you WILL need to call udipe_finish() on each of the
/// input futures eventually in order to check out their results and liberate
/// associated state. All udipe_join() guarantees is that these calls will be
/// nonblocking, which should reduce their individual overhead.
///
/// \param context must point to an \ref udipe_context_t that has been set up
///                via udipe_initialize() and hasn't been liberated via
///                udipe_finalize() since.
/// \param futures must point to an array of at least one futures that were all
///                returned by an asynchronous function (those whose name begins
///                with `udipe_start_`) and have not been liberated by
///                udipe_finish() or udipe_cancel() since.
/// \param num_futures must match the length of the `futures` array, and thus be
///                    at least one.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
void udipe_join(udipe_context_t* context,
                udipe_future_t* const futures[],
                size_t num_futures);

/// Start waiting for multiple asynchronous operations to terminate, returning a
/// chain of futures that will terminate as the upstream operations terminate
///
/// This asynchronous command is somewhat similar to udipe_start_join(), in that
/// it produces a future that lets you asynchronously wait for multiple futures
/// to terminate whenever you are ready for it. However, in contrast with
/// udipe_start_join(), the wait is more fine-grained.
///
/// The future that you get out of of udipe_start_unordered() lets you wait for
/// the **first** upstream operation to complete, then tells you which operation
/// completed and gives you another future that lets you wait for the second
/// upstream operation to complete, and so on until all initially specified
/// operations have completed.
///
/// Compared to joined futures, unordered futures have some benefits...
///
/// - Unordered execution can be used as a more flexible alternative to
///   timeouts, as it lets you race any pair of asynchronous operation (not just
///   time-based ones), pick the output of whichever operation completes first,
///   and cancel the other operation.
/// - Unordered execution avoids blocking the CPU for an unnecessarily long time
///   in concurrent workloads where you can make progress when _any_ upstream
///   operations terminate, as opposed to needing to wait for _all_ upstream
///   operations to terminate before making progress.
///     - It should be noted that this theoretical advantage will only translate
///       into a concrete performance benefit if your upstream operations take a
///       meaningfully different amount of time to complete. If that is not the
///       case, the extra CPU overheads described below will likely dominate and
///       result in a net performance loss.
///
/// ...but unordered execution also has some drawbacks that must be kept in mind
/// before blindly using it all the time for all purposes.
///
/// - Unordered futures are less amenable to chaining work than joined futures
///   because you can only schedule work after the first operation completes and
///   you do not know which of the input operations it will be.
/// - Unordered futures require a lot more thread synchronization and resource
///   allocation transactions (one per element of the `futures` array), and thus
///   will cost more CPU time on the client thread than joined futures.
///
/// ...which is why you should not be afraid of mixing and matching
/// udipe_start_join() with udipe_start_unordered() as appropriate, maybe even
/// using both at the same time on the same set of futures in complex use cases.
///
/// \param context must point to an \ref udipe_context_t that has been set up
///                via udipe_initialize() and hasn't been liberated via
///                udipe_finalize() since.
/// \param futures must point to an array of at least one futures that were all
///                returned by an asynchronous function (those whose name begins
///                with `udipe_start_`) and have not been liberated by
///                udipe_finish() or udipe_cancel() since. The output futures
///                will retain a pointer to this array, which must therefore not
///                be modified or liberated until the completion of all output
///                futures has been awaited via udipe_finish() or has been
///                canceled via udipe_cancel().
/// \param num_futures must match the length of the `futures` array, and thus be
///                    at least one.
///
/// \returns a future that will terminate and yield a result with payload type
///          \ref udipe_unordered_payload_t as soon as **one** of the input
///          futures have terminated. This result will tell you which of the
///          input futures has terminated, so that you can then non-blockingly
///          fetch its result with udipe_finish(). Along with that index, you
///          will also get another future, which lets you wait for the next
///          operation to complete, until all operations have completed.
//
// TODO: Implement.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
UDIPE_PUBLIC
udipe_future_t* udipe_start_unordered(udipe_context_t* context,
                                      udipe_future_t* const futures[],
                                      size_t num_futures);

/// Return a one-shot timer future that will complete with no result once a
/// specific absolute time is reached
///
/// The target time is specified in the same format that is output by the C11
/// [`timespec_get()`](https://en.cppreference.com/w/c/chrono/timespec_get.html)
/// function in `TIME_UTC` mode. On both Unices and Windows, if the system clock
/// is set up correctly, this will corresponds to a number of seconds and
/// nanoseconds elapsed since the Unix epoch (Midnight, January 1, 1970, UTC),
/// with only approximate handling of leap seconds. This behavior matches that
/// of the `CLOCK_REALTIME` system clock defined by POSIX's
/// [`clock_gettime()`](https://pubs.opengroup.org/onlinepubs/9799919799/functions/clock_getres.html).
///
/// This future is not normally used in isolation. It is rather chained before
/// other futures to schedule them for execution at a specific time, or combined
/// with other futures through udipe_start_unordered() when you need an absolute
/// timeout rather than a relative one. In this latter role, one notable
/// property of timer futures is that they can have finer time resolution than
/// standard operating system timeouts on udipe_wait(), at the expense of being
/// more expensive to set up.
///
/// \param context must point to an \ref udipe_context_t that has been set up
///                via udipe_initialize() and hasn't been liberated via
///                udipe_finalize() since.
/// \param ts must point to a valid `struct timespec` indicating at which time
///           the wait will complete, following the conventions outlined above.
///
/// \returns a future that will terminate with an empty result once the
///          specified absolute time point has been reached.
//
// TODO: Implement.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
UDIPE_PUBLIC
udipe_future_t* udipe_start_timer_once(udipe_context_t* context,
                                       const struct timespec *ts);

/// Return a repeating timer future that will first complete once a specific
/// absolute time is reached, then yield a chain of other futures that complete
/// following subsequent timer ticks at a specified time interval.
///
/// At udipe_finish() time, each future within the chain will also tell you how
/// many of the specified timer ticks were missed, which can be used to detect
/// situations where your application is not keeping up with user-specified
/// periodicity constraints and should take corrective actions to get back in
/// the desired performance range.
///
/// For example, let's say that you specify an `initial` time 3s into the future
/// (we'll call that T+3s), then an `interval` of 100ms. This results in a timer
/// with ticks at T+3s, T+3.1s, T+3.2s, etc.
///
/// - The future returned by this function will complete at T+3s.
///   - If you read its result with udipe_finish() between T+3s and T+3.1s, you
///     will observe 0 missed timer ticks.
///   - Between T+3.1s and T+3.2s, you will observe 1 missed timer tick.
///   - Between T+3.2s and T+3.3s, you will observe 2 missed timer tick, etc.
/// - No matter at which time you read the result of a given future, subsequent
///   futures in the chain will keep following the same regular tick cadence. So
///   if for example you read the initial future at T+3.07s, the next future
///   will still complete at T+3.1s.
///
/// \param context must point to an \ref udipe_context_t that has been set up
///                via udipe_initialize() and hasn't been liberated via
///                udipe_finalize() since.
/// \param initial must point to a valid `struct timespec` indicating at which
///                time the first yielded future will complete, following the
///                conventions outlined in the documentation of
///                udipe_start_timer_once().
/// \param interval must specify a nonzero number of nanoseconds to await
///                 between between timer ticks, which subsequent futures in the
///                 chain will report. There is no \ref UDIPE_DURATION_DEFAULT
///                 for this function.
///
/// \returns a future that will terminate with an empty result once the
///          `initial` time point has been reached, yielding a number of missed
///          timer ticks and a chain of other futures that complete following a
///          regular cadence given by `interval`.
//
// TODO: Implement.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
UDIPE_PUBLIC
udipe_future_t* udipe_start_timer_repeat(udipe_context_t* context,
                                         const struct timespec *initial,
                                         udipe_duration_ns_t interval);

/// Create a custom future that will complete with a result of your choosing
/// once udipe_set_custom() is called on it.
///
/// # Custom future basics
///
/// Custom futures enable the asynchronous udipe API to interoperate with other
/// asynchronous and blocking system APIs, at a price: due to design constraints
/// from both udipe and the C type system, the resulting API is rather
/// error-prone. Therefore, if you find yourself using custom futures often, you
/// should consider contacting the udipe development team to see if your use
/// case could gain first-class support in udipe.
///
/// Custom futures can be passed to all usual future-based APIs (udipe_finish(),
/// udipe_wait(), udipe_join()...), but in addition they support two extra
/// operations:
///
/// - When your asynchronous operation is completed and its result is ready, you
///   can submit this result with udipe_custom_try_set_result(). This will mark
///   the future as completed and wake up all threads that were waiting for its
///   completion, assuming the operation wasn't canceled beforehand. Otherwise
///   it will just notify you about the cancelation and keep the future in the
///   canceled state.
/// - If your custom asynchronous operation supports being interrupted, then you
///   should consider periodically checking udipe_custom_canceled() in order to
///   be notified when the future is canceled. If you don't do so, you will
///   still be notified about cancelation upon calling
///   udipe_custom_try_set_result().
///
/// # Deadlock hazards
///
/// By nature, custom futures introduce deadlock hazards. For example, this code
/// will instantly deadlock for hopefully obvious reasons:
///
/// ```c
/// udipe_future_t* custom = udipe_start_custom(context);
/// udipe_finish(custom);
/// // Whoops, too late!
/// udipe_custom_try_set_result(custom, successful, payload);
/// ```
///
/// One less obvious avenue for deadlock, however, is network thread
/// backpressure. To prevent a client submitting work faster than it can be
/// processed from trashing CPU caches and eventually exhausting system RAM,
/// network command submission becomes blocking once the number of waiting tasks
/// reaches a certain threshold. One side-effect of this safety feature is that
/// the following custom future usage pattern is also unsafe:
///
/// - Create a custom future
/// - Schedule network operations to start after this custom future completes
/// - Set the result of the custom future to get the operations started
///
/// The aforementioned deadlock hazards can often be avoided by following some
/// relatively simple deadlock safety principles…
///
/// - In all but the most trivial usage scenarios, custom futures must be
///   awaited by a thread other than the thread that is destined to signal them
///   with udipe_custom_try_set_result().
/// - The thread that creates the custom future must arrange for it to be
///   signaled by another thread before awaiting it or scheduling any work that
///   depends on it.
/// - The thread that is in charge of signaling the future must not perform any
///   operation that may lead it to wait (directly OR indirectly) for a thread
///   that is awaiting the custom future, or scheduling other work that depends
///   on the custom future. To be more specific, it should not wait for an
///   action that these threads are destined to take after awaiting the custom
///   future or scheduling dependent work.
///
/// In practice, these principles can often be honored by segregating your
/// application threads into "udipe threads" on one side that schedule and await
/// udipe work, and "non-udipe threads" on the other side that eagerly perform
/// work then signal its completion to the udipe threads, without ever using any
/// udipe functionality other than creating custom futures, checking for
/// cancelation and submitting results in the process.
///
/// \returns a future that will terminate at a moment of your choosing and with
///          a result of your choosing, via a call to
///          udipe_custom_try_set_result().
//
// TODO: Implement.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
UDIPE_PUBLIC
udipe_future_t* udipe_start_custom(udipe_context_t* context);

/// Check if a custom future was canceled via udipe_cancel()
///
/// Custom tasks that support being interrupted should periodically check this
/// function. If it starts returning `true`, they should stop doing what they
/// are doing as early as possible, then call udipe_custom_finish_cancel() to
/// acknowledge the cancelation signal.
///
/// Custom tasks that do not support being interrupted do not need to bother
/// with this periodical checking and can simply run to completion then call
/// udipe_custom_try_set_result() at the end. The call will fail without
/// changing the future result, which is how they will know that a cancelation
/// signal has been received.
///
/// \param custom must be a custom future that was created with
///               udipe_start_custom() and hasn't been passed to
///               udipe_custom_finish_cancel() or udipe_custom_try_set_result()
///               yet. By exception to the normal udipe future lifetime rules,
///               it is valid to pass in a future that has already been passed
///               to udipe_finish().
//
// TODO: Implement.
UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_PUBLIC
bool udipe_custom_cancelled(udipe_future_t* custom);

/// Acknowledge the cancelation of a custom future
///
/// After receiving the udipe_custom_cancelled() signal, a custom task should
/// interrupt its work as quickly as possible, then call this function to
/// acknowledge that it is done canceling itself and will not perform any
/// further processing related to its initially scheduled task.
///
/// This will have the effect of waking up downstream threads that were waiting
/// for this task to complete, with a notification that it was canceled. At this
/// point, they should be free of modifying or reallocating any input data
/// pointer that was passed in at the time where the custom task was created,
/// without any risk of racing with the thread that implements the custom task. In particular,
///
/// It is an error to call this function on a future that was _not_ passed to
/// udipe_cancel() and therefore never reported being canceled via
/// udipe_custom_canceled().
///
/// \param custom must be a custom future that was created with
///               udipe_start_custom() and cancelled via udipe_cancel(), but
///               hasn't been passed to udipe_custom_finish_cancel() or
///               udipe_custom_try_set_result() yet. By exception to the normal
///               udipe future lifetime rules, it is valid to pass in a future
///               that has already been passed to udipe_finish(). But after
///               being passed to this function, the future must never be passed
///               to any other `udipe_custom_` function again.
//
// TODO: Implement.
UDIPE_NON_NULL_ARGS
UDIPE_PUBLIC
void udipe_custom_finish_cancel(udipe_future_t* custom);

/// Attempt to set the end result of a custom operation
///
/// This will normally mark the future as completed with the specified result,
/// unless it was previously canceled, in which case the operation will fail and
/// report this failure by returning `false`.
///
/// When this happens, the future will still be marked as completed, but without
/// a result. Instead it will be in a canceled state.
///
/// \param custom must be a custom future that was created with
///               udipe_start_custom() and hasn't been passed to
///               udipe_custom_finish_cancel() or udipe_custom_try_set_result()
///               yet. By exception to the normal udipe future lifetime rules,
///               it is valid to pass in a future that has already been passed
///               to udipe_finish(). But after being passed to this function,
///               the future must never be passed to any other `udipe_custom_`
///               function again.
/// \param successful indicates whether the custom operation should be
///                   considered successful, in the sense that other futures
///                   scheduled after this one should be allowed to start
///                   executing.
/// \param payload is a custom data block of your choosing, which encodes the
///                end result of the custom operation. In case of success, it
///                can be used to pass down the result of the operation if it
///                was not transmitted by other means like filling a
///                user-provided buffer. In case of failure, it should encode
///                any information to take appropriate error handling action
///                needed downstream.
///
/// \returns the truth that a result was successfully set. Setting a result can
///          fail if the target future was canceled by its client. In this case
///          `false` will be returned, otherwise `true` will be returned.
//
// TODO: Implement.
UDIPE_NON_NULL_ARGS
UDIPE_PUBLIC
bool udipe_custom_try_set_result(udipe_future_t* custom,
                                 bool successful,
                                 udipe_custom_payload_t payload);
