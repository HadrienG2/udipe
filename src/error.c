#define _GNU_SOURCE 1

#include "error.h"

#include "log.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
    #include <errhandlingapi.h>
#endif


void warn_on_errno() {
    // WARNING: This function is used on the error path of the formated logging
    //          implementation and must therefore not perform any formated
    //          logging (directly or indirectly invoke a logging macro whose
    //          name ends with f). Basic static string logging is fine.

    // No errno, no output
    if (errno == 0) return;
    const int initial_errno = errno;

    // Output buffer
    //
    // Must be statically allocated because warn_on_errno() may be called to
    // describe errors from malloc() and friends, in which case attempting to
    // allocate a string to hold the error description is a bad move...
    static thread_local char output[255] = "";

    // Get the symbolic name of this errno value i.e. "EPERM" if it's EPERM.
    const char* name = strerrorname_np(initial_errno);
    if (!name) {
        // If this fails, just use the integer value + highlight the failure
        int out_chars = snprintf(output,
                                 sizeof(output),
                                 "Got invalid errno value %d!",
                                 initial_errno);
        assert(("Integer snprintf should never fail", out_chars > 0));
        assert(("Output buffer should be large enough to hold an integer",
                (unsigned)out_chars < sizeof(output)));
        warning(output);
        errno = 0;
        return;
    }

    // Basic description that includes the symbolic name only
    const char header[] = "Got errno value ";
    const char trailer[] = ".";
    const size_t min_output_size = strlen(header) + strlen(name) + strlen(trailer) + 1;

    // Full description that includes the human-readable description too
    const char separator[] = ": ";
    const char* description = strerrordesc_np(initial_errno);
    assert(("strerrorname_np() and strerrordesc_np() "
            "should agree on errno validation",
            description));
    const size_t full_output_size = min_output_size + strlen(separator) + strlen(description);

    // Pick the description that fits in the output buffer
    int result;
    if (sizeof(output) >= full_output_size) {
        // Ideally everything...
        result = snprintf(output,
                          sizeof(output),
                          "%s%s%s%s%s",
                          header, name, separator, description, trailer);
    } else {
        // ...but if there's not enough room, just the basics. Do warn about it.
        warning("Internal output buffer is too small for a full errno "
                "description and should be enlarged!");
        assert(("Buffer should be large enough to hold an errorname",
                sizeof(output) >= min_output_size));
        result = snprintf(output,
                          sizeof(output),
                          "%s%s%s",
                          header, name, trailer);
    }
    assert(("String snprintf should never fail!", result > 0));
    warning(output);
    errno = 0;
}


#ifdef _WIN32
    void win32_warn_on_error() {
        unsigned last_error = GetLastError();
        if (last_error == 0) return;
        // TODO: Print out the textual description using FormatMessage() as in
        //       the errno case instead of simply printing the numerical error
        //       code as this code currently does.
        //
        //       For now, please refer to the "Debug system error codes" MSDN
        //       page which provides a table of Windows error codes and some
        //       tools to automatically look them up:
        //       https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes
        warningf("Got thread last-error code %u", last_error);
        SetLastError(0);
    }
#endif

void ensure_comparison_failure(const char* format_template,
                               const char* x_format,
                               const char* y_format,
                               ...) {
    // Determine the format string size
    int result = snprintf(NULL, 0, format_template, x_format, y_format);
    exit_on_negative(result, "Failed to evaluate format string size!");
    size_t format_size = 1 + (size_t)result;

    // Allocate the format string buffer
    char* format = alloca(format_size);
    exit_on_null(format, "Failed to allocate format string!");

    // Generate the format string
    result = snprintf(format, format_size, format_template, x_format, y_format);
    exit_on_negative(result, "Failed to generate format string!");

    // Get two copies of the variadic arguments
    va_list args1, args2;
    va_start(args1, y_format);
    va_copy(args2, args1);

    // Determine the error message size
    result = vsnprintf(NULL, 0, format, args1);
    va_end(args1);
    exit_on_negative(result, "Failed to evaluate error message size!");
    size_t error_message_size = 1 + (size_t)result;

    // Allocate the error message buffer
    char* error_message = alloca(error_message_size);
    exit_on_null(error_message, "Failed to allocate error message buffer!");

    // Generate the error message
    result = vsnprintf(error_message, error_message_size, format, args2);
    va_end(args2);
    exit_on_negative(result, "Failed to generate error message!");

    // Finally print the error message and exit
    exit_with_error(error_message);
}
