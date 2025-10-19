#pragma once

//! \file
//! \brief Error handling primitives
//!
//! This code module provides helpers for error handling.

#include "log.h"

#include <assert.h>
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
