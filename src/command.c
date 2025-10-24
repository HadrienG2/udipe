#include "command.h"

#include "error.h"
#include "log.h"


#ifdef UDIPE_BUILD_TESTS
    #include <stddef.h>

    void command_unit_tests() {
        info("Running command unit tests...");

        debug("Checking struct layout...");
        // x86-specific, to be completed with ifdefs when porting to new arches
        const size_t cache_line_size = 64;
        ensure_le(cache_line_size, FALSE_SHARING_GRANULARITY);
        ensure_eq(alignof(udipe_future_t), FALSE_SHARING_GRANULARITY);
        ensure_eq(sizeof(udipe_future_t), FALSE_SHARING_GRANULARITY);
        udipe_future_t fut;
        ensure_le(
            (char*)(&fut.futex) - (char*)&fut + sizeof(uint32_t),
            cache_line_size
        );
        ensure_eq(alignof(command_t), FALSE_SHARING_GRANULARITY);
        ensure_eq(sizeof(command_t), FALSE_SHARING_GRANULARITY);
        command_t command;
        ensure_le(
            (char*)(&command.id) - (char*)&command + sizeof(udipe_command_id_t),
            cache_line_size
        );
        ensure_eq(alignof(command_queue_t), FALSE_SHARING_GRANULARITY);
        ensure_gt(sizeof(command_queue_t), EXPECTED_MIN_PAGE_SIZE/2);
        ensure_le(sizeof(command_queue_t), EXPECTED_MIN_PAGE_SIZE);

        // TODO: Test other functionality
    }
#endif
