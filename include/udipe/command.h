#pragma once

//! \file
//! \brief Worker thread commands
//!
//! In `libudipe`, UDP communication is performed by sending commands to worker
//! threads, which asynchronously process them.
//!
//! The use of dedicated worker threads lets `libudipe` internally follow many
//! best practices for optimal UDP performance, without forcing your application
//! threads that interact with `libudipe` into the same discipline. But there is
//! a price to pay, which is that individual commands are rather expensive to
//! process as they involve inter-thread communication.
//!
//! This is why most commands that process a single UDP datagram come with a
//! streaming variant that processes an arbitrarily long stream of UDP
//! datagrams. For example, udipe_recv(), which receives a single UDP datagram,
//! comes with a udipe_recv_stream() streaming variant that processes an
//! arbitrary amount of incoming UDP datagrams using arbitrary logic defined by
//! a callback.
//!
//! These callbacks are directly executed by `libudipe` worker threads, which
//! means that they operate without requiring any inter-thread communication.
//! But this also means that they also require careful programming practices
//! when top performance is desired. See the documentation of individual
//! streaming functions for more advice on how to do this.
//!
//! Finally, all commands come with two associated API entry points, a
//! synchronous one and an asynchronous one. For example, the udipe_recv() entry
//! point, which receives a UDP datagram, comes with a udipe_start_recv()
//! asynchronous variant which starts receiving a UDP datagram but does not wait
//! for it to be ready before returning. When you use the asynchronous version,
//! you get a \ref udipe_future_t handle that you can later use to wait for the
//! operation to complete through the udipe_wait() function.
//!
//! The main intended use of asynchronous commands is to let you start an
//! arbitrary amount of udipe tasks, then do arbitrary other work, and finally
//! wait for some of your udipe tasks to complete. In cases where you want to
//! wait for multiple tasks to complete, consider using udipe_wait_all().

#include "context.h"
#include "pointer.h"
#include "visibility.h"

#include <assert.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>


/// \name Options and results of individual commands
/// \{

/// Communication direction(s)
///
/// When you create a \ref udipe_connection_t, you can specify whether you
/// intend to receive datagrams, send datagrams, or both.
///
/// The more restricted configurations that only allow one direction of data
/// exchange clarify intent and require fewer parameters to be set at
/// configuration time. They should also enjoy slightly faster connection setup,
/// though the performance of establishing connections should not matter in
/// realistic use cases.
typedef enum udipe_direction_e {
    /// Can receive datagrams from the remote peer
    UDIPE_IN = 0,

    /// Can send datagrams to the remote peer
    UDIPE_OUT,

    /// Can exchange datagrams with the remote peer in either direction
    UDIPE_INOUT
} udipe_direction_t;

/// IP address
///
/// As a UDP library, `libudipe` only supports IPv4 and IPv6 addresses, i.e.
/// `sockaddr_in` and `sockaddr_in6` in POSIX parlance. As a special extension,
/// `sa_family == 0` is interpreted as a user desire to use the default address,
/// which is defined in the appropriate field of \ref udipe_connect_options_t.
///
/// This union does not need to be accompanied by a tag because the `sockaddr`
/// types features a `sa_family` internal tag that enables IPv4/IPv6/default
/// disambiguation.
typedef union ip_address_u {
    struct sockaddr any;  ///< Used to safely query the `sa_family` field
    struct sockaddr_in v4;  ///< IPv4 address
    struct sockaddr_in6 v6;  ///< IPv6 address
} ip_address_t;

/// Boolean option with a nontrivial default value
///
/// This is used in circumstances where the default value for an option is not
/// `false` but e.g. "`true` if supported", "`true` if deemed worthwhile based
/// on system configuration", etc.
typedef enum udipe_bool_with_default_e {
    UDIPE_FALSE = 1,  ///< Set to `false`
    UDIPE_TRUE = 2,  ///< Set to `true`
    UDIPE_DEFAULT = 0,  ///< Use default value (depends on context)
} udipe_bool_with_default_t;

/// udipe_connect() parameters
///
/// This struct controls the parameters that can be tuned when establishing a
/// UDP connection. Like most configuration structs, it is designed such that
/// zero-initializing results in sane defaults, except for sending traffic where
/// you will need to set at least a `remote_address`.
///
/// \internal
///
/// Because IPv6 addresses are huge, there is no way this struct will ever fit
/// in a single cache line. Taking into account that establishing a connection
/// should be rare, and in the interest of not pessimizing the performance of
/// other command messages which do fit in one cache line, connection options
/// will therefore be passed to worker threads via heap-allocated blocks.
typedef struct udipe_connect_options_s {
    /// Default send timeout in nanoseconds, or 0 = no timeout
    ///
    /// This parameter must not be set if `direction` is \ref UDIPE_IN.
    ///
    /// The default is for send commands to block forever.
    //
    // TODO: Maps to SO_SNDTIMEO if set
    uint64_t send_timeout_ns;

    /// Default receive timeout in nanoseconds, or 0 = no timeout
    ///
    /// This parameter must not be set if `direction` is UDIPE_OUT.
    ///
    /// The default is for recv commands to block forever.
    //
    // TODO: Maps to SO_RCVTIMEO if set
    uint64_t recv_timeout_ns;

    /// Local interface
    ///
    /// If set to a non-`NULL` string, this indicates that you only want to send
    /// and receive traffic via the specified network interface.
    ///
    /// This parameter must be consistent with `local_address` (i.e.
    /// `local_interface` should be able to emit from the address specified in
    /// `local_address` if it is not a catch-all address) and `remote_address`
    /// (i.e. `remote_address` should be reachable from `local_interface`),
    /// otherwise you will not be able to send and receive datagrams.
    ///
    /// By default, the connection is not bound to any network interface.
    //
    // TODO: Maps to SO_BINDTODEVICE
    const char* local_interface;

    /// Local address
    ///
    /// If set to a non-default value, this indicates that you only want to send
    /// and receive traffic via the specified local IP address and port.
    ///
    /// This address must be of the same type as `remote_address` i.e. if one is
    /// an IPv4 address, then the other must be an IPv6 address, and vice versa.
    ///
    /// The default configuration sets this to IPv4 address 0.0.0.0 with port 0
    /// aka a randomly assigned port, unless `remote_address` is an IPv6 address
    /// in which case the default is IPv6 address `::` with port 0.
    //
    // TODO: Provide a way to get that auto-assigned port after connection, this
    //       is done via getsockname().
    ///
    /// This is appropriate if you want to send traffic and do not care which
    /// network interface and UDP port it goes through, or if you want to
    /// receive traffic and are ready to communicate the port number to your
    /// peer (as is common for e.g. local server testing).
    //
    // TODO: Maps to bind() if set + check type consistency with
    //       remote_address
    ip_address_t local_address;

    /// Remote address
    ///
    /// This is used to configure which remote IP address and port you want to
    /// exchange traffic with.
    ///
    /// This address must be of the same type as `local_address` i.e. if one is
    /// an IPv4 address, then the other must be an IPv6 address, and vice versa.
    ///
    /// The default configuration sets this to IPv4 address 0.0.0.0 with port 0
    /// aka any port, unless `local_address` is an IPv6 address in which case
    /// the default is IPv6 address `::` with port 0.
    ///
    /// This is always incorrect for sending traffic and must be changed to the
    /// address of the intended peer. When receiving traffic, it simply means
    /// that you are accepting traffic from any source address and port.
    //
    // TODO: Maps to connect() if set + check type consistency with
    //       local_address
    ip_address_t remote_address;

    /// Send buffer size
    ///
    /// This parameter must not be set if `direction` is \ref UDIPE_IN.
    ///
    /// It cannot be smaller than 1024 or larger than `INT_MAX`. In addition, on
    /// Linux, non-privileged processes cannot go above the limit configured in
    /// pseudo file `/proc/sys/net/core/wmem_max`.
    ///
    /// By default, the send buffer is configured at the OS' default size, which
    /// on Linux is itself configured through pseudo-file
    /// `/proc/sys/net/core/wmem_default` or the equivalent sysctl.
    ///
    /// \internal
    ///
    /// Bitfields are ab(used) there to ensure that attempting to set this to a
    /// value higher than `INT_MAX` is a compiler error.
    //
    // TODO: Implement by trying SO_SNDBUF then SO_SNDBUFFORCE
    unsigned send_buffer : 31;

    /// Receive buffer size
    ///
    /// This parameter must not be set if `direction` is \ref UDIPE_OUT.
    ///
    /// This cannot be smaller than 128 or larger than `INT_MAX`. In addition,
    /// on Linux, non-privileged processes cannot go above the limit configured
    /// in pseudo file `/proc/sys/net/core/rmem_max`.
    ///
    /// By default, the receive buffer is configured at the OS' default size,
    /// which on Linux is itself configured through pseudo-file
    /// `/proc/sys/net/core/rmem_default` or the equivalent sysctl.
    ///
    /// \internal
    ///
    /// Bitfields are ab(used) there to ensure that attempting to set this to a
    /// value higher than `INT_MAX` is a compiler error.
    //
    // TODO: Implement by trying SO_RCVBUF then SO_RCVBUFFORCE
    unsigned recv_buffer : 31;

    /// Communication direction(s)
    ///
    /// You can use this field to specify that you only intend to send or
    /// receive data. See \ref udipe_direction_t for more information.
    ///
    /// By default, the connection is configured to receive traffic only, as
    /// sending traffic requires a remote address and there is no good default
    /// for a remote address.
    //
    // TODO: Used to enforce usage validation by detecting invalid parameters
    //       and commands.
    udipe_direction_t direction;

    /// Enable Generic Segmentation Offload (GSO)
    ///
    /// This is a Linux UDP performance optimization that lets you send multiple
    /// UDP datagrams with a single `send` command. It roughly works by
    /// modifying the semantics of oversized `send` commands whose input buffer
    /// goes above the MTU, so that instead of failing they split the input
    /// buffer into multiple datagrams.
    ///
    /// The granularity at which a `send` operation is split into datagrams is
    /// controlled by the `gso_segment_size` option.
    ///
    /// By default, GSO if enabled if the host operating system supports it and
    /// disabled otherwise. This differs from the behavior of setting this to
    /// \ref UDIPE_TRUE, which makes connection setup fail if GSO is not
    /// supported.
    //
    // TODO: Sets UDP_SEGMENT in tandem with gso_segment_size
    udipe_bool_with_default_t enable_gso;

    /// Enable Generic Receive Offload (GRO)
    ///
    /// This is a Linux UDP performance optimization that lets you receive
    /// multiple UDP datagrams with a single `receive` command. It roughly works
    /// by modifying the semantics of oversized `receive` commands whose output
    /// buffer goes above the MTU, so that instead of receiving a single
    /// datagram they may receive multiple ones and concatenate their payloads.
    ///
    /// You cannot control the granularity of GRO, as it is given by the size of
    /// incoming datagrams (which must be of identical size), but you will be
    /// able to tell the datagram size at the end of the receive operation.
    ///
    /// By default, GRO if enabled if the host operating system supports it and
    /// left disabled otherwise. This differs from the behavior of intentionally
    /// setting this to \ref UDIPE_TRUE, which makes connection setup fail if
    /// GRO is not supported.
    //
    // TODO: Sets UDP_GRO
    udipe_bool_with_default_t enable_gro;

    /// GSO segment size
    ///
    /// This is the granularity at which the payload of a `send` command is
    /// split into separate UDP datagrams when the Generic Segmentation Offload
    /// feature is enabled.
    ///
    /// You must set it such that the resulting packets after adding UDP,
    /// IPv4/v6 and Ethernet headers remain below the network's path MTU.
    ///
    /// Linux additionally enforces that no more than 64 datagrams may be sent
    /// with a single `send` operation when GSO is enabled.
    ///
    /// This option can only be set when `enable_gso` is set to \ref UDIPE_TRUE,
    /// as it makes little sense otherwise and can lead to dangerous judgment
    /// errors where you think that your datagrams have one size but they
    /// actually have another payload size.
    ///
    /// By default, the GSO segment size is auto-tuned to the network path MTU
    /// that is estimated by the Linux kernel.
    //
    // TODO: Sets UDP_SEGMENT in tandem with enable_gso
    uint16_t gso_segment_size;

    /// Desired traffic priority
    ///
    /// Setting a priority higher than zero indicates that the operating system
    /// should attempt to process datagrams associated with this connection
    /// before those associated with other connections.
    ///
    /// On Linux, setting a priority of 7 and above requires `CAP_NET_ADMIN`
    /// privileges.
    ///
    /// By default, the priority is 0 i.e. lowest priority.
    //
    // TODO: Implement by setting SO_PRIORITY
    uint8_t priority;

    /// Allow datagrams to be handled by multiple worker threads
    ///
    /// This is only appropriate for higher-level protocols where UDP datagrams
    /// are independent from each other and the order in which they are sent and
    /// processed doesn't matter. But when that is the case, it can
    /// significantly improve performance in situations where the number of live
    /// network connections is small with respect to the amount of CPU cores.
    ///
    /// When this option is set, the callbacks that are passed to streaming
    /// commands like udipe_stream_send() must be thread-safe.
    ///
    /// By default, each connection is assigned to a single worker thread. This
    /// means that as long as commands associated with the connection only
    /// originate from a single client thread, packets will be sent and
    /// processed in a strict FIFO manner with respect to the order in which the
    /// network provided them. But do remember that UDP as a protocol does not
    /// provide ordering guarantees to allow e.g. switching between IP routes...
    //
    // TODO: This will require SO_REUSEPORT + fancier infrastructure in
    //       udipe_context as a connexion may now be associated with multiple
    //       worker threads. In any case, udipe_context will need a way to know
    //       which worker threads can handle which connexion.
    bool allow_multithreading;

    /// Request packet timestamps
    ///
    /// If enabled, each packet will come with a timestamp that indicates when
    /// the network interface processed it. This can be combined with
    /// application-side timestamps to estimate the kernel and application
    /// processing delay on the receive path.
    ///
    /// By default, timestamps are not requested.
    //
    // TODO: Implement by setting SO_TIMESTAMPNS and checking the
    //       `SCM_TIMESTAMPNS` cmsg.
    bool enable_timestamps;

    // TODO: Activer aussi IP_RECVERR et logger voire gérer les erreurs, cf man
    //       7 ip pour plus d'infos.

    // TODO: Activer périodiquement SO_RXQ_OVFL pour check l'overflow côté
    //       socket, puis le désactiver après réception du cmsg suivant.

    // TODO: Utiliser IP_MTU après binding pour autotuning de la segment size
    //       GRO, cf man 7 ip.

    // TODO: Dans udipe-config, creuser man 7 netdevice et man 7 rtnetlink pour
    //       la configuration device + check pseudofichiers mentionnés à la fin
    //       de man 7 socket, man 7 ip et man 7 udp pour la config kernel.

    // TODO: Quand j'aurai implémenté le multithreading des workers, utiliser
    //       SO_INCOMING_CPU pour associer le socket de chaque worker à son CPU.
} udipe_connect_options_t;

// TODO: Flesh out definitions, add docs
//
// TODO: Add max-size warnings in \internal, beware that they will not be the
//       same for options and results as for options we need to fit in the
//       internal command_t type.
typedef int udipe_connect_result_t;
typedef int udipe_disconnect_options_t;
typedef int udipe_disconnect_result_t;
typedef int udipe_send_options_t;
typedef int udipe_send_result_t;
typedef int udipe_recv_options_t;
typedef int udipe_recv_result_t;
typedef int udipe_send_stream_options_t;
typedef int udipe_send_stream_result_t;
typedef int udipe_recv_stream_options_t;
typedef int udipe_recv_stream_result_t;
typedef int udipe_reply_stream_options_t;
typedef int udipe_reply_stream_result_t;

/// \}


/// \name Genericity over the command type
/// \{

/// Variant payload from a \ref udipe_result_t
///
/// This union is normally paired with a \ref udipe_command_id_t that indicates
/// what command produced the result in question.
///
/// \internal
///
/// The size of this union should be kept such that \ref udipe_future_t fits in
/// one single cache line on all CPU platforms of interest. This currently
/// amounts to a size limit of 60B.
typedef union udipe_result_payload_u {
    udipe_connect_result_t connect;  ///< Result of udipe_connect()
    udipe_disconnect_result_t disconnect;  ///< Result of udipe_disconnect()
    udipe_send_result_t send;  ///< Result of udipe_send()
    udipe_recv_result_t recv;  ///< Result of udipe_recv()
    udipe_send_stream_result_t send_stream;  ///< Result of udipe_send_stream()
    udipe_recv_stream_result_t recv_stream;  ///< Result of udipe_recv_stream()
    udipe_reply_stream_result_t reply_stream;  ///< Result of udipe_reply_stream()
} udipe_result_payload_t;

/// Command identifier
///
/// This enumerated type has one positive value per `libudipe` command. It is
/// used to build types like \ref udipe_result_t that are generic over multiple
/// command types.
///
/// The zero sentinel variant \ref UDIPE_NO_COMMAND serves a dual purpose: it
/// enables zero-initialization and makes it possible to signal an absence of
/// result in situations where this is appropriate (like e.g. if a wait command
/// had a timeout and it passed without an operation completion notification).
typedef enum udipe_command_id_e {
    UDIPE_CONNECT = 1,  ///< udipe_connect()
    UDIPE_DISCONNECT,  ///< udipe_disconnect()
    UDIPE_SEND,  ///< udipe_send()
    UDIPE_RECV,  ///< udipe_recv()
    UDIPE_SEND_STREAM,  ///< udipe_send_stream()
    UDIPE_RECV_STREAM,  ///< udipe_recv_stream()
    UDIPE_REPLY_STREAM,  ///< udipe_reply_stream()
    UDIPE_NO_COMMAND = 0  ///< Sentinel value with no associated command
} udipe_command_id_t;

/// Generic result type
///
/// This type can encapsulate the result of any `libudipe` command, as well as
/// an absence of result.
typedef struct udipe_result_s {
    /// Result of the command, if any
    ///
    /// `command_id` can be used to check whether there is a result, and if so
    /// which command produced that result.
    udipe_result_payload_t payload;

    /// Command that returned this result, or \ref UDIPE_NO_COMMAND to denote an
    /// absence of result
    ///
    /// Even when one is using infaillible wait commands such as udipe_wait()
    /// with a `timeout` of 0, this field can be useful for debug assertions
    /// that a result is associated with the expected command type. It also
    /// enables having generic utilities that can handle all types of results.
    udipe_command_id_t command_id;
} udipe_result_t;

/// \}


/// \name Asynchronous operation futures
/// \{

/// Asynchronous operation future
///
/// A pointer to this opaque struct is built by every asynchronous command
/// (those whose name begins with `udipe_start_`). It can be used to query
/// whether the associated asynchronous operation is done executing, wait for it
/// to finish executing, and collect the result.
///
/// Its content is an opaque implementation detail of `libudipe` that you should
/// not attempt to read or modify.
///
/// It cannot be used again after the completion of the operation has been
/// successfully awaited using a function like udipe_wait().
typedef struct udipe_future_s udipe_future_t;

/// Truth that an asynchronous operation is finished
///
/// If this returns true, then a call to udipe_wait() for this future is
/// guaranteed to return the result immediately without blocking this thread.
///
/// If you find yourself needing to use this function for periodical polling
/// because you are also waiting for some events outside of `libudipe`, please
/// contact the `libudipe` developers. There _may_ be a way to provide a uniform
/// blocking wait interface for you, at the expense of reducing portability or
/// exposing more `libudipe` implementation details.
///
/// \param future must be a future that was returned by an asynchronous entry
///               point (those whose name begins with `udipe_start_`), and that
///               has not been successfully awaited yet.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
bool udipe_done(const udipe_future_t* future);

/// Wait for the result of an asynchronous operation
///
/// This command will wait until the asynchronous operation designated by
/// `future` completes or the timeout delay specified by `timeout_ns` (see
/// below) elapses.
///
/// If the asynchronous operation completes before the timeout, then the output
/// \ref udipe_result_t will have the nonzero `command_id` of the command that
/// was originally submitted. In this case, the future object is destroyed and
/// must not be used again.
///
/// If the asynchronous operation takes longer than the specified timeout to
/// complete, then this function will return an invalid result (with
/// `command_id` set to \ref UDIPE_NO_COMMAND). In this case, the future object
/// remains valid and can be awaited again.
///
/// \param future must be a future that was returned by an asynchronous entry
///               point (those whose name begins with `udipe_start_`), and that
///               has not been successfully awaited yet.
/// \param timeout_ns specifies a minimal time in nanoseconds during which
///                   udipe_wait() will wait for the asynchronous operation to
///                   complete. The actual delay will be rounded up to the next
///                   multiple of the system scheduler clock granularity and may
///                   be affected by system task scheduling overheads. If a
///                   delay of UINT64_MAX is specified, then udipe_wait() will
///                   not return until the specified operation completes.
///
/// \returns The result of the asynchronous operation if it completes, or an
///          invalid result (with `command_id` set to \ref UDIPE_NO_COMMAND) if
///          the operation did not complete before the timeout was reached.
//
// TODO: Wait for an asynchronous task to finish and fetch its result.
//       Recycle the future into the host thread's local cache.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
udipe_result_t udipe_wait(udipe_future_t* future, uint64_t timeout_ns);

/// Wait for the result of multiple asynchronous operations
///
/// This is a collective version of udipe_wait() that waits for multiple futures
/// to complete, or for the timeout to elapse. The output boolean indicates
/// whether all futures have completed or the request has timed out.
///
/// If the result is `true`, indicating full completion, then it is guaranteed
/// that the operations associated with all futures have completed. Therefore
/// none of the output `results` have their `command_id` field set to \ref
/// UDIPE_NO_COMMAND, and none of the input `futures` can be used afterwards.
///
/// If the result is `false`, indicating that the wait has timed out, then you
/// must check each entry of `result` to see which operations have completed. By
/// the same logic as udipe_wait(), those which have **not** completed will have
/// the `command_id` field of their \ref udipe_result_t set to \ref
/// UDIPE_NO_COMMAND.
///
/// As a reminder, futures associated with operations that have completed have
/// been destroyed and must not be used again.
///
/// \param num_futures must indicate the number of elements in the `futures`
///                    and `results` arrays.
/// \param futures must be an array of length `num_futures` containing futures
///                that have not been successfully awaited yet.
/// \param results must be an array of length `num_futures` of \ref
///                udipe_result_t. The initial value of these results does not
///                matter, they will be overwritten.
/// \param timeout_ns works as in udipe_wait().
///
/// \returns `true` if all asynchronous operations completed, and `false` if the
///          operation did not complete before the timeout was reached.
//
// TODO: Implement through repeated calls to udipe_wait_any() or an optimized
//       version thereof that uses an internal API to recycle internal fds,
//       epoll context, etc.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
bool udipe_wait_all(size_t num_futures,
                    udipe_future_t* futures[],
                    udipe_result_t results[],
                    uint64_t timeout_ns);

/// Wait for the result of at least one asynchronous operation
///
/// This is a collective version of udipe_wait() that waits for at least one
/// future to complete, or for the timeout to elapse. The result indicates how
/// many futures have completed, if it is 0 then the request has timed out.
///
/// Aside from the obvious difference that it waits for 1+ operation rather than
/// all of them, this function is used a lot like udipe_wait_all(), with a few
/// API tweaks. We will therefore mainly focus on the differences, and let you
/// check the documentation of udipe_wait_all() where they work identically.
///
/// \param num_futures works as in udipe_wait_all() except it also indicates
///                    the size of the `result_positions` array if there is one.
/// \param futures works as in udipe_wait_all()
/// \param results works as in udipe_wait_all()
/// \param result_positions can be `NULL`. If it is set, then it must point to
///                         an array of `size_t` of length `num_futures`.
///                         This array will be used to record the positions of
///                         the futures that did reach completion, the return
///                         value of the function will tell how many entries
///                         were filled this way.
/// \param timeout_ns works as in udipe_wait().
///
/// \returns the number of operations that have completed, which will be nonzero
///          if at least one operation has completed and zero otherwise.
//
// TODO: Implement by first checking futexes for completion, then converting the
//       remaining futexes to file descriptors using FUTEX_FD, then polling
//       these fds using epoll(), then discarding everything. Consider having an
//       internal variant that keeps the context around instead, used by the
//       implementation of udipe_wait_any().
UDIPE_PUBLIC
UDIPE_NON_NULL_SPECIFIC_ARGS(2, 3)
size_t udipe_wait_any(size_t num_futures,
                      udipe_future_t* futures[],
                      udipe_result_t results[],
                      size_t* result_positions,
                      uint64_t timeout_ns);

/// \}


/// \name Worker thread commands
/// \{

// TODO: document and implement
//
// TODO: Explain somewhere that a udipe connection is mostly like a POSIX socket
//       but may be implemented using multiple sockets under the hood.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* udipe_start_connect(udipe_context_t* context,
                                    udipe_connect_options_t options);

// TODO: document
static inline
UDIPE_NON_NULL_ARGS
udipe_connect_result_t udipe_connect(udipe_context_t* context,
                                     udipe_connect_options_t options) {
    udipe_future_t* future = udipe_start_connect(context, options);
    assert(future);
    udipe_result_t result = udipe_wait(future, 0);
    assert(result.command_id == UDIPE_CONNECT);
    return result.payload.connect;
}

// TODO: document and implement
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* udipe_start_disconnect(udipe_context_t* context,
                                       udipe_disconnect_options_t options);

// TODO: document
static inline
UDIPE_NON_NULL_ARGS
udipe_disconnect_result_t udipe_disconnect(udipe_context_t* context,
                                           udipe_disconnect_options_t options) {
    udipe_future_t* future = udipe_start_disconnect(context, options);
    assert(future);
    udipe_result_t result = udipe_wait(future, 0);
    assert(result.command_id == UDIPE_DISCONNECT);
    return result.payload.connect;
}

// TODO: document and implement
//
// TODO: Should have GSO-like semantics, i.e. if you give a large enough buffer
//       then multiple datagrams may be sent. If GSO is disabled, then it just
//       sends a single datagram. Do not attempt to send more than 64 datagrams.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* udipe_start_send(udipe_context_t* context,
                                 udipe_send_options_t options);

// TODO: document
static inline
UDIPE_NON_NULL_ARGS
udipe_send_result_t udipe_send(udipe_context_t* context,
                               udipe_send_options_t options) {
    udipe_future_t* future = udipe_start_send(context, options);
    assert(future);
    udipe_result_t result = udipe_wait(future, 0);
    assert(result.command_id == UDIPE_SEND);
    return result.payload.send;
}

// TODO: document and implement
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* udipe_start_recv(udipe_context_t* context,
                                 udipe_recv_options_t options);

// TODO: document
//
// TODO: Should have GSR-like semantics, i.e. if you give a large enough buffer
//       then multiple datagrams may be received, and there will be anciliary
//       data telling you how large the inner segments are. If GSO is disabled,
//       then it just receives a single datagram.
static inline
UDIPE_NON_NULL_ARGS
udipe_recv_result_t udipe_recv(udipe_context_t* context,
                               udipe_recv_options_t options) {
    udipe_future_t* future = udipe_start_recv(context, options);
    assert(future);
    udipe_result_t result = udipe_wait(future, 0);
    assert(result.command_id == UDIPE_RECV);
    return result.payload.recv;
}

// TODO: document and implement
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* udipe_start_send_stream(udipe_context_t* context,
                                        udipe_send_stream_options_t options);

// TODO: document
static inline
UDIPE_NON_NULL_ARGS
udipe_send_stream_result_t
udipe_send_stream(udipe_context_t* context,
                  udipe_send_stream_options_t options) {
    udipe_future_t* future = udipe_start_send_stream(context, options);
    assert(future);
    udipe_result_t result = udipe_wait(future, 0);
    assert(result.command_id == UDIPE_SEND_STREAM);
    return result.payload.send_stream;
}

// TODO: document and implement
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* udipe_start_recv_stream(udipe_context_t* context,
                                        udipe_recv_stream_options_t options);

// TODO: document
static inline
UDIPE_NON_NULL_ARGS
udipe_recv_stream_result_t
udipe_recv_stream(udipe_context_t* context,
                  udipe_recv_stream_options_t options) {
    udipe_future_t* future = udipe_start_recv_stream(context, options);
    assert(future);
    udipe_result_t result = udipe_wait(future, 0);
    assert(result.command_id == UDIPE_RECV_STREAM);
    return result.payload.recv_stream;
}

// TODO: document and implement
// TODO: This is sort of the combination of a send_stream() and a
//       recv_stream(). It combines an incoming and outgoing connection (which
//       may be the same connection) in such a way that for each incoming
//       datagram on one connection, you can send a datagram to the other
//       connection, which is derived from the incoming one.
UDIPE_PUBLIC
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* udipe_start_reply_stream(udipe_context_t* context,
                                         udipe_reply_stream_options_t options);

// TODO: document
static inline
UDIPE_NON_NULL_ARGS
udipe_reply_stream_result_t
udipe_reply_stream(udipe_context_t* context,
                   udipe_reply_stream_options_t options) {
    udipe_future_t* future = udipe_start_reply_stream(context, options);
    assert(future);
    udipe_result_t result = udipe_wait(future, 0);
    assert(result.command_id == UDIPE_REPLY_STREAM);
    return result.payload.reply_stream;
}

/// \}
