# Scope

## Motivation

As network links keep getting faster, sustaining incoming packet flow and
saturating outgoing bandwidth keeps getting harder. This is especially true for
UDP communications, where untimely incoming traffic processing and improper
outgoing traffic pacing can easily cause data loss.

Hardware and operating systems have organically grown a large amount of features
to help with this challenge. But making the most of these features requires...

- Careful operating system configuration (buffer sizes at several layers of the
  network stack, number and kind of hardware RX/TX queues + associated load
  balancing policies, careful CPU pinning of hardware IRQs and associated
  software post-processing, all with due attention paid to NUMA and cache
  locality concerns...)
- Explicit software support, i.e. a simple POSIX server that does just a
  `socket()` + `bind()` + `recv()` loop will either not make the most of these
  features or not benefit at all. For optimal performance, use of special APIs
  (like `io_uring` on Linux) and configuration (like the many `setsockopt()`
  tunables from `man 7 socket`, `man 7 ip` and `man 7 udp`) is often required.
- Willingness to either cut off support for older OS/hardware or add complexity
  in the form of fallback code paths that handle the absence of newer features.

Finally, the associated knowledge is not neatly collected in a nice centralized
place, like a One True Networking book that every expert should read and keep at
hand. It is rather scattered across many articles on the Internet, each of which
is laser-focused on a particular fragment of the networking stack, and many of
which are outdated with respect to the latest hardware and OS developments.

When all of this is combined, it is no wonder that building efficient UDP
networking applications keeps getting harder dans harder. The goal of `udipe` is
to make this easier, at least for some categories of applications.


## Target audience

As mentioned above, `udipe` is not a general-purpose networking library. It is
focused on UDP communication[^udp-only], and many of its design choices are
biased towards the needs of physics data acquisition systems. In those
systems...

- UDP is mainly used for the purpose of sending data out of electronics cards
  because TCP is too costly to implement on FPGAs. Therefore acquisition
  computers are mainly concerned with handling large volumes of _incoming_ UDP
  traffic.
    - Some UDP control messages do get sent sometimes, however, and having a
      fast send path allows `udipe` benchmarks not to depend on e.g. `iperf`. So
      the send path should still be pretty fast, if not optimally so.
- CPU processing of incoming packets is a common bottleneck, i.e. often the
  packet correctly gets from FPGA A to the NIC of computer B then gets lost
  because one buffer filled up on the path from NIC to OS kernel to user
  processing thread.
- The number of peers that are sending data to a particular server can be small,
  reducing the effectiveness of hardware/OS automatic load balancing and
  parallelization.
- The main production platform is dedicated Linux servers, which are managed by
  the acquisition system's development team. This means that...
    - Recent versions of Linux distributions and packages can easily be used.
    - System settings can easily be tuned for optimal application performance.
    - Virtualization layers, which add overhead and make performance harder to
      control and reason about, can easily be avoided.
    - Shared resources like networked filesystems can easily be taken off the
      application's hot path, further increasing control on system performance.
    - Reserving a large amount of system resources (e.g. several network
      interfaces, many CPU cores...) for the nearly exclusive use of a
      particular application is fair game.
    - Supporting Windows or macOS is not a strong requirement, merely a
      nice-to-have convenience for local testing on developer machines.
- The developer audience presents a relatively high willingness to use unusual
  network APIs in the pursuit of optimal performance.

To the developers and maintainers of such systems, the `udipe` project wants to
provides two things:

- A C11[^gnu-extensions] library called `libudipe` that, given some
  configuration (from sane defaults to very detailed manual tweaking for a
  specific workload), sets up a high-performance UDP network pipeline in your
  application.
- A Linux system administration tool called `udipe-setup` that ingests the same
  configuration as `libudipe` and automatically configures the underlying system
  optimally for the intended network workload. This tool is supplemented by a
  `udipe-setup.service` systemd service to easily enable automatic boot-time
  machine configuration.


## System support

As high-performance networking primitives are OS-specific and evolve fast even 
for a particular OS, supporting many OSes and OS versions is costly. As a
resource-constrained project, `udipe` opts to provide multiple tiers of support:

1. The main production platform is the latest Ubuntu LTS release, 24.04 at the
   time of writing. On this Linux distribution, `udipe` aims to achieve peak
   performance, with an installation that is as easy as possible (minimal
   deviation from the standard distribution package configuration). All `udipe`
   components are frequently built, tested and benchmarked on this platform.
2. R&D is carried out using newer rolling release Linux distributions like Arch
   Linux, to evaluate new kernel/software features and assess the benefits of
   mid-cycle production platform updates like `-hwe` kernels or `liburing`
   updates. `udipe` is regularly tested on these platforms, but less extensively
   than on the production platform. On these distributions `udipe` may not yet
   leverage all the latest kernel and library network performance features.
3. The minimal support tier is systems that provide the basic POSIX UDP
   interface (`socket()` + `bind()` + `connect()` +
   `send()`/`sendto()`/`sendmsg()` + `recv()`/`recvfrom()`/`recvmsg()`). The
   goal here is that it should be possible to build and run `libudipe`-based
   applications on these systems and they should behave correctly, but...
    - No effort is made to keep the build process easy, so e.g. some toolchain
      and library upgrades may be required.
    - The resulting application may not perform optimally because old network
      performance optimizations that have been superseded by newer ones (e.g.
      `sendmmsg()`/`recvmmsg()` which is mostly replaced by `io_uring`) may not
      be supported by `udipe`.
    - The `udipe-setup` system configuration assistant may not be usable
      (especially on non-Linux platforms like macOS).


[^gnu-extensions]: ...with occasional use of GNU extensions supported by GCC and
                   clang in situations where standard C cannot express the
                   desired semantics.

[^udp-only]: Non-UDP primitive, e.g. filesystem I/O or signal handling helpers,
             may be provided to expose OS features like
             [`sendfile()`](https://www.man7.org/linux/man-pages/man2/sendfile.2.html)
             or
             [`ppoll()`](https://www.man7.org/linux/man-pages/man2/ppoll.2.html)
             that cannot be used without a hook into the low-level OS I/O 
             primitives that `udipe` abstract away. But these operations are not
             the core focus of `udipe`.
