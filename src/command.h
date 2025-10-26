#pragma once

//! \file
//! \brief Command queue
//!
//! This code module implements the FIFO message queue that is used to to submit
//! commands from client threads that use `libudipe` via a \ref udipe_context_t
//! to a particuler worker thread managed by `libudipe`. Each worker thread gets
//! such a queue, and the \ref udipe_context_t takes care of balancing the
//! incoming workload between worker threads.

#include <udipe/command.h>

#include "arch.h"

#include <assert.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <threads.h>


/// \name Asynchronous operation futures
/// \{

/// \copydoc udipe_future_t
struct udipe_future_s {
    /// Result of the command, if any
    ///
    /// Once the underlying command is done running to completion, its result
    /// will be written down to this field.
    alignas(FALSE_SHARING_GRANULARITY) udipe_result_payload_t payload;

    /// Futex that can be used to wait for the command to run to completion
    ///
    /// It is initialized to \ref UDIPE_NO_COMMAND and used as follows:
    ///
    /// - The client thread waits for it to move away from \ref
    ///   UDIPE_NO_COMMAND, with acquire ordering upon completion.
    /// - Once the worker thread is done, it sets `payload` to the command's
    ///   result, then this futex to the appropriate \ref udipe_command_id_t
    ///   with release ordering, and finally it wakes the futex.
    _Atomic uint32_t futex;
};
static_assert(alignof(udipe_future_t) == FALSE_SHARING_GRANULARITY);
static_assert(sizeof(udipe_future_t) == FALSE_SHARING_GRANULARITY);
static_assert(
    offsetof(udipe_future_t, futex) + sizeof(uint32_t) <= CACHE_LINE_SIZE
);
// Also check result layout while we're at it
static_assert(sizeof(udipe_result_t) <= CACHE_LINE_SIZE);

/// \}


/// \name Command queue
/// \{

/// Reference-counted connection options
///
/// \ref udipe_connect_options_t is rather large, and it unfortunately has to be
/// because IPv6 addresses are huge. We would rather not have this big struct
/// bloat up the internal union of the \ref command_t struct that is sent to
/// worker threads on every request.
///
/// But on the flip side, connecting to a new host should be a rare event. Which
/// means that it is fine to use some special allocation policy for the
/// connection options struct that is a bit less optimal from a performance or
/// liveness perspective.
///
/// We therefore use a small pool of preallocated reference-counted connection
/// options within \ref udipe_context_t such that...
///
/// - Each connection attempt from a client thread takes one of these structs if
///   available, or blocks if none is available, using the synchronization
///   protocol described in the internal documentation of \ref udipe_context_t.
/// - If this is a parallel connection that is destined to be serviced by
///   multiple worker threads, then a shared struct is allocated for all of
///   them, and reference counting is used to synchronize worker threads with
///   each other in the subsequent struct liberation process.
typedef struct shared_connect_options_s {
    /// Reference count
    ///
    /// This should be zero upon allocation if correct synchronization was used
    /// by prior worker threads. It is initialized to the number of worker
    /// threads that will consume this struct (1 for sequential connections,
    /// >= 1 for parallel connections) and will go down until it reaches zero.
    ///
    /// If this refcount is initially 1 (which can be checked with a relaxed
    /// load and is the case for all sequential connections), then the
    /// consumer worker thread can take the following fast path:
    ///
    /// - Read the `options` member
    /// - Set this refcount to zero with a relaxed store
    /// - Liberate this struct as directed in the documentation of \ref
    ///   udipe_context_t.
    ///
    /// If this refcount is not initially 1, then the standard reference
    /// counting pattern must be followed instead.
    ///
    /// - Read the `options` member
    /// - Decrement this refcount with a relaxed fetch_sub()
    /// - If the refcount reaches zero (i.e. fetch_sub() returns an initial
    ///   value of 1), then liberate this struct as directed in the
    ///   documentation of \ref udipe_context_t.
    ///   - Currently, this is done using a release atomic operation, so there
    ///     is no need for an additional release fence here, but if the release
    ///     procedure changes then a release fence will need to be added. It
    ///     therefore seems more prudent to make sure all of this is implemented
    ///     in the same function, with an appropriate warning comment.
    alignas(FALSE_SHARING_GRANULARITY) atomic_size_t reference_count;

    /// Connection options
    ///
    /// If the reference count is greater than 1, this struct is visible by
    /// multiple worker threads and must not be modified. This means that
    /// default values must be normalized into final settings within the client
    /// thread before this struct is sent to worker threads.
    udipe_connect_options_t options;
} shared_connect_options_t;
static_assert(alignof(shared_connect_options_t) == FALSE_SHARING_GRANULARITY);
static_assert(sizeof(shared_connect_options_t) == FALSE_SHARING_GRANULARITY);

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
///   granule.
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
    /// Completion future, to be filled up and signaled upon command completion
    ///
    /// This pointer cannot be `NULL`.
    alignas(FALSE_SHARING_GRANULARITY) udipe_future_t* completion;

    /// Parameters that are appropriate for this command type
    ///
    /// The value of `id` indicates which of this union's variants is valid.
    union {
        shared_connect_options_t* connect;
        udipe_disconnect_options_t disconnect;
        // TODO: Add and implement
        /*udipe_send_options_t send;
        udipe_recv_options_t recv;
        udipe_send_stream_options_t send_stream;
        udipe_recv_stream_options_t recv_stream;*/
    };

    // TODO: Consider some kind of QoS infrastructure so that e.g. IPBus slow
    //       control avoids using the same threads as acquisition. Can this just
    //       be a connection option, or does it need to be more?

    /// Type of work that was requested from the worker thread
    ///
    /// On a correctly initialized command, it will never have the placeholder
    /// value \ref UDIPE_NO_COMMAND.
    udipe_command_id_t id;
} command_t;
static_assert(alignof(command_t) == FALSE_SHARING_GRANULARITY);
static_assert(sizeof(command_t) == FALSE_SHARING_GRANULARITY);
static_assert(
    offsetof(command_t, id) + sizeof(udipe_command_id_t) <= CACHE_LINE_SIZE
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

    // === Remaining false sharing granules: worker/client synchronization ===

    /// Ring buffer that holds commands destined for worker thread processing
    ///
    /// The first control block indicates which entries from this array can be
    /// read by the worker thread and which can safely be overwritten by client
    /// threads then published through a `client_idx` increment.
    command_t commands[COMMAND_QUEUE_LEN];
} command_queue_t;
static_assert(alignof(command_queue_t) == FALSE_SHARING_GRANULARITY);
static_assert(sizeof(command_queue_t) > EXPECTED_MIN_PAGE_SIZE/2);
static_assert(sizeof(command_queue_t) <= EXPECTED_MIN_PAGE_SIZE);

/// \}


/// \name Unit tests
/// \{

#ifdef UDIPE_BUILD_TESTS
    /// Unit tests for commands
    ///
    /// This function runs all the unit tests for commands. It must be called
    /// within the scope of with_logger().
    void command_unit_tests();
#endif

/// \}