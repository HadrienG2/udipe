# Logging & errors

## Limits of error codes

C error codes, whether provided via negative integer return values or the
`errno` thread-local variables, are meant to let applications that call a
library understand what kind of error occured and take appropriate action that
is tailored to the error at hand. But in practice...

- Most errors are fatal/unrecoverable at the application level, in the sense
  that the only thing a typical application can do when the error is encountered
  is to log what happened somewhere (typically `stderr`) in the hope of easing
  debugging, then cleanly terminate.
- That single error log, assuming the caller does remember to print it out, is
  often not very helpful in later debugging. It provides little context over
  which function was called, why it was called, or what kind of parameters were
  passed to it. To answer such questions without complex debugging tools, the
  developer needs extended logging that provides context over what the
  application was doing at the time where the error occured.
- Part of the reason why `perror()`-style logs are unhelpful is that error codes
  as a function return value only provide a very imprecise description of what
  happened. `errno` is even worse as its standard codes are not adjusted to the
  needs of individual functions, and it easily gets overwritten on the error
  propagation path. More precise programmatic error descriptions are possible,
  but used sparingly as they expose implementation details and either...
    1. Increase the size of a function's return value, thus pessimizing the
       performance in the common case where no error occured.
    2. Require awkward APIs such as out parameters that only get filled on the
       error path, thread-local state with reentrance support, POSIX signals &
       other callbacks...

Coming from this perspective, `libudipe` takes the following stance :

- Unrecoverable errors require abnormal termination, abnormal termination
  requires debugging, and debugging requires precise information beyond what
  integer error codes can provide. Logging is the sanest way to provide the
  required context for debugging, avoiding extended error descriptors that
  require complex APIs and expose too many library internals that are subjected
  to future change.
- Given the necessity of logging, traditional error codes become redundant,
  inferior to it due to their reduced precision with it, and a usability hazard
  as a caller may forget to check for them. They should thus only be kept in the
  situation where 1/some errors are recoverable and 2/there are multiple
  logically distinct recovery paths which require matching program control flow.
  Otherwise, there are simpler and safer strategies, as detailed below.


## Error reporting policy

Due to the above, `libudipe` embraces the following logging and error reporting
policy:

- The most verbose logging level provided by udipe (`TRACE`) should allow
  someone to figure out the exact control flow path that was taken through
  `libudipe` code. Less verbose levels exist...
    - To reduce the performance overhead of logging until the `INFO` point, at
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
  describing a realistic recovery scenario. Fatal errors are handled via
  `exit(EXIT_FAILURE)` after sending an appropriate `ERROR` log to explain what
  went wrong.
- Non-fatal errors are further classified according to how many non-fatal error
  cases could require qualitatively different handling on the application side.
    - If from the application side there are only two handling scenarios, error
      and non-error, then the error is handled by returning a sentinel value,
      e.g. a null pointer for a function that returns a pointer. If the return
      type does not provide a sentinel value that is idiomatic to the seasoned C
      programmer (null pointers and negative signed integer `-1`), then this
      pattern will be used...

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
      the various error conditions. Otherwise this pattern will be used...

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
your choosing. When this is not done, or when the user-defined log sink is not
available (e.g. during logger initialization), log messages go to `stderr` per
Unix convention.


## Input validation policy

Beyond the question of _how_ errors should be reported lies the question of
_when_ error conditions should be checked for. Indeed, it is not uncommon for
the implementation of a particular library function to transitively call other
library functions, and repeatedly validating user inputs and system call outputs
every step down the pipeline gets expensive from a performance perspective.

The `libudipe` policy here is that...

- Error avoidance is the preferred strategy, so if there is a way to redesign a
  function such that it has fewer invalid inputs (e.g. by using unsigned integer
  types instead of signed ones when a positive value is expected), then that
  path should be preferred.
- Every user-visible function (with a declaration in `include/`) where some
  parameter values are invalid (e.g. null pointer, out-of-range integer
  index...) must document invalid parameter values in its documentation, check
  for them, and handle them appropriately as described above.
- Functions that are not user-visible and consume parameters which should have
  been pre-validated by user-visible frontend functions must still document
  invalid parameters in internal documentation. But they are allowed to only
  check for such invalid values in `Debug` builds i.e. when the `NDEBUG` define
  is not set, because such values should only be present when `libudipe` has an
  input validation bug. In `Release` builds with `NDEBUG`, conditions that
  should only result from `libudipe` validation bugs are allowed to have
  arbitrarily bad outcome, including undefined behavior, when performance
  considerations justify it.
- Every part of the `libudipe` that calls into the operating system must handle
  all documented error cases from the underlying system call. Such error cases
  must be described in their internal documentation (referring to the original
  system call(s) documentation is fine for internal functions), recursively
  across callers, all the way to user-visible entry points where an
  easy-to-understand description of error cases is preferred.
