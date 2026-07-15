#pragma once

//! \file
//! \brief Code scope tracking utilities
//!
//! The utilities provided by this module enable you to...
//!
//! - Track arbitrarily nested code scopes.
//! - Track how deeply nested the current call stack of tracked scopes is.
//! - Schedule some "destructor" code to be executed upon normal exit from such
//!   a scope (including normal exit, function, return, loop break...).
//!
//! ...by abstracting over compiler-specific facilities at the time of writing,
//! with the aim of eventually migrating to C defer once it finally lands.

#include <udipe/pointer.h>

#include <assert.h>
#include <stddef.h>
#include <threads.h>


/// Scope destructor callback
///
/// See \ref SCOPE_START_WITH_DESTRUCTOR for more information.
typedef void (*scope_destructor_t)(void* /* context */);


/// \name Implementation details
/// \{

/// State of a single udipe-tracked scope
///
/// Each \ref SCOPE_START call fills up one of these structs and links it at the
/// top of the linked list of the \ref scope_tracker_t. The inner data is later
/// used for the purpose of performing scope-based operations.
typedef struct scope_guard_s scope_guard_t;

/// \copydoc scope_guard_t
struct scope_guard_s {
    /// Pointer to any lower-level scope that was active before \ref SCOPE_START
    ///
    const scope_guard_t* next_scope;

    /// __func__ pointer of the function that called \ref SCOPE_START
    ///
    const char* func;

    /// Optional user-defined function to be called on scope exit (or `NULL`)
    ///
    scope_destructor_t destructor;

    /// Optional parameter to be passed to `destructor`
    ///
    /// User-defined semantics, can always be `NULL` and must be `NULL` if
    /// `destructor` is `NULL`.
    void* destructor_context;
};

/// Udipe scope tracker
///
/// Each thread gets a thread-local instance of this struct called
/// `udipe_scope_tracker`. It tracks a singly linked list of \ref scope_guard_t
/// from the top scope to the bottom scope along with an integer that avoids
/// slow list traversal for the common query of evaluating how deeply nested the
/// current scope is.
typedef struct scope_tracker_s {
    /// State of the innermost udipe-tracked scope
    ///
    /// Previous scopes can be accessed by following the singly linked list of
    /// \ref scope_guard_t::next_scope pointers.
    const scope_guard_t* top_scope;

    /// Number of nested udipe-tracked scope
    ///
    /// This is simply the length of the linked list of \ref scope_guard_t
    /// accessible via `top_scope`, but it is precomputed as it is much faster
    /// to track an integer count than to traverse a linked list.
    size_t nesting_depth;
} scope_tracker_t;

/// Thread-local tracking of udipe scopes
///
/// See \ref scope_tracker_t for more information.
extern thread_local scope_tracker_t udipe_scope_tracker;

/// \ref scope_guard_t destructor that gets called on scope exit
///
/// This function gets automatically called upon normal exit of the scope formed
/// by a `SCOPE_START` macro and the `SCOPE_END` macro. It gets passed in a
/// pointer to the `guard` that was previously set up by \ref
/// SCOPE_START_WITH_DESTRUCTOR_AND_ID and ensures that...
///
/// - The user-specified destructor (if any) gets called.
/// - The thread-local scope tracker is reset to the parent scope.
/// - The scope nesting depth is updated accordingly.
///
/// \param guard should point to the `guard` that was previously set up by \ref
///              SCOPE_START_WITH_DESTRUCTOR_AND_ID.
UDIPE_NON_NULL_ARGS
static inline
void scope_guard_finalize(const scope_guard_t* guard) {
    assert(guard->destructor || (guard->destructor_context == NULL));
    if (guard->destructor) {
        (guard->destructor)(guard->destructor_context);
    }
    udipe_scope_tracker.top_scope = guard->next_scope;
    assert(udipe_scope_tracker.nesting_depth >= (size_t)1);
    --udipe_scope_tracker.nesting_depth;
}

/// Implementation of `SCOPE_START` macros
///
/// This works just like \ref SCOPE_START_WITH_DESTRUCTOR except an extra `id`
/// must be provided. This `id` should expand into a single C token, which
/// should be unique across all calls to `SCOPE_START_WITH_DESTRUCTOR_AND_ID`
/// within the current function.
#ifdef __GNUC__
    #define SCOPE_START_WITH_DESTRUCTOR_AND_ID(destructor_, context_, id)  \
        do {  \
            scope_guard_t const udipe_scope_guard_ ## id  \
                __attribute__((__cleanup__(scope_guard_finalize)))  \
                    = (scope_guard_t){  \
                        .next_scope = udipe_scope_tracker.top_scope,  \
                        .func = __func__,  \
                        .destructor = (destructor_),  \
                        .destructor_context = (context_)  \
                    };  \
            udipe_scope_tracker.top_scope = &(udipe_scope_guard_ ## id);  \
            ++udipe_scope_tracker.nesting_depth;
#elif defined(_MSC_VER)
    #define SCOPE_START_WITH_DESTRUCTOR_AND_ID(destructor_, context_, id)  \
        __try {  \
            scope_guard_t const udipe_scope_guard_ ## id  \
                = (scope_guard_t){  \
                    .next_scope = udipe_scope_tracker.top_scope,  \
                    .func = __func__,  \
                    .destructor = (destructor_),  \
                    .destructor_context = (context_)  \
                };  \
            udipe_scope_tracker.top_scope = &(udipe_scope_guard_ ## id);  \
            ++udipe_scope_tracker.nesting_depth;
#else
    #error "Sorry, we don't support your compiler yet. Please file a bug report about it!"
#endif

/// \}


/// \name Scope tracking API
/// \{

/// Start a udipe-tracked scope with an optional destructor
///
/// This macro behaves like \ref SCOPE_START and must similarly be paired with a
/// later use of \ref SCOPE_END. But it additionally schedules a call to
/// `destructor(context)` when the scope is exited by normal means (finishing
/// the last line of code from it, early return, loop break...).
///
/// This macro, which currently relies on GNU and MSVC extensions, can be used
/// to approximate the behavior of destructors in C++. It is meant to eventually
/// be replaced by `defer` once this feature finally lands into the C standard.
///
/// \param destructor must be a \ref scope_destructor_t callback, which can be
///                   `NULL`. If non-null, it will be called when the scope is
///                   exited by normal means.
/// \param context is an arbitrary `void*` pointer that will be passed back to
///                `destructor`. It can be `NULL`, but is only allowed to be
///                non-null if `destructor` is non-null.
///
/// \internal
///
/// __LINE__ should be a good enough unique identifier within the scope of a
/// function, unless 1/said function's definition is spread across multiple
/// files that are patched together using `#include` or 2/the programmer uses
/// `#line` to adjust the `__LINE__` counter. I refuse to support such
/// programming crimes, so if you do this, you're on your own...
#define SCOPE_START_WITH_DESTRUCTOR(destructor, context)  \
    SCOPE_START_WITH_DESTRUCTOR_AND_ID(destructor,  \
                                       context,  \
                                       __LINE__)

/// Start a udipe-tracked scope
///
/// This macro behaves like an opening brace (`{`) that starts a new code block,
/// which must be closed by a matching \ref SCOPE_END later in the same code
/// block where \ref SCOPE_START was used.
///
/// You can use \ref IS_INSIDE_LOCAL_SCOPE in a macro to check if some code is
/// surrounded by a \ref SCOPE_START / \ref SCOPE_END pair, and you can use
/// global_scope_depth() to count how many \ref SCOPE_START / \ref SCOPE_END
/// pairs surround the active call stack.
///
/// This functionality is used by the udipe logger to...
///
/// - Ensure that every function that calls into the logger features at least
///   one \ref SCOPE_START / \ref SCOPE_END pair, the outermost of which should
///   be located at the first and last line of this function.
/// - Use this to determine how deeply nested the call stack is at the point
///   where the logger is called, and cut off overly nested logs unless the user
///   enables them.
#define SCOPE_START  SCOPE_START_WITH_DESTRUCTOR(NULL, NULL)

/// End a udipe-tracked scope
///
/// See \ref SCOPE_START for more information about how scopes work.
#ifdef __GNUC__
    #define SCOPE_END  \
        } while(false);
#elif defined(_MSC_VER)
    #define SCOPE_END  \
        } __finally {  \
            scope_guard_finalize(udipe_scope_tracker.top_scope);  \
        }
#else
    #error "Sorry, we don't support your compiler yet. Please file a bug report about it!"
#endif

/// Truth that the current code is surrounded by a \ref SCOPE_START / \ref
/// SCOPE_END pair **within the same function**
///
/// Be warned that by nature, this boolean expression can only be used inside of
/// a macro, and extracting its use into a utility function won't work as
/// expected even if said function is declared `static inline`.
///
/// One use of this utility is to let logging macros check that they are being
/// used inside of a \ref LOGGED_FUNCTION_START / \ref LOGGED_FUNCTION_END
/// block.
///
/// \internal
///
/// Comparing strings with == is fine here because __func__ is defined by the C
/// standard to be "as if defined immediately after the opening brace" which
/// means that it will have a single address throughout the entire function,
/// including from \ref SCOPE_START to \ref SCOPE_END.
#define IS_INSIDE_LOCAL_SCOPE  \
    (udipe_scope_tracker.top_scope  \
        && udipe_scope_tracker.top_scope->func == __func__)

/// Amount of of \ref SCOPE_START / \ref SCOPE_END pairs that surround the
/// **entire call stack**
///
/// In contrast with \ref IS_INSIDE_LOCAL_SCOPE, this utility does not consider
/// just the active function, but also every parent function in the full call
/// stack that eventually led the program to call into the active function.
///
/// One use of this utility is to let logging macros check how deeply nested
/// their call stack is in order to adjust logging verbosity accordingly.
static inline
size_t global_scope_depth() {
    return udipe_scope_tracker.nesting_depth;
}

/// \}


#ifdef UDIPE_BUILD_TESTS
    /// Unit tests
    ///
    /// This function runs all the unit tests for this module. It must be called
    /// within a logging scope.
    void scope_unit_tests();
#endif
