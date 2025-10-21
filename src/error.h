#pragma once

//! \file
//! \brief Error handling primitives
//!
//! This code module provides helpers for error handling.

#include "log.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>


/// If `errno` is currently set to a non-zero value, log a warning that
/// describes its current value, then clear `errno`.
///
/// This function must be called within the scope of with_logger().
void warn_on_errno();


/// Exit with an error message
///
/// This macro must be used within the scope of with_logger(), but outside of
/// the udipe static string logger implementation (since it emits logs).
#define exit_with_error(error_message)  \
    do {  \
        error(error_message);  \
        exit(EXIT_FAILURE);  \
    } while(false)


/// Make sure that a condition is true, otherwise exit with an error message
///
/// This is mainly useful for unit tests, but may prove to be useful for other
/// purposes someday.
///
/// The use of direct fprintf to stderr is for consistency with ensure_eq(),
/// which cannot easily be made to use the logging macros.
#define ensure(assertion)  \
    do {  \
        if (!(assertion)) {  \
            fprintf(stderr,  \
                    "ensure() FAILED @ %s():%u -> "  \
                    "Expected " #assertion " to be true but it isn't",  \
                    __func__, __LINE__);  \
            exit(EXIT_FAILURE);  \
        }  \
    } while(false)


/// Proper format string for some expression (ensure_eq() implementation detail)
///
/// This is an implementation detail of ensure_eq() that you should never need
/// to call into yourself.
//
// TODO: Expand list of supported types as needed
#define format_for(x) _Generic((x),  \
                                 const char*: "%s",  \
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
                                       void*: "%p",  \
                                        bool: "%u"  \
                      )


/// Failure branch of ensure_eq() and ensure_ne()
///
/// You should not call this function directly, but rather call the ensure_eq()
/// macro, which will take care of calling it with the right parameters.
void ensure_eq_failure(const char* format_template,
                       const char* x_format,
                       const char* y_format,
                       ...);


/// Make sure that two things are equal, otherwise exit with an error message
///
/// This is mainly useful for unit tests, but may prove to be useful for other
/// purposes someday.
///
/// Internally, the macro works by generating a format string that is
/// appropriate for its argument types, which is then used for the actual
/// fprintf call before exiting.
#define ensure_eq(x, y)  \
    do {  \
        typeof(x) udipe_x = (x);  \
        typeof(y) udipe_y = (y);  \
        if (udipe_x != udipe_y) {  \
            ensure_eq_failure(  \
                "ensure_eq() FAILED @ %%s():%%u -> Expected " #x " == " #y  \
                ", but it evaluates to %s != %s",  \
                format_for(udipe_x),  \
                format_for(udipe_y),  \
                __func__, __LINE__, udipe_x, udipe_y  \
            );  \
        }  \
    } while(false)


/// Make sure that two things are different, otherwise exit with an error message
///
/// This is basically the opposite of ensure_eq() and works similarly.
#define ensure_ne(x, y)  \
    do {  \
        typeof(x) udipe_x = (x);  \
        typeof(y) udipe_y = (y);  \
        if (udipe_x == udipe_y) {  \
            ensure_eq_failure(  \
                "ensure_ne() FAILED @ %%s():%%u -> Expected " #x " != " #y  \
                ", but it evaluates to %s == %s",  \
                format_for(udipe_x),  \
                format_for(udipe_y),  \
                __func__, __LINE__, udipe_x, udipe_y  \
            );  \
        }  \
    } while(false)


/// Exit following the failure of a C function
///
/// This macro handles situations where all of the following is true:
///
/// - A C function is known to have previously failed.
/// - `errno` may or may not be set to further explain what error occured.
/// - None of the possible error cases can or should be recovered from.
/// - exit_with_error() preconditions are fulfilled.
#define exit_after_c_error(error_message)  \
    do {  \
        warn_on_errno();  \
        exit_with_error(error_message);  \
    } while(false)


/// Exit if an int-returning C functions fails
///
/// This handles functions where all of the following is true:
///
/// - The return type is `int`
/// - Errors are signaled by returning a negative value
/// - exit_after_c_error() preconditions are fulfilled
#define exit_on_negative(result, error_message)  \
    do {  \
        int udipe_result = (result);  \
        if (udipe_result < 0) exit_after_c_error(error_message);  \
    } while(false)



/// Exit if a pointer-returning C functions fails
///
/// This handles functions where all of the following is true:
///
/// - Errors are signaled by returning NULL
/// - exit_after_c_error() preconditions are fulfilled
#define exit_on_null(result, error_message)  \
    do {  \
        void* udipe_result = (result);  \
        if (!udipe_result) exit_after_c_error(error_message);  \
    } while(false)
