#pragma once

//! \file
//! \brief Connection-related definitions
//!
//! Like all other udipe commands, the udipe_connect() and udipe_disconnect()
//! commands are defined in \ref command.h. But they come with a fairly large
//! amount of related definitions, which have been extracted into this dedicated
//! header the interest of code clarity.

#include "duration.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __unix__
    #include <netinet/in.h>
    #include <sys/socket.h>
#elif defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#endif


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
    /// Default send timeout in nanoseconds or 0 = no timeout / wait forever
    ///
    /// This parameter must not be set if `direction` is \ref UDIPE_IN.
    ///
    /// The default is to wait indefinitely for datagrams to be sent. See \ref
    /// udipe_duration_ns_t for more info on timeout semantics.
    //
    // TODO: Maps to SO_SNDTIMEO if set
    udipe_duration_ns_t send_timeout;

    /// Default receive timeout in nanoseconds or 0 = no timeout / wait forever
    ///
    /// This parameter must not be set if `direction` is UDIPE_OUT.
    ///
    /// The default is to wait indefinitely for datagrams to be received. See
    /// \ref udipe_duration_ns_t for more info on timeout semantics.
    //
    // TODO: Maps to SO_RCVTIMEO if set
    udipe_duration_ns_t recv_timeout;

    // TODO: Add `udipe_future_t* after` option to chain this after other ops.

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
    /// If you use the udipe_start_connect() asynchronous version of
    /// udipe_connect(), then the string targeted by this pointer must not be
    /// modified or liberated until the future associated with
    /// udipe_start_connect() had been awaited via udipe_finish().
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

    /// Reserved for future use, leave at `false` for now
    ///
    bool reserved : 1;

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

    /// Enable Generic Receive Offload (GRO)
    ///
    /// Setting this to `true` enables GRO, a Linux UDP performance optimization
    /// that basically does the reverse of GSO : concatenate input packets as
    /// long as they have the same size and there's room in the buffer passed to
    /// `recvmsg()`, eventually return and tell how big the input segments were
    /// in case the user needs to re-split the result into the original datagram
    /// payloads.
    //
    // TODO: Sets UDP_GRO
    bool enable_gro : 1;

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

    /// GSO segment size (nonzero to enable)
    ///
    /// Setting this to a nonzero value enables GSO, a Linux UDP performance
    /// optimization (now available on some other operating systems) that lets
    /// you send multiple UDP datagrams with a single `send` command.
    ///
    /// It works by splitting an oversized `send` request into smaller segments
    /// of the specified `gso_segment_size`, until either all input bytes are
    /// sent or only a remainder of smaller size is left. In the latter case,
    /// the remainder is sent as a smaller datagram.
    ///
    /// You must set this parameter such that the UDP datagrams that result
    /// after adding UDP, IPv4/v6 and Ethernet headers to a segment remain below
    /// the network's path MTU.
    ///
    /// Linux additionally enforces that no more than 64 datagrams may be sent
    /// with a single `send` operation when GSO is enabled.
    ///
    /// Setting this to 0 disables GSO, falling back to the standard Unix send
    /// semantics where the kernel tries to send the whole payload as a single
    /// datagram (and fails if it ends up being larger than the MTU).
    //
    // TODO: Sets UDP_SEGMENT
    uint16_t gso_segment_size;

    /// Desired traffic priority
    ///
    /// Setting a priority higher than zero indicates that the operating system
    /// should attempt to process datagrams associated with this connection
    /// before those associated with other connections.
    ///
    /// On Linux, setting a priority of 7 requires `CAP_NET_ADMIN` privileges.
    /// At the time of writing, this is the only privileged priority level
    /// supported by udipe as user demand for privileged priority seemed weak to
    /// nonexistent and having fewer priority levels simplified the udipe
    /// implementation. However, the udipe project is open to supporting more
    /// privileged priority levels on user demand, just open an issue on the
    /// udipe code repo and describe your use case!
    ///
    /// By default, the priority is 0 i.e. lowest priority.
    //
    // TODO: Implement by setting SO_PRIORITY
    unsigned priority : 3;

    // TODO: Activer aussi IP_RECVERR, et logger voire gérer les erreurs, cf man
    //       7 ip pour plus d'infos. A utiliser en combinaison avec
    //       getsockopt(SO_ERROR).

    // TODO: Activer périodiquement SO_RXQ_OVFL pour check l'overflow côté
    //       socket, puis le désactiver après réception du cmsg suivant + cf
    //       ioctls udp et
    //       <https://www.kernel.org/doc/html/latest/networking/statistics.html>.

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
