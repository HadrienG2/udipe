#include <udipe/context.h>
#include <udipe/allocator.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/// Callback that configures the memory allocator of each udipe worker thread
udipe_thread_allocator_config_t configure_allocator(void* /* context */) {
    return (udipe_thread_allocator_config_t){
        .buffer_size = 9216,
        .buffer_count = 42
    };
}


int main() {
    // Start from the default libudipe configuration
    udipe_config_t config;
    memset(&config, 0, sizeof(udipe_config_t));

    // Adjust the memory allocator configuration
    config.allocator = (udipe_allocator_config_t){
        .callback = configure_allocator,
        .context = NULL
    };

    // Set up the upipe context
    udipe_context_t* context = udipe_initialize(config);
    assert(context);

    // Finalize the libudipe context
    udipe_finalize(context);
    return 0;
}
