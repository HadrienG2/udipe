#include "scope.h"

#include "error.h"
#include "log.h"

#include <threads.h>


thread_local scope_tracker_t udipe_scope_tracker = { 0 };


#ifdef UDIPE_BUILD_TESTS

    void set_to_true(void* context) {
        bool* target = (bool*)context;
        *target = true;
    }

    void scope_unit_tests() {
        const bool initial_inside_local = IS_INSIDE_LOCAL_SCOPE;
        const size_t initial_depth = global_scope_depth();

        LOGGED_FUNCTION_START_NO_PARAMS
            info("Running scope unit tests...");

            debug("Checking initial scope...");
            ensure(!initial_inside_local);

            debug("Checking new scope...");
            ensure(IS_INSIDE_LOCAL_SCOPE);
            ensure_eq(global_scope_depth(), initial_depth + 1);

            debug("Checking nested normal scope...");
            SCOPE_START
                ensure(IS_INSIDE_LOCAL_SCOPE);
                ensure_eq(global_scope_depth(), initial_depth + 2);
            SCOPE_END
            ensure(IS_INSIDE_LOCAL_SCOPE);
            ensure_eq(global_scope_depth(), initial_depth + 1);

            debug("Checking nested scope with destructor...");
            bool destroyed = false;
            SCOPE_START_WITH_DESTRUCTOR(set_to_true, (void*)&destroyed)
                ensure(IS_INSIDE_LOCAL_SCOPE);
                ensure_eq(global_scope_depth(), initial_depth + 2);
                ensure(!destroyed);
            SCOPE_END
            ensure(IS_INSIDE_LOCAL_SCOPE);
            ensure_eq(global_scope_depth(), initial_depth + 1);
            ensure(destroyed);
        LOGGED_FUNCTION_END
    }

#endif  // UDIPE_BUILD_TESTS
