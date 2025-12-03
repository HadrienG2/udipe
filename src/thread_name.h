#pragma once

//! \file
//! \brief OS-independent thread naming
//!
//! This code module abstracts away differences between the thread naming
//! primitives of supported operating systems.

#include <udipe/pointer.h>

#include <stddef.h>


/// Maximum thread name length that is guaranteed to be supported by all
/// udipe-supported operating systems
///
/// See set_thread_name() for more information about the various restrictions
/// that apply to thread names.
#define MAX_THREAD_NAME_LEN ((size_t)15)

/// Set the name of the calling thread
///
/// To accomodate the limitations of all supported operating systems and ensure
/// that thread names will not be mangled by any of them, said names must
/// honor the following restrictions:
///
/// - A thread name cannot be empty (but does not need to be unique)
/// - Only use printable ASCII code points except for the trailing NUL. No
///   Unicode tricks allowed here.
/// - Feature exactly one occurence of NUL at the end, like all C strings.
/// - Be no longer than \ref MAX_THREAD_NAME_LEN bytes, excluding the
///   aforementioned trailing NUL.
///
/// Since \ref MAX_THREAD_NAME_LEN is very short (only 15 useful ASCII chars on
/// Linux), it is recommended to simply give the thread a summary identifier
/// whose semantics are further detailed via logging.
///
/// For example, a backend that spawns one thread per connection could name its
/// threads something like `udp_cx_89ABCDEF`, with a 32 bit hex identifier at
/// the end which is just the index of the connexion thread in some internal
/// table. When the connection thread is created, it emits an `INFO` log message
/// announcing that it is in charge of handling a connexion with certain
/// properties, and thus the user should be able to tell which thread handles
/// which peer(s).
///
/// If the user decides to be difficult by using multiple udipe contexts at the
/// same time, we must detect this and switch to a less optimal naming
/// convention that handles multiple contexts like one that is based on TID
/// (`udp_th_89ABCDEF`), otherwise we'll get multiple threads with the same name
/// which is quite bad for ergonomics.
///
/// This function must be called within the scope of with_logger().
///
/// \param name is a thread name that must follow the constraints listed above.
UDIPE_NON_NULL_ARGS
void set_thread_name(const char* name);

/// Get the name of the calling thread
///
/// Although udipe names its worker threads under the constraints spelled out in
/// the documentation of set_thread_name(), callers of this function should be
/// ready for names that do not follow these constraints when it is called on
/// client threads not spawned by udipe.
///
/// Indeed, these client threads may have been named by the application on an
/// operating system where thread names are less constrained than the lowest
/// constraint denominator used by udipe.
///
/// \returns the name of the current thread, or a stringified hexadecimal TID
///          like `pthread_89ABCDEF` if the current thread is not named. This
///          name string cannot be modified and may only be used until the next
///          call to \ref get_thread_name() or the exit of the current thread.
UDIPE_NON_NULL_RESULT
const char* get_thread_name();


#ifdef UDIPE_BUILD_TESTS
    /// Unit tests
    ///
    /// This function runs all the unit tests for this module. It must be called
    /// within the scope of with_logger().
    void thread_name_unit_tests();
#endif
