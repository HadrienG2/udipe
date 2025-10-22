#pragma once

//! \file
//! \brief Error handling primitives
//!
//! This code module provides helpers for error handling.

#include "log.h"

#include <stddef.h>
#include <stdlib.h>


/// \name External error handling
/// \{

/// If `errno` is currently set to a non-zero value, log a warning that
/// describes its current value, then clear `errno`.
///
/// This function must be called within the scope of with_logger().
void warn_on_errno();

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

/// \}


/// \name Test assertions
/// \{

/// Make sure that `assertion` is true, otherwise exit with an error message.
///
/// This macro is mainly designed for unit tests, but could in principle be used
/// in any circumstance where an internal assertion should be checked even in
/// `Release` builds because the impact of it being violated is too great.
///
/// It must be called within the scope of with_logger().
#define ensure(assertion)  \
    do {  \
        if (!(assertion)) {  \
            errorf("ensure() failed at %s:%u.\n"  \
                   "Expected " #assertion "\n"  \
                   "...but that is false!",  \
                   __FILE__, __LINE__);  \
            exit(EXIT_FAILURE);  \
        }  \
    } while(false)

/// Make sure `x == y`, otherwise exit with an error message
///
/// See ensure_comparison() for preconditions and a list of other assertions.
#define ensure_eq(x, y)  ensure_comparison("eq", (x), ==, (y))

/// Make sure `x != y`, otherwise exit with an error message
///
/// See ensure_comparison() for preconditions and a list of other assertions.
#define ensure_ne(x, y)  ensure_comparison("ne", (x), !=, (y))

/// Make sure `x > y`, otherwise exit with an error message
///
/// See ensure_comparison() for preconditions and a list of other assertions.
#define ensure_gt(x, y)  ensure_comparison("gt", (x), >, (y))

/// Make sure `x < y`, otherwise exit with an error message
///
/// See ensure_comparison() for preconditions and a list of other assertions.
#define ensure_lt(x, y)  ensure_comparison("lt", (x), <, (y))

/// Make sure `x >= y`, otherwise exit with an error message
///
/// See ensure_comparison() for preconditions and a list of other assertions.
#define ensure_ge(x, y)  ensure_comparison("ge", (x), >=, (y))

/// Make sure `x <= y`, otherwise exit with an error message
///
/// See ensure_comparison() for preconditions and a list of other assertions.
#define ensure_le(x, y)  ensure_comparison("le", (x), <=, (y))

/// \}


/// \name Other utilities
/// \{

/// Exit with an error message
///
/// This macro must be used within the scope of with_logger(), but outside of
/// the udipe static string logger implementation (since it emits logs).
#define exit_with_error(error_message)  \
    do {  \
        error(error_message);  \
        exit(EXIT_FAILURE);  \
    } while(false)

/// \}


/// \name Implementation details
/// \{

/// printf() format specifier for some expression
///
/// This is an implementation detail of ensure_comparison() that you should
/// not call directly.
///
/// It takes an expression as input and generates an appropriate printf() format
/// specifier for this expression.
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

/// Failure branch of ensure_comparison()
///
/// This is an implementation detail of ensure_comparison() that you should
/// not call directly.
///
/// See the internal section of the ensure_comparison() documentation for more
/// information about what it does.
void ensure_comparison_failure(const char* format_template,
                               const char* x_format,
                               const char* y_format,
                               ...);

/// Make sure that `x op y` returns `true`, otherwise exit wich an error
/// message which mentions `macro_name`.
///
/// This is the implementation of higher-level comparison macros ensure_eq(),
/// ensure_ne(), ensure_gt(), ensure_lt(), ensure_ge() and ensure_ge(), which
/// you should prefer over calling this low-level macro directly.
///
/// It must be called within the scope of with_logger().
///
/// \internal
///
/// If you are trying to understand the implementation of this macro, then you
/// need to know that in order to produce an appropriate format string for
/// operands `x` and `y`, which may be of nearly any type, the failure path
/// needs to be surprisingly convoluted:
///
/// - First we use format_for() to determine appropriate format specifiers for
///   input expressions `x` and `y`.
/// - Then we generate a format string that uses these format specifiers.
///   * This is needed because the output of format_for() cannot be concatenated
///     with the rest of the format string at compile time.
///   * It means that arguments other than the format specifiers must be escaped
///     with a double percent sign so that they are not used during this first
///     formatting pass, but become valid format specifiers afterwards.
/// - Generate an error message based on this format string.
///   * This step is needed because the errorf() macro does not have a
///     `verrorf()` variant that takes a `va_list`. And since this macro is the
///     only use case for it that came up so far, it did not feel worth adding.
/// - Log this error message then die with `exit(EXIT_FAILURE)`.
#define ensure_comparison(op_name, x, op, y)  \
    do {  \
        typeof(x) udipe_x = (x);  \
        typeof(y) udipe_y = (y);  \
        if (!(udipe_x op udipe_y)) {  \
            ensure_comparison_failure(  \
                "ensure_" op_name "() failed at %%s:%%u.\n"  \
                "Expected " #x " " #op " " #y "\n"  \
                "      => %s " #op " %s\n"  \
                "...but that is false!",  \
                format_for(udipe_x),  \
                format_for(udipe_y),  \
                __FILE__, __LINE__, udipe_x, udipe_y  \
            );  \
        }  \
    } while(false)

/// \}
