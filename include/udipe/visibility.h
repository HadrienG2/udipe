#pragma once

//! \file
//! \brief Imported symbol visibility
//!
//! This project uses GCC's `-fvisibility=hidden` model, i.e. all functions
//! which are not marked as public are not exported from the library, which
//! improves linker and library performance.
//!
//! Functions which are marked as `extern` are excluded from this mechanism, but
//! they must also go through the ELF PLT to allow linker overriding, which
//! means that internal calls from `udipe` are pessimized to take the same path
//! as external calls from outside `udipe`.
//!
//! Therefore, a third visibility mode is needed for functions which are used by
//! `udipe` internally **and** by external clients of udipe. And enforcing this
//! visibility is the job of \ref UDIPE_PUBLIC.


/// Mark a function declaration as public
///
/// This is used to annotate which functions are meant to be used by udipe end
/// users, which has the following consequences:
///
/// - On GNU platforms, we switch back from the hidden visibility model to the
///   default visibility model so that the function can be used externally.
/// - On Windows, we declare the functions as externally available from a DLL.
#ifdef __GNUC__
    #define UDIPE_PUBLIC __attribute__((visibility("default")))
#elif defined(_MSC_VER)
    #define UDIPE_PUBLIC __declspec(dllimport)
#else
    #define UDIPE_PUBLIC
#endif
