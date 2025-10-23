#include <udipe/context.h>
#include <udipe/allocator.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/// Memory allocator configuration callback that applies the same parameters
/// for all udipe worker threads
///
/// \param context is a `const udipe_thread_allocator_config_t*` in disguise
///                that indicates which parameters should be applied.
udipe_thread_allocator_config_t apply_shared_configuration(void* context) {
    const udipe_thread_allocator_config_t* config =
        (udipe_thread_allocator_config_t*)context;
    return *config;
}

int main() {
    // Start from the default libudipe configuration
    udipe_config_t config;
    memset(&config, 0, sizeof(udipe_config_t));

    // Adjust the memory allocator configuration
    udipe_thread_allocator_config_t thread_config = {
        .buffer_size = 9000,
        .buffer_count = 42
    };
    config.allocator = (udipe_allocator_config_t){
        .callback = apply_shared_configuration,
        .context = (void*)&thread_config
    };

    // Set up the upipe context
    udipe_context_t* context = udipe_initialize(config);
    assert(context);

    // Finalize the libudipe context
    udipe_finalize(context);
    return 0;
}
