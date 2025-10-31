#include <udipe/context.h>
#include <udipe/log.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/// Logging callback that sends messages to a temporary file
///
/// \param context is the `FILE*` pointer associated with the temporary file.
void log_to_tempfile(void* context,
                     udipe_log_level_t level,
                     const char location[],
                     const char message[]) {
    FILE* tempfile = (FILE*)context;
    int result = fprintf(tempfile,
                         "%s from %s: %s\n",
                         udipe_log_level_name(level), location, message);
    if (result < 0) {
        perror("Failed to write log to tempfile");
        exit(EXIT_FAILURE);
    }
}

int main() {
    // Set up a temporary file
    char tempname[] = "/tmp/udipe-log_to_tempfile.XXXXXX";
    int tempfd = mkstemp(tempname);
    if (tempfd < 0) {
        perror("Failed to create temporary file");
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "Logs will be written to %s", tempname);
    FILE* tempfile = fdopen(tempfd, "w+");
    if (!tempfile) {
        perror("Failed to open temporary file");
        exit(EXIT_FAILURE);
    }

    // Start from the default libudipe configuration
    udipe_config_t config;
    memset(&config, 0, sizeof(udipe_config_t));

    // Set up maximally verbose logging to the temporary file
    config.log = (udipe_log_config_t){
        .min_level = UDIPE_TRACE,
        .callback = log_to_tempfile,
        .context = (void*)tempfile
    };

    // Set up the upipe context
    udipe_context_t* context = udipe_initialize(config);
    assert(context);

    // Finalize the libudipe context
    udipe_finalize(context);

    // Close the temporary file
    if(fclose(tempfile) < 0) {
        perror("Failed to close temporary file");
        exit(EXIT_FAILURE);
    }
    return 0;
}
