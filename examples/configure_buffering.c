#include <udipe/buffer.h>
#include <udipe/context.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/// Buffering configuration callback that applies the same parameters
/// for all udipe worker threads
///
/// \param context is a `const udipe_buffer_config_t*` in disguise that
///                indicates which parameters should be applied.
udipe_buffer_config_t apply_shared_configuration(void* context) {
    const udipe_buffer_config_t* config = (udipe_buffer_config_t*)context;
    return *config;
}

int main() {
    // Start from the default libudipe configuration
    udipe_config_t config = { 0 };

    // Adjust the buffering configuration
    udipe_buffer_config_t buffer_config = {
        .buffer_size = 9000,
        .buffer_count = 42
    };
    config.buffer = (udipe_buffer_configurator_t){
        .callback = apply_shared_configuration,
        .context = (void*)&buffer_config
    };

    // Set up the upipe context
    udipe_context_t* context = udipe_initialize(config);
    assert(context);

    // Finalize the libudipe context
    udipe_finalize(context);
    return 0;
}
