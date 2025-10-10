#include <udipe/context.h>

#include <stdlib.h>
#include <string.h>

int main() {
    udipe_config_t config;
    memset(&config, 0, sizeof(udipe_config_t));
    udipe_context_t* context = udipe_initialize(config);
    udipe_finalize(context);
    return 0;
}
