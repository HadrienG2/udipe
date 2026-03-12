#pragma once

//! \file
//! \brief Nodiscard polyfill
//!
//! As compiler C23 support is not quite ready at the time of writing, this
//! header provides a GCC/clang specific alternative to the `[[nodiscard]]`
//! attribute. It does nothing on MSVC and will eventually be replaced by
//! `[[nodiscard]]` once we do move to C23.


/// Assertion that a function's result must be used in some way, and failing to
/// use it is always an error.
///
/// This will make lack of use a warning on compilers that support it.
#ifdef __GNUC__
    #define UDIPE_NODISCARD __attribute__((warn_unused_result))
#else
    // TODO: Replace with [[nodiscard]] once the project is ready to move to C23
    #define UDIPE_NODISCARD
#endif
