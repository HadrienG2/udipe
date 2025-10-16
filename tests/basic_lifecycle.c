#include <udipe/context.h>

#include <assert.h>
#include <string.h>


int main() {
    // Set up libudipe with the default configuration
    udipe_config_t config;
    memset(&config, 0, sizeof(udipe_config_t));
    udipe_context_t* context = udipe_initialize(config);

    // Cross-check that initialization never fails
    assert(context);

    // Liberate the libudipe context once your're done with it
    udipe_finalize(context);
    return 0;
}
