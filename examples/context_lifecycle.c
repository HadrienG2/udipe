#include <udipe/context.h>

#include <assert.h>
#include <string.h>


int main() {
    // Set up libudipe with the default configuration
    udipe_config_t config = { 0 };
    udipe_context_t* context = udipe_initialize(config);

    // This is guaranteed to always hold as initialization failure is fatal
    assert(context);

    // Remember to finalize the libudipe context once you're done with it
    udipe_finalize(context);
    return 0;
}
