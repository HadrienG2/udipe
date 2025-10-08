# `udipe`: Solving the riddle of high-throughput UDP[^credits]

![UDiPus is facing the sphinx except the sphinx is lots of UDP traffic and all
the dead people have done it horribly wrong](doc/src/udipe.jpg)

This is `udipe`, a toolchain for building high-throughput UDP networking systems
on Linux. It is composed of two components that share a common configuration:

- `libudipe` is a C11 networking library. It eases the development of
  applications that follow the best practices for high-throughput UDP on modern
  Linux systems, by abstracting these out behind a reasonably easy-to-use
  interface.
- `udipe-setup` is a system administration utility that can be used to
  automatically apply the system configuration tuning that a `libudipe`-based
  application needs for optimal performance. It comes with a matching
  `udipe-setup.service` systemd service that lets you automatically reapply this
  tuning on every system boot.

Find out more in [the documentation](https://udipe-20b9cf.pages.in2p3.fr/).


## Licensing

This software is subject to the terms of the [Mozilla Public License, v.
2.0](LICENSE.md).

If you are not familiar with it, the key difference with most other open-source
licenses is that it has weak copyleft with file-based granularity: when you
distribute software that is based on a modified version of `libudipe`, you are
required to also redistribute your modified versions of all source code files
from the `libudipe` source code under MPLv2.


---

[^credits]: Illustration from ["Myths and Legends of All Nations; Famous Stories
            from the Greek, German, English, Spanish, Scandinavian, Danish,
            French, Russian, Bohemian, Italian and other
            sources"](https://www.gutenberg.org/files/20740/20740-h/20740-h.htm),
            trans./ed. Logan Marshall (1914)
