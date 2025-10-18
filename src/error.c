#define _GNU_SOURCE 1

#include "error.h"

#include "log.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>


// Important note: This function is used on the error path of the formatted
//                 logging macros. It must therefore not use said macros, only
//                 the basic macros that log a pre-existing message string.
//                 Which is why it does all the formatting itself.
void warn_on_errno() {
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
                                 "Got invalid errno value %d.",
                                 initial_errno);
        assert(("integer snprintf should never fail", out_chars > 0));
        assert(("Output buffer should be large enough to hold an integer",
                (unsigned)out_chars < sizeof(output)));
        warning(output);
        errno = 0;
        return;
    }

    // Basic description that includes the symbolic name only
    const char header[] = "Got errno value ";
    const char trailer[] = ".";
    size_t min_output_size = strlen(header) + strlen(name) + strlen(trailer) + 1;

    // Full description that includes the human-readable description too
    const char separator[] = ": ";
    const char* description = strerrordesc_np(initial_errno);
    assert(("errorname and errordesc should agree on errno validation", description));
    size_t full_output_size = min_output_size + strlen(separator) + strlen(description);

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
        warning("Internal output buffer is too small for a full errno description, should be enlarged!");
        assert(("Buffer should be large enough to hold an errorname",
                sizeof(output) >= min_output_size));
        result = snprintf(output,
                          sizeof(output),
                          "%s%s%s",
                          header, name, trailer);
    }
    assert(("string snprintf should never fail", result > 0));
    warning(output);
    errno = 0;
}