# Logging & errors

The general stance of `libudipe` on error handling is that most (but not all)
error conditions are unrecoverable at the application level, and bubbling up
error codes and other detailed error description types mainly serves as a way to
provide a minimal explanation of what error occured and why when debugging the
subsequent application crash.

From this perspective, if logging is already capable of providing such
"debugging context", and does it better by virtue of also explaining what the
application was doing at the time where the error occured (in higher-verbosity
modes), then detailed error condition classification at the API entry point is
redundant. It should only be kept around when there are at least two distinct 
error cases that are application-recoverable (like e.g. `EAGAIN` and `EINTR` for
Unix I/O entry points).

Taking this into account, the `libudipe` error handling and logging policy is
that...

- The most verbose logging level provided by udipe (`TRACE`) should allow
  someone to figure out the exact control flow path that was taken through
  `libudipe` code. Less verbose levels exist...
    - To reduce the performance overhead of logging, until the `INFO` point at
      and beyond which the logging overhead should be negligible in all expected
      usage scenarios.
    - To visually distinguish normal conditions (`INFO`), suspicious conditions
      (`WARNING`) and clearly bogous conditions (`ERROR`) so that the latter
      stand out in the logs.
    - To make logs easier to read in the common case where the application runs
      fine and precise control flow tracing is unnecessary. When an unexplained
      errors appears, the application can be re-run at a more verbose logging
      level until the error is encountered again and understood, at which point
      logging can be tuned back down.
- We distinguish fatal and non-fatal errors. Fatal errors are those that a
  realistic application cannot recover from, which should result in application
  termination. All errors are fatal unless proven non-fatal by virtue of
  describing a realistic recovery scenario.
- Fatal errors are handled via `exit(EXIT_FAILURE)` after sending an appropriate
  `ERROR` log to explain what went wrong.
- Non-fatal errors are further classified according to how many non-fatal error
  cases could require qualitatively different handling on the application side.
    - If from the application side there are only two handling scenarios, error
      and non-error, then the error is handled by returning a sentinel value,
      e.g. a null pointer for a function that returns a pointer. If the return
      type does not naturally provide such a sentinel value in one of the ways
      that is idiomatic to the seasoned C practicioner (null pointers and
      negative signed integer `-1`), then this pattern is used...

      ```c
      typedef struct foo_result_s {
          bool valid;
          T success;
      } foo_result_t;

      foo_result_t foo();
      ```

      ...to highlight the need to check for errors before examining the output
      value.
    - If from the application side there are multiple error cases that require
      qualitatively different handling with different control flow paths, then
      if the function naturally returns a positive integer that can safely be
      turned into a signed integer, the negative values are used to enumerate
      the various error conditions. Otherwise this pattern is used...

      ```c
      typedef enum bar_status_e {
        BAR_SUCCESS = 0,
        BAR_ERROR_OOPS,
        BAR_ERROR_MYBAD,
        BAR_ERROR_WHATEVER,
      } bar_status_t;

      typedef struct bar_result_s {
          bar_status_t status;
          T success;
      } bar_result_t;

      bar_result_t bar();
      ```

      ...to highlight the need to check for errors before examining the output
      value.

Aside from this strong stance on fatal errors and the role of logging,
`libudipe` otherwise tries to follow traditional C library design conventions,
and its logging system features the usual log levels (`TRACE`, `DEBUG`, `INFO`,
`WARNING` and `ERROR`) along with a way to redirect log messages to a sink of
your choosing. When this is not done, log messages go to `stderr` by default,
per Unix convention.
