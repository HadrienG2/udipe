#ifdef UDIPE_BUILD_BENCHMARKS

    #pragma once

    //! \file
    //! \brief Temporal outlier filter
    //!
    //! This module implements an timing data filter that attempts to detect OS
    //! scheduler driven outliers via sliding window analysis, under the
    //! assumption that the OS scheduler is responsible for the vast majority of
    //! benchmark workload interrupts.

    #include <udipe/pointer.h>

    #include "log.h"

    #include <assert.h>
    #include <stddef.h>
    #include <stdint.h>


    /// \name Configuration constants
    /// \{

    /// Width of the sliding window of inputs used for temporal outlier
    /// filtering
    ///
    /// See \ref temporal_filter_t for general overview of the temporal outlier
    /// filtering algorithm.
    ///
    /// This is the number of previous input data points kept around to assess
    /// whether a newly incoming input data point is an outlier or not. It
    /// affects temporal outlier filtering as follows:
    ///
    /// - The window width must be at least 3, and usually more. That's because
    ///   any given window may contain an outlier value, and should contain at
    ///   least two other distinct input values to be able to estimate the input
    ///   distribution spread, otherwise it will misclassify all isolated
    ///   maximal inputs as outliers. Because input values can and will often
    ///   repeat, consistently getting two distinct inputs in the input window
    ///   tends to require window widths much greater than 3.
    /// - Longer input windows improve knowledge of the input data distribution
    ///   spread (if combined with a matching reduction of \ref
    ///   TEMPORAL_TOLERANCE). Therefore they reduce the odds that an isolated
    ///   non-outlier local maxima is misclassified as an outlier.
    /// - Longer input windows lower the maximum run duration above which a
    ///   given input window will contain two OS scheduler interrupts and
    ///   outlier detection efficiency drops to 0%.
    /// - Longer input windows reduce the algorithm's ability to accomodate
    ///   qualitative changes in benchmark behavior (e.g. CPU clock rate
    ///   switches). For a longer period of time, the input window will contain
    ///   a mixture of the two behaviors, resulting in an over-estimated local
    ///   input distribution spread and thus a greater tendency to
    ///   misclassify outlier inputs as non-outliers.
    ///
    /// Currently the window width cannot be greater than 65535, but this
    /// limitation can easily be lifted if necessary.
    #define TEMPORAL_WINDOW ((uint16_t)10)
    static_assert(TEMPORAL_WINDOW >= 3,
                  "Temporal outlier detection requires at very least 3 inputs");

    /// Tolerance of the temporal outlier detection algorithm
    ///
    /// See \ref temporal_filter_t for general overview of the temporal outlier
    /// filtering algorithm.
    ///
    /// This is the correction that is applied to the empirical input maximum in
    /// order to estimate the true input distribution maxima that we would get
    /// if we could sample the input distribution for an infinite amount of time
    /// with no outlier or benchmark behavior change.
    ///
    /// As this correction is meant to compensate a small input window, it
    /// should usually be tuned down when \ref TEMPORAL_WINDOW goes up and be
    /// tuned up when \ref TEMPORAL_WINDOW goes down.
    #define TEMPORAL_TOLERANCE 0.1
    static_assert(TEMPORAL_TOLERANCE >= 0.0,
                  "TEMPORAL_TOLERANCE can only broaden the distribution");

    /// \}


    /// Temporal outlier filter
    ///
    /// This filter is mainly designed to detect benchmark run duration outliers
    /// caused by OS scheduler interrupts, which are the most common kind of
    /// duration outlier in microbenchmarks. It is based on the following
    /// observations...
    ///
    /// - OS scheduler interrupts are usually periodical (e.g. each millisecond
    ///   for a classic OS scheduler operating at 1 kHz), but can alternatively
    ///   be spaced by a guaranteed minimal amount of time instead (e.g. in
    ///   "tickless" Linux kernel configurations).
    /// - Given sufficient timing precision and a task of sufficiently stable
    ///   duration, a benchmark run that is interrupted by the OS scheduler
    ///   takes a lot longer than a benchmark run that is not interrupted by the
    ///   OS scheduler, deviating from the normal duration by much more than the
    ///   normal input duration distribution spread.
    ///
    /// ...which allows it to operate under the following hypotheses:
    ///
    /// - For sufficiently small benchmark run durations, a sliding window of
    ///   \ref TEMPORAL_WINDOW measured durations contains at most one OS
    ///   scheduler induced duration outlier. If a certain benchmark run
    ///   duration occurs more than once in such a window, it is not an OS
    ///   scheduler outlier and should be kept.
    /// - For sufficiently large values of \ref TEMPORAL_WINDOW, the empirical
    ///   input distribution spread is a good proxy for the true input
    ///   distribution spread that we would get given an infinite amount of
    ///   unperturbed data points, and said input distribution spread can
    ///   therefore be guessed by mere dilation of the empirical distribution
    ///   spread via \ref TEMPORAL_TOLERANCE.
    ///
    /// Like all statistical algorithms, the outlier detection algorithm can
    /// have false positives and false negatives, but interestingly some false
    /// positives can be detected after observing _later_ data points from the
    /// input sequence (typically if the system qualitatively undergoes a
    /// step-change in behavior between two data points). When this happens, the
    /// previously misclassified input will be returned as a second output of
    /// temporal_filter_apply().
    typedef struct temporal_filter_s {
        /// Window of previous input data points
        ///
        /// Whenever a new input comes in, it is compared with the distribution
        /// of previous inputs within `window` (which is assumed to contain
        /// either zero or one outlier) to determine whether it should be
        /// considered an outlier.
        ///
        /// After this, `window[next_idx]` is replaced by this input value,
        /// other state variables are updated as needed, and the cycle repeats
        /// for subsequent inputs.
        int64_t window[TEMPORAL_WINDOW];

        /// Minimum value from `window`
        ///
        /// The number of occurences in `window` is tracked by `min_count`.
        int64_t min;

        /// Maximum value from `window` that is known not to be an outlier
        ///
        /// If `max` is not considered to be an outlier, then this is `max`,
        /// otherwise it is a value smaller than `max` which is the largest
        /// value in `window` that is not considered to be an outlier.
        ///
        /// The number of occurences in `window` is tracked by
        /// `max_normal_count`.
        int64_t max_normal;

        /// Upper bound of the outlier tolerance range
        ///
        /// This is derived from `min` and `max_normal`, and must therefore be
        /// updated whenever any of those values is changed, which is done via
        /// temporal_filter_update_tolerance().
        ///
        /// An isolated maximum value within `window` is considered to be an
        /// outlier when it is greater than this threshold.
        int64_t upper_tolerance;

        /// Maximum value from `window`, which may or may not be an outlier
        ///
        /// This will differ from `max_normal` if and only if there is a single
        /// value above `max_normal` that is considered to be an outlier.
        int64_t max;

        /// Position of the oldest entry of `window`
        ///
        /// The next input will be inserted here, overwriting the oldest entry.
        /// Other filter state will be adjusted to account for the addition of a
        /// new data point and the removal of an old data point.
        uint16_t next_idx;

        /// Number of occurences of `min` in `window`
        ///
        /// When this drops to 0, `min`, `min_count` and `upper_tolerance` must
        /// be updated according to the new minimum value of `window`, which is
        /// done via temporal_filter_set_min() and
        /// temporal_filter_update_tolerance().
        uint16_t min_count;

        /// Number of occurences of `max_normal` in `window`
        ///
        /// When this drops to 0, `max_normal`, `max_normal_count` and
        /// `upper_tolerance` must be updated according to the new maximum
        /// non-outlier value of `window`, which is done via
        /// temporal_filter_reset_maxima().
        uint16_t max_normal_count;

        /// Index of the last input that was classified as an outlier back when
        /// it was an isolated `max`, or \ref TEMPORAL_WINDOW to denote the
        /// absence of outliers in the input window
        uint16_t outlier_idx;
    } temporal_filter_t;


    /// \name Implementation details
    /// \{

    /// Set `min` and `min_count` according to the contents of `window`
    ///
    /// This function is an implementation detail of other functions that
    /// shouldn't be called directly.
    ///
    /// \internal
    ///
    /// This function sets `min` and `min_count` according to the current
    /// contents of `window`. It does not read or write any other fields, which
    /// means that...
    ///
    /// - It can be called at a point where only `filter->window` is
    ///   initialized.
    /// - It will not set `max_normal`, `upper_tolerance`, `max` or
    ///   `upper_tolerance`, you need to call one of the other functions below
    ///   to perform this state update if necessary.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param filter is a temporal filter that has been initialized with
    ///               temporal_filter_initialize() and hasn't been destroyed
    ///               with temporal_filter_finalize() yet.
    UDIPE_NON_NULL_ARGS
    void temporal_filter_set_min(temporal_filter_t* filter);

    /// Set `max`, `upper_tolerance`, `max_normal` and `max_normal_count`
    /// according to the current contents of `window`
    ///
    /// This function is an implementation detail of other functions that
    /// shouldn't be called directly.
    ///
    /// \internal
    ///
    /// This function uses `min`, which must be up to date. It can be deduced
    /// from `window` using temporal_filter_set_min() if necessary.
    ///
    /// From this initial state, this function will set `max`,
    /// `upper_tolerance`, `max_normal` and `max_normal_count` to a value that
    /// is correct when `window` is the full input dataset.
    ///
    /// This will produce correct results when called on a freshly constructed
    /// \ref temporal_filter_t. However, if called on a \ref temporal_filter_t
    /// that has more input history behind its current input window, it may
    /// reclassify inputs which were previously classified as normal as
    /// outliers. This is undesirable as it may lead to inputs being emitted
    /// multiple times. Therefore, after initialization,
    /// temporal_filter_reset_maxima() must be used instead to avoid this
    /// problem (and should be faster as a bonus).
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param filter is a temporal outlier filter that has been initialized
    ///               with temporal_filter_initialize() and hasn't been
    ///               destroyed with temporal_filter_finalize() yet.
    UDIPE_NON_NULL_ARGS
    void temporal_filter_init_maxima(temporal_filter_t* filter);

    /// Update an outlier filter's `upper_tolerance` value
    ///
    /// This function is an implementation detail of other functions that
    /// shouldn't be called directly.
    ///
    /// \internal
    ///
    /// This function must be called between any change to `min` or `max_normal`
    /// and any later use of `upper_tolerance`. It uses `min` and `max_normal`
    /// and must therefore be called at a point where these values are known.
    ///
    /// On its own, this function does not affect the current outlier
    /// classification status of `max` and `max_normal`, it is more of a
    /// preparatory step towards such reclassification.
    ///
    /// It must be called within the scope of with_logger().
    ///
    /// \param filter is a temporal filter that has been initialized with
    ///               temporal_filter_initialize() and hasn't been destroyed
    ///               with temporal_filter_finalize() yet.
    UDIPE_NON_NULL_ARGS
    void temporal_filter_update_tolerance(temporal_filter_t* filter);

    /// \copydoc temporal_filter_result_s
    ///
    typedef struct temporal_filter_result_s temporal_filter_result_t;

    /// Reclassify a temporal outlier filter's maximum value as normal
    ///
    /// This function is an implementation detail of temporal_filter_apply()
    /// that shouldn't be called directly.
    ///
    /// \internal
    ///
    /// This function is used when `max` was previously classified as an
    /// outlier, but it is later discovered that it should not be for some
    /// reason.
    ///
    /// It invalidates `upper_tolerance` and must therefore be followed by a
    /// call to temporal_filter_update_tolerance() before the next use of
    /// `upper_tolerance`.
    ///
    /// It must be called within the scope of with_logger().
    ///
    /// \param filter is a temporal outlier filter that has been initialized
    ///               with temporal_filter_initialize() and hasn't been
    ///               destroyed with temporal_filter_finalize() yet.
    /// \param result is a \ref temporal_filter_result_t whose fields will be
    ///               set to indicate to the caller that `max` is not an outlier
    ///               after all.
    /// \param reason indicates the reason why the input was reclassified as
    ///               non-outlier, which will be logged.
    UDIPE_NON_NULL_ARGS
    void temporal_filter_make_max_normal(temporal_filter_t* filter,
                                        temporal_filter_result_t* result,
                                        const char reason[]);

    /// Update a temporal outlier filter's state after encountering an input
    /// smaller than its current `min`
    ///
    /// This function is an implementation detail of temporal_filter_apply() that
    /// shouldn't be called directly.
    ///
    /// \internal
    ///
    /// Decreasing `min` increases `upper_tolerance`, which may lead a `max`
    /// that is currently classified as an outlier to be reclassified as
    /// non-outlier. This function will call temporal_filter_make_max_normal()
    /// for you in this case, which is the reason why it takes a `result`
    /// out-parameter.
    ///
    /// This function may or may not update `upper_tolerance`. Use its return
    /// value to tell if it must be updated via
    /// temporal_filter_update_tolerance().
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param filter is a temporal outlier filter that has been initialized
    ///               with temporal_filter_initialize() and hasn't been
    ///               destroyed with temporal_filter_finalize() yet.
    /// \param result is a \ref temporal_filter_result_t whose fields may be set
    ///               if this change of minimum leads to a reclassification of
    ///               `max` as non-outlier.
    /// \param new_min indicates the new minimum value of the input window.
    ///
    /// \returns the truth that `upper_tolerance` must be updated using
    ///          temporal_filter_update_tolerance().
    UDIPE_NON_NULL_ARGS
    bool temporal_filter_decrease_min(temporal_filter_t* filter,
                                     temporal_filter_result_t* result,
                                     int64_t new_min);

    /// Update a temporal outlier filter's state after encountering an input
    /// larger than its current `max`
    ///
    /// This function is an implementation detail of temporal_filter_apply()
    /// that shouldn't be called directly.
    ///
    /// \internal
    ///
    /// If the current `max` is classified as an outlier, then it must be
    /// reclassified as non-outlier because there can be at most one outlier
    /// input in the data window. This function will call
    /// temporal_filter_make_max_normal() for you in this case, which is the
    /// reason why it takes a `result` out-parameter.
    ///
    /// This function uses the current value of `upper_tolerance`, and may or
    /// may not update it if it changes `max_normal`. Use its return value to
    /// tell if `upper_tolerance` must be updated via
    /// temporal_filter_update_tolerance().
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param filter is a temporal outlier filter that has been initialized
    ///               with temporal_filter_initialize() and hasn't been
    ///               destroyed with temporal_filter_finalize() yet.
    /// \param result is a \ref temporal_filter_result_t whose fields may be set
    ///               if this change of minimum leads to a reclassification of
    ///               `max` as non-outlier.
    /// \param new_max indicates the new maximum value of the input window.
    ///
    /// \returns the truth that `upper_tolerance` must be updated using
    ///          temporal_filter_update_tolerance().
    UDIPE_NON_NULL_ARGS
    bool temporal_filter_increase_max(temporal_filter_t* filter,
                                     temporal_filter_result_t* result,
                                     int64_t new_max);

    /// Update a temporal outlier filter's state after encountering an input
    /// larger than its current `max_normal`
    ///
    /// This function is an implementation detail of temporal_filter_apply() that
    /// shouldn't be called directly.
    ///
    /// \internal
    ///
    /// This function can only be called if `max` is currently considered to be
    /// an outlier, otherwise temporal_filter_increase_max() will be called
    /// instead.
    ///
    /// Increasing `max_normal` increases `upper_tolerance`, which may lead
    /// `max` to be reclassified as non-outlier. This function will call
    /// temporal_filter_make_max_normal() for you in this case, which is the
    /// reason why it takes a `result` out-parameter.
    ///
    /// This function may or may not update `upper_tolerance`. Use its return
    /// value to tell if it must be updated via
    /// temporal_filter_update_tolerance().
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param filter is a temporal outlier filter that has been initialized
    ///               with temporal_filter_initialize() and hasn't been
    ///               destroyed with temporal_filter_finalize() yet.
    /// \param result is a \ref temporal_filter_result_t whose fields may be set
    ///               if this change of minimum leads to a reclassification of
    ///               `max` as non-outlier.
    /// \param new_max_normal indicates the new maximum non-outlier value of the
    ///                       input window.
    ///
    /// \returns the truth that `upper_tolerance` must be updated using
    ///          temporal_filter_update_tolerance().
    UDIPE_NON_NULL_ARGS
    bool temporal_filter_increase_max_normal(temporal_filter_t* filter,
                                            temporal_filter_result_t* result,
                                            int64_t new_max_normal);

    /// Reset `max`, `upper_tolerance`, `max_normal` and `max_normal_count`
    /// after the last occurence of `max_normal` has been discarded to make room
    /// for new input
    ///
    /// This function is an implementation detail of other functions that
    /// shouldn't be called directly.
    ///
    /// \internal
    ///
    /// This function uses `min`, which must be up to date. It can be deduced
    /// from `window` using temporal_filter_set_min() if necessary.
    ///
    /// It also uses `outlier_idx` which is initialized by
    /// temporal_filter_init_maxima() and updated by temporal_filter_apply()
    /// during the part of the cycle where new inputs are classified as
    /// (non-)outliers and old inputs are discarded.
    ///
    /// From this initial state, this function will adjust `max`,
    /// `upper_tolerance`, `max_normal` and `max_normal_count` as necessary to
    /// match current contents of `window` and previous decisions to classify
    /// maxima as outliers.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param filter is a temporal outlier filter that has been initialized
    ///               with temporal_filter_initialize() and hasn't been
    ///               destroyed with temporal_filter_finalize() yet.
    UDIPE_NON_NULL_ARGS
    void temporal_filter_reset_maxima(temporal_filter_t* filter);

    /// \}


    /// \name Public API
    /// \{

    /// Set up a temporal outlier filter
    ///
    /// To avoid initially operating with worse classification characteristics
    /// and constantly checking for an initial vs steady state, the outlier
    /// filter must be "seeded" with a full window of input values.
    ///
    /// After this is done, you can use the TEMPORAL_FILTER_FOREACH_NORMAL()
    /// macro to iterate over the initial input values from this window that are
    /// not considered to be outliers.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param initial_window is the set of input values that the detector will
    ///        be seeded with.
    ///
    /// \returns a temporal outlier filter that must later be finalized with
    ///          temporal_filter_finalize().
    temporal_filter_t
    temporal_filter_initialize(const int64_t initial_window[TEMPORAL_WINDOW]);

    /// Iterate over all previous inputs from a temporal_filter_t's input window
    /// that are not considered to be outliers
    ///
    /// This is normally used after temporal_filter_initialize() to collect the
    /// initial list of non-outlier inputs, excluding any detected outlier, so
    /// that data from the initial input window is not lost.
    ///
    /// \param filter_ptr must be a pointer to a \ref temporal_filter_t.
    /// \param value_ident must be an unused variable name. A constant with this
    ///                    name will be created and receive the value of the
    ///                    current non-outlier value on each iteration.
    ///
    /// The remainder of this macro's input is a code block that indicates what
    /// must be done with each non-outlier value from the input dataset, which
    /// using `value_indent` to refer to said value.
    #define TEMPORAL_FILTER_FOREACH_NORMAL(filter_ptr, value_ident, ...)  \
        const temporal_filter_t* udipe_filter = (filter_ptr);  \
        for (size_t udipe_iter = 0; udipe_iter < TEMPORAL_WINDOW; ++udipe_iter) {  \
            const size_t udipe_idx =  \
                (udipe_filter->next_idx + udipe_iter) % TEMPORAL_WINDOW;  \
            const int64_t value_ident = udipe_filter->window[udipe_idx];  \
            if (value_ident > udipe_filter->upper_tolerance) continue;  \
            do __VA_ARGS__ while(false);  \
        }

    /// Result of temporal_filter_apply()
    ///
    /// This indicates whether the current input is considered to be an outlier,
    /// and whether a former input that was previously classified as an outlier
    /// has been reclassified as non-outlier.
    struct temporal_filter_result_s {
        /// Truth that the current input is an outlier
        ///
        /// If this is true, then the `input` duration that was passed to
        /// temporal_filer_apply() is likely to have been enlarged by an OS
        /// interrupt and should not be inserted into the output distribution.
        bool current_is_outlier;

        /// Truth that a previous input was misclassified as an outlier
        ///
        /// If this is `true`, then `previous_input` is set and can be inserted
        /// into the output data distribution, along with the current `input` if
        /// it is itself not classified as an outlier by `current_is_outlier`.
        bool previous_not_outlier;

        /// Previous input that was misclassified as an outlier
        ///
        /// This member is only set when `previous_not_outlier` is true.
        int64_t previous_input;
    };

    /// Record a new input data point, tell if it looks an outlier and possibly
    /// reclassify a previous outlier as non-outlier in the process
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param filter is a temporal outlier filter that has been initialized
    ///               with temporal_filter_initialize() and hasn't been
    ///               destroyed with temporal_filter_finalize() yet.
    ///
    /// \returns the truth that the current input should be treated as an
    ///          outlier and that a former input was wrongly classified as an
    ///          outlier and should be included in the normal dataset after all.
    UDIPE_NON_NULL_ARGS
    static inline
    temporal_filter_result_t temporal_filter_apply(temporal_filter_t* filter,
                                                 int64_t input) {
        assert(filter->min <= filter->max_normal);
        assert(filter->max_normal <= filter->max);
        assert(filter->max_normal <= filter->upper_tolerance);
        assert(filter->next_idx < TEMPORAL_WINDOW);
        assert(filter->min_count >= 1);
        assert(filter->max_normal_count >= 1);

        temporal_filter_result_t result = (temporal_filter_result_t){
            .previous_not_outlier = false
        };

        tracef("Integrating new input %zd...", input);
        bool must_update_tolerance = false;
        if (input < filter->min) {
            trace("Input is the new min.");
            must_update_tolerance = temporal_filter_decrease_min(filter,
                                                                &result,
                                                                input);
        } else if (input > filter->max) {
            trace("Input is the new max.");
            must_update_tolerance = temporal_filter_increase_max(filter,
                                                                &result,
                                                                input);
        } else if (input > filter->max_normal && input < filter->max) {
            trace("Input is the new max_normal.");
            must_update_tolerance = temporal_filter_increase_max_normal(filter,
                                                                       &result,
                                                                       input);
        } else {
            assert(input >= filter->min &&
                   (input <= filter->max_normal || input == filter->max));
            if (input == filter->min) {
                trace("Input is another occurence of min.");
                ++(filter->min_count);
            }
            // This if statement is disjoint from the previous one on purpose:
            // min == max_normal is a valid state even though it is suspicious
            // and suggests TEMPORAL_WINDOW is too small.
            if (input == filter->max_normal) {
                trace("Input is another occurence of max_normal.");
                ++(filter->max_normal_count);
            } else if (input == filter->max) {
                assert(filter->max > filter->max_normal);
                trace("Input is another occurence of max, "
                      "which is thus not an outlier.");
                temporal_filter_make_max_normal(
                    filter,
                    &result,
                    "encountered another occurence"
                );
                ++(filter->max_normal_count);
                must_update_tolerance = true;
            }
        }

        trace("Classifying input...");
        if (must_update_tolerance) temporal_filter_update_tolerance(filter);
        result.current_is_outlier = (input > filter->upper_tolerance);
        if (result.current_is_outlier) {
            trace("Input is considered an outlier, but "
                  "later data points may disprove this assessment.");
            filter->outlier_idx = filter->next_idx;
        } else {
            trace("Input is not an outlier and will never be considered one.");
            if (filter->outlier_idx == filter->next_idx) {
                filter->outlier_idx = TEMPORAL_WINDOW;
            }
        }

        const int64_t removed = filter->window[filter->next_idx];
        tracef("Replacing oldest input %zd...", removed);
        assert(removed >= filter->min && removed <= filter->max);
        filter->window[filter->next_idx] = input;
        filter->next_idx = (filter->next_idx + 1) % TEMPORAL_WINDOW;
        if (removed == filter->min) --(filter->min_count);
        const int64_t former_max = filter->max;
        if (removed == filter->max) filter->max = filter->max_normal;
        if (removed == filter->max_normal) --(filter->max_normal_count);
        //
        const bool removed_max_normal = filter->max_normal_count == 0;
        if (filter->min_count == 0) {
            trace("Last occurence of min escaped window, reset min...");
            const int64_t prev_min = filter->min;
            temporal_filter_set_min(filter);
            // This operation can only increase the minimum, which will reduce
            // upper_tolerance in a fashion that could theoretically reclassify
            // a former isolated max_normal value as an outlier if filter stats
            // were strictly derived from the current contents of window.
            //
            // But we want to avoid such non-outlier to outlier
            // reclassification: a data point should only be classified as an
            // outlier if no input window ever classified it as non-outlier.
            assert(filter->min > prev_min);
            // Furthermore, because a window contains at least 3 data points, we
            // removed only one data point and we know that min_count was
            // formerly 1, there are at least two values strictly greater than
            // min which were not removed. At least one of them must be
            // max_normal per the single-outlier hypothesis, and one of them
            // (possibly the same one) must be max. Combining this and the
            // above, maxima are unaffected and don't need to be recomputed.
            static_assert(
                TEMPORAL_WINDOW >= 3,
                "Need at least two points other than an outlier to tell min/max"
            );
            assert(!removed_max_normal);
            // As a result, only upper_tolerance needs to be recomputed.
            temporal_filter_update_tolerance(filter);
        } else if (removed_max_normal) {
            tracef("Last occurence of max_normal = %zd escaped window, "
                   "reset maxima...", filter->max_normal);
            temporal_filter_reset_maxima(filter);
        }

        return result;
    }

    /// Destroy a temporal outlier filter
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param filter is an temporal outlier filter that has been initialized
    ///               with temporal_filter_initialize() and hasn't been
    ///               destroyed with temporal_filter_finalize() yet.
    UDIPE_NON_NULL_ARGS
    void temporal_filter_finalize(temporal_filter_t* filter);

    /// \}


    #ifdef UDIPE_BUILD_TESTS
        /// Unit tests
        ///
        /// This function runs all the unit tests for this module. It must be called
        /// within the scope of with_logger().
        void temporal_filter_unit_tests();
    #endif

#endif  // UDIPE_BUILD_BENCHMARKS