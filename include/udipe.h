#pragma once

//! \file
//! \brief Catch-all header for the udipe library.
//!
//! Consider including more focused `udipe/xyz.h` headers for clarity.

//! \mainpage `libudipe` API reference
//!
//! This is the API reference documentation of `libudipe`, a library for
//! high-performance UDP communication. It supplements the <a
//! href="https://hadrieng2.github.io/udipe/">top-level udipe documentation</a>,
//! which is mandatory reading, with extra information about the various types
//! and functions exposed by `libudipe`.
//!
//! By default, it only covers the public API available to end users of the
//! library, which corresponds to the header files available in the `include/`
//! directory of the `libudipe` source tree.
//!
//! If you are working on `libudipe` itself, you may want to turn on the
//! `-DUDIPE_BUILD_DOXYGEN_INTERNAL=1` build option, in order to additionally
//! document the internal components of the library within the `src/` directory
//! of the `libudipe` source tree.

// Not including udipe/benchmarks.h as it isn't meant for end user consumption
#include "udipe/buffer.h"
#include "udipe/command.h"
#include "udipe/connect.h"
#include "udipe/context.h"
#include "udipe/future.h"
#include "udipe/log.h"
#include "udipe/pointer.h"
#include "udipe/result.h"
// Not including udipe/unit_tests.h as it isn't meant for end user consumption
#include "udipe/time.h"
#include "udipe/visibility.h"
