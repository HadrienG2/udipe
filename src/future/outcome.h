#pragma once

//! \file
//! \brief Execution outcome of \ref udipe_future_t


/// Future execution outcome
///
/// After a future enters \ref STATE_CANCELING or \ref STATE_RESULT, this enum
/// tracks whether the associated asynchronous operation completed successfully
/// or errored out in some fashion.
///
/// The switch from \ref OUTCOME_UNKNOWN to another outcome only occurs once in
/// the lifetime of a particular future, after which the outcome remains fixed
/// for the rest of the future's useful life. Even if multiple problems could
/// prevent a future from reaching a successful outcome (e.g. a future is
/// canceled, then the operation errors out), only the first problem will be
/// reported via this outcome enum.
///
/// Outcome information is encoded into selected bits of the \ref
/// future_status_word_t so that 1/it can be atomically checked and modified
/// along with other future status info and 2/changes can be awaited using
/// address_wait() for future types that support this synchronization method.
typedef enum future_outcome_e /* : _BitInt(3) */ {
    /// Outcome is not (yet) known
    ///
    /// Futures keep this outcome until their \ref future_state_t reaches \ref
    /// STATE_CANCELING or \ref STATE_RESULT, which is the state where the
    /// outcome of the associated asynchronous operation is actually known.
    /// After being liberated, futures go back to this dummy state.
    OUTCOME_UNKNOWN = 0,

    /// Successful outcome
    ///
    /// Because futures are about asynchronous operation scheduling, the notion
    /// of success that they live by is that it is valid for an operation
    /// scheduled after the current operation to run.
    ///
    /// Future types that exhibit non-fatal failure modes (e.g. packet loss in
    /// UDP) should supplement such a "successful" outcome with warning
    /// information, reported via logging and/or supplementary status fields in
    /// the result payload that is specific to this future type.
    OUTCOME_SUCCESS,

    /// Dependency-induced failure
    ///
    /// This outcome is reported when the asynchronous operation associated with
    /// this future could not start, because it was scheduled after other
    /// futures and at least one of them has reached this outcome or one of the
    /// other failure outcomes described below.
    ///
    /// It cannot be reached for future types that have no dependency.
    OUTCOME_FAILURE_DEPENDENCY,

    /// Internal failure
    ///
    /// This outcome is reported when the asynchronous operation associated with
    /// this future started being processed, but the processing then failed due
    /// to reasons specific to this operation type. For example, failing to send
    /// a network packet due to a "network unreachable" error will lead to this
    /// future outcome.
    ///
    /// Individual future types will provide more specific error reporting via
    /// logging and/or operation-specific metadata in their result payload.
    OUTCOME_FAILURE_INTERNAL,

    /// Cancelation-induced failure
    ///
    /// This outcome is reported when the asynchronous processing associated
    /// with this future was not carried out because the user expressed loss of
    /// interest by passing this specific future to udipe_cancel().
    ///
    /// To make it possible to differentiate such direct cancelation from
    /// indirect cancelation through cancelation of an upstream future which
    /// this future depends on in constant time, the latter is reported via \ref
    /// OUTCOME_FAILURE_DEPENDENCY.
    OUTCOME_FAILURE_CANCELED,

    /// Not a true outcome, only needed to count how many outcomes there are
    ///
    NUM_OUTCOMES,

    // NOTE: If this enum gets more than 8 variants, excluding NUM_OUTCOMES
    //       which is never assigned to it, reallocate the bit budget of the
    //       outcome field of the future_status_word_t then adjust the _BitInt
    //       comment above for future C23 migration accordingly.
} future_outcome_t;
