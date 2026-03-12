#include "command.h"

#include "error.h"
#include "log.h"
#include "visibility.h"


/* DEFINE_PUBLIC
UDIPE_NON_NULL_ARGS
udipe_connect_result_t udipe_connect(udipe_context_t* context,
                                     udipe_connect_options_t options) {
    // FIXME: Do not force this synchronous implementation style, leave the
    //        choice to the backend.
    udipe_future_t* future = udipe_start_connect(context, options);
    assert(future);
    udipe_result_t result = udipe_finish(future);
    assert(result.type == UDIPE_CONNECT);
    return result.payload.network.connect;
}

DEFINE_PUBLIC
UDIPE_NON_NULL_ARGS
udipe_disconnect_result_t udipe_disconnect(udipe_context_t* context,
                                           udipe_disconnect_options_t options) {
    // FIXME: Do not force this synchronous implementation style, leave the
    //        choice to the backend.
    udipe_future_t* future = udipe_start_disconnect(context, options);
    assert(future);
    udipe_result_t result = udipe_finish(future);
    assert(result.type == UDIPE_DISCONNECT);
    return result.payload.network.connect;
}

DEFINE_PUBLIC
UDIPE_NON_NULL_ARGS
udipe_send_result_t udipe_send(udipe_context_t* context,
                               udipe_send_options_t options) {
    // FIXME: Do not force this synchronous implementation style, leave the
    //        choice to the backend.
    udipe_future_t* future = udipe_start_send(context, options);
    assert(future);
    udipe_result_t result = udipe_finish(future);
    assert(result.type == UDIPE_SEND);
    return result.payload.network.send;
}

DEFINE_PUBLIC
UDIPE_NON_NULL_ARGS
udipe_recv_result_t udipe_recv(udipe_context_t* context,
                               udipe_recv_options_t options) {
    // FIXME: Do not force this synchronous implementation style, leave the
    //        choice to the backend.
    udipe_future_t* future = udipe_start_recv(context, options);
    assert(future);
    udipe_result_t result = udipe_finish(future);
    assert(result.type == UDIPE_RECV);
    return result.payload.network.recv;
} */


#ifdef UDIPE_BUILD_TESTS
    void command_unit_tests() {
        info("Running command unit tests...");

        // TODO: Write the tests
        warn("All tests have been turned into static assertions, for now.");
    }
#endif
