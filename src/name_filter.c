#include "name_filter.h"

#include "error.h"
#include "log.h"

#include <string.h>


UDIPE_NON_NULL_ARGS
UDIPE_NON_NULL_RESULT
name_filter_t name_filter_initialize(const char* key) {
    if (strcmp(key, "") != 0) {
        infof("Will only execute tests/benchmarks whose name contains \"%s\"", key);
    }
    char* filter = strdup(key);
    exit_on_null(filter, "Failed to allocate name filter");
    return filter;
}

UDIPE_NON_NULL_ARGS
bool name_filter_matches(name_filter_t filter, const char* name) {
    bool passed = (bool)strstr(name, filter);
    if (!passed) debugf("Filtered out \"%s\"", name);
    return passed;
}

UDIPE_NON_NULL_ARGS
void name_filter_finalize(name_filter_t* filter) {
    debug("Liberating name filter...");
    free(*filter);
    *filter = NULL;
}


#ifdef UDIPE_BUILD_TESTS
    void name_filter_unit_tests() {
        info("Running name filtering unit tests...");
        with_log_level(UDIPE_DEBUG, {
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
        });
    }
#endif  // UDIPE_BUILD_TESTS
