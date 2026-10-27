#pragma once

//! \file
//! \brief Command queue
//!
//! This code module implements the priority message queue that is used to to
//! submit commands from client threads that use `libudipe` via a \ref
//! udipe_context_t to a particuler worker thread managed by `libudipe`.
//!
//! Each worker thread gets such a queue, and the \ref udipe_context_t takes
//! care of balancing the incoming workload between worker threads.

// TODO: Review include list after implementation rework
#include <udipe/connect.h>
#include <udipe/future.h>

#include "arch.h"
#include "bits.h"
#include "messaging.h"

#include <assert.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <threads.h>


/// \name Implementation limits
/// \{

/// Number of commands that can be pending for each priority level
///
/// If a client thread attempts to submit a new command to a worker thread that
/// already has this many pending unprocessed commands of the same priority, the
/// client thread will block waiting for the worker to make progress.
///
/// This is currently enforced by simply having one messaging allocator per
/// priority level, which provides an ample buffer of 32 incoming commands while
/// ensuring absence of allocator-originated priority inversion (i.e. no amount
/// of pending low-priority commands can prevent a client from allocating and
/// submitting a higher-priority command).
#define MAX_COMMANDS_PER_PRIORITY ((size_t)MESSAGING_BLOCK_LEN)

/// Sentinel priority level that indicates that no commands are available yet,
/// but more commands may come and a worker should wait for them using the
/// command queue's \ref event_t.
#define PRIORITY_WAIT_FOR_EVENT  SIZE_MAX
static_assert(PRIORITY_WAIT_FOR_EVENT > PRIORITY_HIGHEST,
              "PRIORITY_WAIT_FOR_EVENT sentinel must not overlap valid priority range");

/// Sentinel priority level that indicates that no commands are available and no
/// more commands should be coming because the worker has been requested to shut down.
#define PRIORITY_SHUTDOWN  (PRIORITY_WAIT_FOR_EVENT - (size_t)1)
static_assert(PRIORITY_SHUTDOWN > PRIORITY_HIGHEST,
              "PRIORITY_SHUTDOWN sentinel must not overlap valid priority range");

/// \}


/// \name Global status word
/// \{

/// Global command queue status word
///
/// This machine word contains a high-level summary of a worker's command queue
/// state. It is used to guide and report high-level worker decisions:
///
/// - The highest-order bit indicates whether the worker is waiting for commands
///   and must be notified with an \ref event_t when new work is ready.
///     * When this bit is set, clients which report incoming work must signal
///       the \ref event_t to notify the worker about it, after clearing this
///       bit to prevent other clients from sending unnecessary extra event
///       object notifications.
///     * When this bit is unset, the worker is actively processing work, so
///       clients who publish work do not need to notify the event object as
///       long as the bit is cleared at the time where the work gets published.
/// - The next highest-order bit indicate that the worker shutdown process has
///   started, and no new commands will be incoming.
/// - The \ref NUM_PRIORITIES lowest-order bits indicate, when set, that a
///   particular priority class has commands available for processing.
///     * The low-order bit represents the highest supported command priority,
///       each higher order bit represents a lower priority. So for 8 supported
///       priorities, bit 0 represents priority 7, bit 1 represents priority 6,
///       ..., and bit 7 represents priority 0. This layout makes it possible to
///       find out the highest priority class that has commands available using
///       the find-first-set word binary arithmetic operation, which typically
///       translates to a single CPU instruction.
/// - Other bits are reserved and must be stay zero.
///
/// Lock-free transactions on this word mostly use an Acquire-Release memory
/// model:
///
/// - Any readout that is used to infer the state of a memory location other
///   than the global status word, typically for the sake of subsequently
///   accessing this memory location, must have Acquire memory ordering.
/// - Any operation that tells other threads that the state of other memory
///   locations has evolved, in a manner that would lead said threads to
///   subsequently investigate these memory locations, must have Release memory
///   ordering.
/// - Any operation that does both of the above (reports world state change and
///   acquires knowledge of new world state) must have Acquire-Release memory
///   ordering.
/// - Any operation that does none of the above (only cares about the state of
///   this word, typically because it is only used as preparation for another
///   atomic transaction on the word that has stronger ordering) can have
///   Relaxed memory ordering.
typedef _Atomic word_t command_global_status_t;

/// Bit of \ref command_global_status_t tracking the need for \ref event_t
/// signaling
///
/// If this bit is set at the time where a client signals the availability of
/// new commands, then this client must notify the worker about work
/// availability by signaling the global event object.
#define COMMAND_GLOBAL_EVENT_NEEDED  ((word_t)1 << (BITS_PER_WORD - 1))

/// Bit of \ref command_global_status_t tracking the fact that worker shutdown
/// has started
///
/// This bit is set in debug builds at the time where a worker shutdown is
/// requested by udipe_finalize(). In addition to its basic context lifecycle
/// support role, it is also used by debug assertions to detect racy command
/// submission or processing in parallel with udipe_finalize().
#define COMMAND_GLOBAL_SHUTTING_DOWN  (COMMAND_GLOBAL_EVENT_NEEDED >> 1)

/// Bits of \ref command_global_status_t tracking the availability of new work
/// and its priority.
///
/// See \ref command_global_status_t for more information about how the
/// availability of work at various priorities is tracked
#define COMMAND_GLOBAL_PRIORITY_MASK  \
    (((word_t)1 << NUM_PRIORITIES) - (word_t)1)
static_assert(
    (COMMAND_GLOBAL_PRIORITY_MASK & COMMAND_GLOBAL_EVENT_NEEDED) == 0
    && (COMMAND_GLOBAL_PRIORITY_MASK & COMMAND_GLOBAL_SHUTTING_DOWN) == 0,
    "Current implementation does not support that many priorities"
);

/// Bits of \ref command_global_status_t that are reserved and must stay zeroed
///
/// Checked in debug builds, ignored in release builds.
#define COMMAND_GLOBAL_RESERVED_MASK  (  \
    WORD_MAX  \
    - COMMAND_GLOBAL_EVENT_NEEDED  \
    - COMMAND_GLOBAL_SHUTTING_DOWN   \
    - COMMAND_GLOBAL_PRIORITY_MASK  \
)

/// Initialize a \ref command_global_status_t
///
/// In the initial state, the worker is waiting for an \ref event_t signal
/// reporting the availability of new commands, and no command is available.
///
/// This operation is not thread-safe and must be performed before the worker's
/// global status word is exposed to client threads.
///
/// This function must be called within a logging scope.
///
/// \param global_status is meant to target a newly created global status word,
///                      which may not be initialized yet and must in any case
///                      not have begun finalization via
///                      command_global_status_start_finalize() yet. It will be
///                      initialized to a state that is ready for worker and
///                      client use.
// TODO implement
void command_global_status_initialize(
    command_global_status_t* global_status
);

/// Notify the worker that work is available with a certain priority via the
/// \ref command_global_status_t, tell if \ref event_t signaling is needed
///
/// This operation must be performed after the appropriate command list has been
/// fully filled and the associated local status word has been updated with
/// Release ordering to publish its final state.
///
/// As a lock-free operation, it has Release memory ordering, because it tells
/// the worker that the state of the associated local status word has changed.
/// It additionally has Acquire ordering (totaling AcqRel ordering) when it
/// observes that the worker is waiting for an \ref event_t.
///
/// This function must be called within a logging scope.
///
/// \param global_status is the global status word of the worker, which must
///                      have previously been initialized via
///                      command_global_status_initialize() and must not have
///                      begun finalization via
///                      command_global_status_start_finalize() yet.
/// \param priority is the priority class that has new commands available, which
///                 must be lower than or equal to \ref HIGHEST_PRIORITY.
/// \returns the truth that work availability must be additionally signaled via
///          the \ref event_t of the command queue.
// TODO implement, check priority range
bool command_global_status_notify(command_global_status_t* global_status,
                                  size_t priority);

/// Check the current value of a \ref command_global_status_t as a prelude for a
/// worker's command-fetching loop
///
/// This operation is solely used to initialize the worker's command-fetching
/// loop after an \ref event_t notification has been received. It performs
/// its internal state-checking accordingly.
///
/// As a lock-free operation, it has Relaxed memory ordering, because it only
/// serves as a prelude to other command_global_status_fetch_next() operations
/// that perform any required synchronization with the proper ordering.
///
/// This function must be called within a logging scope.
///
/// \param global_status is the global status word of the worker, which must
///                      have previously been initialized via
///                      command_global_status_initialize(), must not have
///                      reported finalization via a \ref PRIORITY_SHUTDOWN
///                      return from command_global_status_fetch_next() yet, and
///                      must reflect a state in which the worker is not waiting
///                      for an \ref event_t signal + has work available.
/// \returns the initial state of the global status word, after debug checks.
// TODO implement
word_t command_global_status_fetch_start(
    const command_global_status_t* global_status
);

/// Subset of a worker's command-fetching loop iteration that acts on a \ref
/// command_global_status_t
///
/// Starting from a known but possibly stale previous global status word state,
/// this operation checks out the highest priority level at which availability
/// of new commands is being reported, if any, then...
///
/// * If commands are available, it clears the associated highest-priority
///   availability bit with Acquire ordering, then returns the observed command
///   batch priority. The worker must then go to the local status word for that
///   priority in order to process and acknowledge/recycle the freshly observed
///   command batch.
/// * If no commands are available, but more could still be coming, it sets the
///   \ref COMMAND_GLOBAL_EVENT_NEEDED bit with Release ordering, then returns
///   \ref PRIORITY_WAIT_FOR_EVENT sentinel value. Upon receiving this sentinel
///   value, the worker must wait for the command queue's \ref event_t to be
///   signaled, indicating availability of new work, while processing its
///   background I/O workload meanwhile.
/// * If no commands are available and it is known that no more could be coming
///   because the worker has been requested to shut down, it acknowledges
///   shutdown and returns \ref PRIORITY_SHUTDOWN. Upon receiving this sentinel
///   value, the worker must proceed to shut down, not touching this global
///   status word ever again in the process.
///
/// This function must be called within a logging scope.
///
/// \param global_status is the global status word of the worker, which must
///                      have previously been initialized via
///                      command_global_status_initialize(), must not have
///                      reported finalization via a \ref PRIORITY_SHUTDOWN
///                      return from command_global_status_fetch_next() yet, and
///                      must reflect a state in which the worker is not waiting
///                      for an \ref event_t signal. It will be updated as
///                      described above.
/// \param expected is the expected value of the global status word at the start
///                 of the transaction. It can be initialized using
///                 command_global_status_fetch_start(), then each call to this
///                 function will update it to the final value of the status
///                 word after the transaction has been committed.
/// \returns the priority at which commands must be fetched, \ref
///          PRIORITY_WAIT_FOR_EVENT if no command is available but more could
///          still come and be signaled via the \ref event_t, or \ref
///          PRIORITY_SHUTDOWN if no command is available and it is known that
///          no more command will come.
// TODO implement
size_t command_global_status_fetch_next(
    command_global_status_t* global_status,
    word_t* expected
);

/// Start the finalization of a \ref command_global_status_t
///
/// This is done by the command queue finalizer, which itself is called by
/// udipe_finalize(). It tells workers that they should not expect any more
/// commands, and are asked to shut down.
///
/// This function must be called within a logging scope.
///
/// \param global_status is the global status word of the worker, which must
///                      have previously been initialized via
///                      command_global_status_initialize() and must not have
///                      begun finalization via
///                      command_global_status_start_finalize() yet. It will be
///                      switched to a finalizing state that must not be touched
///                      by clients, but can be touched by
///                      command_global_status_fetch_next() one last time before
///                      full finalization occurs.
// TODO implement
void command_global_status_start_finalize(
    command_global_status_t* global_status
);

/// \}

// TODO: Local status word
// TODO: Local command list double buffer
// TODO: Local state = Local status word + local command list double
//       buffer + 1 messaging allocator + 1 messaging block, arranged with due
//       attention paid to alignment and false sharing issues.
// TODO: Command queue struct = 1 local state per priority class + 1 global
//       status word + 1 event object which should be externally provided (in
//       case it's set up together with an inpoll on Linux). Use careful
//       alignment-aware packing, and put everything into a realtime-safe memory
//       allocation that is allocated with the NUMA locality of the cpuset of
//       the worker thread.



// TODO: Reworking the stuff below


/// Worker thread command queue capacity
///
/// This dictates how many commands can be waiting to be processed by the worker
/// thread before an attempt to enqueue another blocks the client thread,
/// thusly enforcing backpressure.
///
/// \internal
///
/// This capacity is chosen such that each command queue should fit inside of a
/// single dedicated page of data, by building upon the following principles:
///
/// - Any given client thread should send commands to `libudipe` rarely (as
///   streams are meant to handle the main use cases for large amounts of
///   commands), so achieving spatial cache locality across consecutive command
///   submitted by the same client thread in quick successions is not a goal.
/// - Each command may be submitted by a different client thread and, by the
///   above observation, should therefore be written to its own false sharing
///   atom.
/// - To maximize worker thread performance, the control block that multiple
///   client threads use to decide which client will send the next command, for
///   which there may be arbitrarily high cache contention, should be distinct
///   from the control block that the winner client thread uses to synchronize
///   with the worker. We therefore need two control blocks in addition to
///   command storage.
/// - Because network transfers are much slower than integer division, we
///   probably do not need to enforce a power-of-two queue capacity (which, in
///   the presence of control blocks, would unfortunately divide usable queue
///   capacity by half).
#define COMMAND_QUEUE_LEN (EXPECTED_MIN_PAGE_SIZE/FALSE_SHARING_GRANULARITY - 2)

// TODO: Also plan ahead a "shut down now" signal, but it should bypass
//       normal FIFO queuing. Maybe cross-thread SIGTERM would be fine here?

/// Worker thread command
///
/// This is a complete worker thread command, which tells a worker thread about
/// a certain task that it should perform.
///
/// \internal
///
/// This struct should strive to stay smaller than the cache line size of all
/// supported CPU architectures. As of 2025, all high-performance CPU
/// architectures have a cache line size of 64B or larger.
typedef struct command_s {
    /// Parameters that are appropriate for this command type
    ///
    /// The value of `id` indicates which of this union's variants is valid.
    alignas(FALSE_SHARING_GRANULARITY) union {
        udipe_connect_options_t* connect;
        udipe_disconnect_options_t disconnect;
        // TODO: Add and implement
        /*udipe_send_options_t send;
        udipe_recv_options_t recv;*/
    } options;

    /// Result future, to be filled up and signaled upon command completion
    ///
    /// Also indicates what kind of command you are dealing with.
    ///
    /// This pointer cannot be `NULL`.
    udipe_future_t* future;

    // TODO: Consider some kind of QoS infrastructure so that e.g. IPBus slow
    //       control avoids using the same threads as acquisition. Can this just
    //       be a connection option, or does it need to be more?
} command_t;
static_assert(alignof(command_t) == FALSE_SHARING_GRANULARITY,
              "Each command may originate from a different client thread and "
              "should therefore be stored in its own false sharing atom");
static_assert(sizeof(command_t) == FALSE_SHARING_GRANULARITY,
              "Shouldn't need more than one false sharing atom per command");
static_assert(
    offsetof(command_t, future) + sizeof(udipe_future_t*) <= CACHE_LINE_SIZE,
    "Should fit on a single cache line for optimal memory access performance "
    "on CPUs where the FALSE_SHARING_GRANULARITY upper bound is pessimistic"
);

/// Multi-producer single-consumer command queue of a `libudipe` worker thread
///
/// This queue is blocking on the client side but lock-free on the worker side,
/// following from the observation that UDP network threads are effectively soft
/// real-time tasks that require special lock-free care to minimize the odds of
/// packet loss, but other application threads (including the main thread) do
/// not normally require such precautions.
///
/// \internal
///
/// This struct is designed such that it fills up one small memory page (4KiB on
/// x86_64). The idea is that the command queue for each thread can be located
/// within one mmap()-allocated and mlock()ed block allocated by this thread.
//
// TODO: Figure out how the address of the command queue is passed back to the
//       main thread once this is done. Could just be a matter of filling up an
//       array in the udipe_context_t, decrementing an atomic each time, and
//       signaling a global futex once this is done.
typedef struct command_queue_s {
    // === First control block for worker/client synchronization ===

    /// Index within \link #command_queue_t::commands `commands`\endlink from
    /// which the worker thread will read next
    alignas(FALSE_SHARING_GRANULARITY) atomic_size_t worker_idx;

    /// Index within \link #command_queue_t::commands `commands`\endlink to
    /// which client threads will write next
    ///
    /// If this is equal to \link #command_queue_t::worker_idx
    /// `worker_idx`\endlink, then the queue is empty.
    atomic_size_t client_idx;

    /// Condition variable that client threads use to wait for the worker thread
    /// to process some commands
    ///
    /// Must be signaled with cond_signal(), not cond_broadcast(), to avoid a
    /// thundering herd effect.
    cnd_t client_condition;

    // === Second control block for client/client synchronization ===

    /// Mutex that a client thread must lock to submit commands
    ///
    /// Should not be locked frequently as the intent is for commands to be rare
    /// (use streams for frequent commands), and uncontended mutexes are pretty
    /// cheap. But it is best to keep it on a separate cache line so that the
    /// worker thread is partially shielded from the cache ping pong that occurs
    /// when multiple clients are fighting for this mutex.
    alignas(FALSE_SHARING_GRANULARITY) mtx_t client_mutex;

    // === Remaining false sharing atoms: worker/client synchronization ===

    /// Ring buffer that holds commands destined for worker thread processing
    ///
    /// The first control block indicates which entries from this array can be
    /// read by the worker thread and which can safely be overwritten by client
    /// threads then published through a `client_idx` increment.
    command_t commands[COMMAND_QUEUE_LEN];
} command_queue_t;
static_assert(alignof(command_queue_t) == FALSE_SHARING_GRANULARITY,
              "Should be a normal consequence of alignment directives, "
              "which themselves ensure that threads which should not fight "
              "over cache lines don't fight over cache lines");
static_assert(sizeof(command_queue_t) > EXPECTED_MIN_PAGE_SIZE/2,
              "Should use at least half a page (otherwise size can double)");
static_assert(sizeof(command_queue_t) <= EXPECTED_MIN_PAGE_SIZE,
              "Should not use more than a page (otherwise size should shrink)");

// TODO: Add queue operations


// TODO: Add unit tests
