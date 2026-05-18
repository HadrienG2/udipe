#include "storage_page.h"

#include <udipe/pointer.h>

#include "../status_ops.h"

#include "../../memory.h"

#include <assert.h>
#include <string.h>


UDIPE_NON_NULL_ARGS
void future_storage_allocate(future_storage_page_t** next) {
    void* const page = realtime_allocate(get_page_size());
    assert(page);

    memset(page, 0, get_page_size());
    future_storage_page_t* const new = (future_storage_page_t*)page;
    for (size_t i = 0; i < future_storage_page_len(); ++i) {
        // Must do this in addition to the memset because C11 specifies that
        // atomic_initialize() is the only valid way to initialize an atomic
        // variable. With a bit of luck, it should be optimized out.
        future_status_initialize(&(new->futures[i]),
                                 (future_status_t) { 0 });
        #ifdef __linux__
            // 0 is a valid fd number (stdin), so -1 is a better placeholder
            new->futures[i].status_sync.any = -1;
        #endif
    }

    new->next = *next;
    *next = new;
}

UDIPE_NON_NULL_ARGS
void future_storage_liberate_all(future_storage_page_t** first) {
    future_storage_page_t* next = *first;
    while (next) {
        void* const current = (void*)next;
        next = next->next;
        realtime_liberate(current, get_page_size());
    }
    *first = NULL;
}
