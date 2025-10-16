#pragma once

//! \file
//! \brief Error handling primitives
//!
//! This code module provides helpers for error handling.


/// If `errno` is currently set to a non-zero value, log a warning that
/// describes its current value, then clear `errno`.
///
/// This function must be called within the scope of with_logger().
void warn_on_errno();
