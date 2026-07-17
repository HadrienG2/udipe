#include "name_filter.h"

#include <udipe/nodiscard.h>

#include "error.h"
#include "log.h"

#include <string.h>


UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
name_filter_t name_filter_initialize(const char* key) {
    LOGGED_FUNCTION_START("\"%s\"", key)
        if (strcmp(key, "") != 0) {
            infof(
                "Will only execute tests/benchmarks whose name contains \"%s\"",
                key
            );
        }
        char* filter = strdup(key);
        exit_on_null(filter, "Failed to allocate name filter");
        return filter;
    LOGGED_FUNCTION_END
}

UDIPE_NODISCARD
UDIPE_NON_NULL_ARGS
bool name_filter_matches(name_filter_t filter, const char* name) {
    LOGGED_FUNCTION_START("\"%s\", \"%s\"", filter, name)
        bool passed = (bool)strstr(name, filter);
        if (!passed) debugf("Filtered out \"%s\"", name);
        return passed;
    LOGGED_FUNCTION_END
}

UDIPE_NON_NULL_ARGS
void name_filter_finalize(name_filter_t* filter) {
    LOGGED_FUNCTION_START("&%p", *filter)
        debugf("Liberating name filter %p...", filter);
        free(*filter);

        debug("...and poisoning it to make use-after-free more detectable...");
        *filter = NULL;
    LOGGED_FUNCTION_END
}


#ifdef UDIPE_BUILD_TESTS
    void name_filter_unit_tests() {
        LOGGED_FUNCTION_START_NO_PARAMS
            info("Running name filtering unit tests...");

            debug("Testing catch-all empty name filter...");
            name_filter_t filter = name_filter_initialize("");
            ensure(name_filter_matches(filter, ""));
            ensure(name_filter_matches(filter, "a"));
            ensure(name_filter_matches(filter, "ba"));
            name_filter_finalize(&filter);

            debug("Testing non-empty name filter...");
            filter = name_filter_initialize("abc");
            ensure(!name_filter_matches(filter, ""));
            ensure(!name_filter_matches(filter, "a"));
            ensure(!name_filter_matches(filter, "ab"));
            ensure(name_filter_matches(filter, "abc"));
            ensure(name_filter_matches(filter, "dabc"));
            ensure(name_filter_matches(filter, "dabce"));
            ensure(name_filter_matches(filter, "abce"));
            ensure(!name_filter_matches(filter, "bc"));
            ensure(!name_filter_matches(filter, "c"));
            name_filter_finalize(&filter);
        LOGGED_FUNCTION_END
    }
#endif  // UDIPE_BUILD_TESTS
