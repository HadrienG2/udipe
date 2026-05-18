#include "allocator.h"

#include <udipe/nodiscard.h>
#include <udipe/pointer.h>

#include "type.h"

#include "../error.h"
#include "../future.h"
#include "../context.h"


UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
udipe_future_t* future_allocate(udipe_context_t* context,
                                future_type_t type) {
    // TODO: Implement (see function docs)
    // TODO: Check initial status, using swap in debug builds.
    exit_with_error("Not implemented yet!");
}

UDIPE_NON_NULL_ARGS
void future_liberate(udipe_future_t* /*future*/) {
    // TODO: Implement.
    // TODO: Check initial status, using swap in debug builds.
    // TODO: Set most values to zero-ish and the output fd to -1 before
    //       recycling the future into the thread-local pool.
    // TODO: See udipe_future_t field descriptions to see which inner file
    //       descriptors should be recycled and which should be
    //       destroyed/recreated.
    exit_with_error("Not implemented yet!");
}
