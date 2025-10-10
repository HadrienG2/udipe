#pragma once

/// \file udipe/visibility.h
/// \brief Symbol visibility configuration
///
/// This project uses GCC's `-fvisibility=hidden` model, i.e. all functions
/// which are not marked as public are not exported from the library, which
/// improves linker and library performance.
///
/// Functions which are marked as `extern` are excluded from this mechanism, but
/// hey MUST go through the ELF PLT to allow linker overriding, which means that
/// internal calls from `udipe` are pessimized to take the same path as external
/// calls from outside `udipe`.
///
/// Therefore, a third visibility mode is needed for functions which are used by
/// `udipe` internally **and** by external clients of udipe. And enforcing this
/// visibility is the job of \ref UDIPE_PUBLIC.

/// Declare a function a public
///
/// This is used to annotate function declarations so that udipe can use them
/// internally with optimal performance and external users can use them too.
#define UDIPE_PUBLIC __attribute__((visibility("default")))
