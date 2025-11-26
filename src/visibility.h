#pragma once

//! \file
//! \brief Exported symbol visibility
//!
//! The \ref UDIPE_PUBLIC annotation on function declarations needs a matching
//! annotation on the function definition. This header defines that annotation.


/// Annotate a function definition for \ref UDIP_PUBLIC visibility.
///
/// This is the proper annotation for the definition of a function that has been
/// declared with \ref UDIPE_PUBLIC visibility.
#ifdef __GNUC__
    #define DEFINE_PUBLIC __attribute__((visibility("default")))
#elif defined(_MSC_VER)
    #define DEFINE_PUBLIC __declspec(dllexport)
#else
    #define DEFINE_PUBLIC
#endif
