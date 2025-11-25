#pragma once

//! \file
//! \brief Formating primitives
//!
//! This code module provides helpers for usage of printf()-like functions.


/// printf() format specifier for some expression
///
/// This macro takes an expression as input and generates an appropriate
/// printf() format specifier for this expression.
#define format_for(x) _Generic((x),  \
                                         const char*: "%s",  \
                                               char*: "%s",  \
                                         signed char: "%hhd",  \
                                               short: "%hd",  \
                                                 int: "%d",  \
                                                long: "%ld",  \
                                           long long: "%lld",  \
                                       unsigned char: "%hhu",  \
                                      unsigned short: "%hu",  \
                                            unsigned: "%u",  \
                                       unsigned long: "%lu",  \
                                  unsigned long long: "%llu",  \
                                              double: "%f",  \
                                         long double: "%Lf",  \
                                         const void*: "%p",  \
                                               void*: "%p",  \
                                                bool: "%u"  \
                              )
