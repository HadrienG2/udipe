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

#include <stdalign.h>
#include <stdatomic.h>
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
    uint32_t futex;
};

/// \}


/// \name Command queue
/// \{

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

// TODO: Also plan ahead a "shut down now" signal, but it should ignore
//       normal FIFO queuing. Maybe cross-thread SIGTERM could do?

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
        udipe_connect_options_t connect;
        udipe_disconnect_options_t disconnect;
        udipe_send_options_t send;
        udipe_recv_options_t recv;
        udipe_send_stream_options_t send_stream;
        udipe_recv_stream_options_t recv_stream;
    };

    // TODO: Consider some kind of QoS infrastructure so that e.g. IPBus slow
    //       control avoids using the same threads as acquisition. Can this just
    //       be a connection option, or does this need to be more?

    /// Type of work that was requested from the worker thread
    ///
    /// On a correctly initialized command, it will never have the placeholder
    /// value \ref UDIPE_NO_COMMAND.
    udipe_command_id_t id;
} command_t;

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
    // === First control block: Worker/Client synchronization ===

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

    // === Second control block: Client/Client synchronization ===

    /// Mutex that a client thread must lock to submit commands
    ///
    /// Should not be locked frequently as the intent is for commands to be rare
    /// (use streams for frequent commands), and uncontended mutexes are pretty
    /// cheap. But it is best to keep it on a separate cache line so that the
    /// worker thread is partially shielded from the cache ping pong that occurs
    /// when multiple clients are fighting for this mutex.
    alignas(FALSE_SHARING_GRANULARITY) mtx_t client_mutex;

    // === Next false sharing granules: Worker/Client synchronization ===

    /// Ring buffer that holds commands destined for worker thread processing
    ///
    /// The first control block indicates which entries from this array can be
    /// read by the worker thread and which can safely be overwritten by client
    /// threads then published through a `client_idx` increment.
    command_t commands[COMMAND_QUEUE_LEN];
} command_queue_t;

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