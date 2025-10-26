#pragma once

//! \file
//! \brief Connection-related definitions
//!
//! Like all other udipe commands, the udipe_connect() and udipe_disconnect()
//! commands are defined in \ref command.h. But they come with a fairly large
//! amount of related definitions, which have been extracted into this dedicated
//! header the interest of code clarity.

#include <netinet/in.h>
#include <stdint.h>
#include <sys/socket.h>


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
/// `sa_family == 0` is interpreted as requesting some default address.
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
/// This is needed in circumstances where the default value for an option is not
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
/// will therefore be passed to worker threads via a pointer indirection.
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
