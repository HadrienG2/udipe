#ifdef UDIPE_BUILD_BENCHMARKS

    #pragma once

    //! \file
    //! \brief Benchmarking utilities
    //!
    //! This supplements the "public" interface defined inside of
    //! `udipe/benchmarks.h` with private utilities that can only be used within
    //! the libudipe codebase and microbenchmarks thereof.

    // TODO: Consider splitting this module into a directory of more specialized
    //       benchmarking modules like benchmark/distribution.h,
    //       benchmark/stats.h, benchmark/clock.h...

    #include <udipe/benchmark.h>

    #include <udipe/pointer.h>
    #include <udipe/time.h>

    #include "arch.h"
    #include "error.h"
    #include "log.h"
    #include "memory.h"
    #include "name_filter.h"

    #include <assert.h>
    #include <hwloc.h>
    #include <stdalign.h>
    #include <stddef.h>
    #include <stdint.h>
    #include <time.h>

    #ifdef __unix__
        #include <time.h>
        #include <unistd.h>
    #elif defined(_WIN32)
        #include <profileapi.h>
    #endif


    /// \name Temporal outlier filter
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
    #define TEMPORAL_TOLERANCE 0.5
    static_assert(TEMPORAL_TOLERANCE >= 0.0,
                  "TEMPORAL_TOLERANCE can only broaden the distribution");

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

    /// Result of temporal_filter_apply()
    ///
    /// This indicates whether the current input is considered to be an outlier,
    /// and whether a former input that was previously classified as an outlier
    /// has been reclassified as non-outlier.
    typedef struct temporal_filter_result_s {
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
    } temporal_filter_result_t;

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
                                        const char* reason);

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

    // TODO: Add tests then integrate including FOREACH_NORMAL

    /// \}


    /// \name Duration-based value distribution
    /// \{

    /// Mechanism for recording then sampling the distribution of a
    /// duration-based dataset
    ///
    /// This encodes a set of duration-based values with a sparse histogram
    /// format. If we denote `N` the number of histogram bins, which is the
    /// number of distinct values that were inserted into the distribution so
    /// far, then this data structure has...
    ///
    /// - `O(N)` memory usage (and thus `O(N)` cache footprint)
    /// - `O(N)` cost for inserting a previously unseen value
    /// - `O(log(N))` cost for incrementing a known value's occurence count
    /// - `O(log(N))` cost for randomly sampling a value
    ///
    /// This works well in practice because duration datasets tend to feature
    /// many occurences of a few values, which in turn happens because...
    ///
    /// - Computer clocks have a coarse granularity, which leads slightly
    ///   different durations to be measured as the same duration.
    /// - Program execution durations tend to exhibit multi-modal timing laws
    ///   for various reasons (whether some data is in cache or not, whether a
    ///   CPU backend slot is available at the start of a loop or not...).
    ///
    /// To maximize code sharing between different clocks (system, CPU...) and
    /// different stages of the benchmarking process (calibration,
    /// measurement...), the unit of inner values is purposely left unspecified.
    ///
    /// A \ref distribution_t has a multi-stage lifecycle:
    ///
    /// - At first, distribution_initialize() is called, returning an empty \ref
    ///   distribution_builder_t.
    /// - Values are then added into this \ref distribution_builder_t using
    ///   distribution_insert().
    /// - Once all values have been inserted, distribution_build() is called,
    ///   turning the \ref distribution_builder_t into a `distribution_t` that
    ///   can be sampled with distribution_sample().
    /// - Once the distribution is no longer useful, it can either be turned
    ///   back into an empty \ref distribution_builder_t using
    ///   distribution_reset() or destroyed using distribution_finalize().
    typedef struct distribution_s {
        /// Memory allocation in which the histogram is stored
        ///
        /// Histogram data layout is as follows:
        ///
        /// 1. At the start of the allocation, there is a sorted array of `len`
        ///    previously inserted values of type `int64_t`.
        /// 2. At byte offset `capacity * sizeof(int64_t)`, there is an array
        ///    of `len` values of type `size_t`, whose contents depends on the
        ///    current stage of the distribution lifecycle:
        ///     - At the initial \ref distribution_builder_t stage, this array
        ///       contains the number of occurences of each value.
        ///     - At the final \ref distribution_t stage, this array instead
        ///       contains the number of occurences of values smaller than or
        ///       equal to each value, i.e. the cumulative sum of the
        ///       aforementioned occurence count.
        void* allocation;

        /// Number of bins that the histogram currently has
        ///
        /// See `allocation` for more information about how histogram bin data
        /// is laid out in memory.
        size_t num_bins;

        /// Maximum number of bins that the histogram can hold
        ///
        /// Allocation size is `capacity * sizeof(int64_t) + capacity *
        /// sizeof(size_t)`.
        ///
        /// Every time this capacity limit is reached, a new allocation of
        /// double capacity is allocated, then the contents of the old
        /// allocation are migrated in there, then the old allocation is
        /// liberated. This strategy borrowed from C++'s `std::vector` ensures
        /// that allocation costs are amortized constant not linear.
        size_t capacity;
    } distribution_t;

    /// Memory layout of a \ref distribution_builder_t or \ref distribution_t
    ///
    /// This layout information is be computed using distribution_layout().
    ///
    /// In the case of \ref distribution_builder_t, it is invalidated when the
    /// allocation is grown (as signaled by distribution_grow()) or when it is
    /// turned into a \ref distribution_t through distribution_build().
    ///
    /// In the case of \ref distribution_t, it is invalidated when the
    /// distribution is recycled into a \ref distribution_builder_t by
    /// distribution_reset() or when it is destroyed by distribution_finalize().
    typedef struct distribution_layout_s {
        /// Sorted list of previously inserted values
        int64_t* sorted_values;

        /// Matching value occurence counts or cumsum thereof
        union {
            /// Value occurence counts from a \ref distribution_builder_t
            size_t* counts;

            /// Cumulative occurence counts from all bins up to and including
            /// the current bin of a \ref distribution_t.
            ///
            /// This is the cumulative sum of the `counts` from the \ref
            /// distribution_builder_t that was used to build a \ref
            /// distribution_t.
            size_t* end_indices;
        };
    } distribution_layout_t;

    /// Determine the layout of a duration-based value distribution
    ///
    /// \param allocation is the internal `allocation` of a \ref
    ///                   distribution_builder_t or \ref distribution_t.
    /// \param capacity is the internal `capacity` of a \ref
    ///                 distribution_builder_t or \ref distribution_t.
    ///
    /// \returns layout information that is valid until the point specified in
    ///          the documentation of \ref distribution_layout_t.
    UDIPE_NON_NULL_ARGS
    static inline
    distribution_layout_t distribution_layout(const distribution_t* dist) {
        assert(dist->allocation);
        assert(alignof(int64_t) >= alignof(size_t));
        return (distribution_layout_t){
            .sorted_values = (int64_t*)(dist->allocation),
            .counts = (size_t*)(
                (char*)(dist->allocation) + dist->capacity * sizeof(int64_t)
            )
        };
    }

    /// \ref distribution_t wrapper used during initial data recording
    ///
    /// This is a \ref distribution_t that is wrapped into a different type in
    /// order to check for correct usage at compile time.
    typedef struct distribution_builder_s {
        distribution_t inner;  ///< Internal data collection backend
    } distribution_builder_t;

    /// Set up storage for a distribution
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \returns a \ref distribution_builder_t that can be filled with values
    ///          via distribution_insert(), then turned into a \ref
    ///          distribution_t via distribution_build().
    distribution_builder_t distribution_initialize();

    /// Create a new histogram bin
    ///
    /// This is an implementation detail of distribution_insert() that should
    /// not be used directly.
    ///
    /// \internal
    ///
    /// This creates a new histogram bin associated with value `value` at
    /// position `pos`, with an occurence count of 1. It reallocates storage and
    /// moves existing data around as needed to make room for this new bin.
    ///
    /// The caller of this function must honor the histogram invariants:
    ///
    /// - `pos` must be in range `[0; dist->num_bins]`, i.e. it must either
    ///   correspond to the position of an existing bin or lie one bin past the
    ///   end of the histogram.
    /// - `value` must be strictly larger than the value associated with the
    ///   existing histogram bin at position `pos - 1`, if any.
    /// - `value` must be strictly smaller than the value assocated with the
    ///   histogram bin that was formerly at position `pos`, if any.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_builder_t that has previously
    ///             been set up via distribution_initialize() and hasn't yet
    ///             been turned into a \ref distribution_t by a call to
    ///             distribution_build().
    /// \param pos is the index at which the newly inserted bin will be
    ///            inserted. It must match the constraints spelled out earlier
    ///            in this documentation.
    /// \param value is the value that will be inserted at this position. It
    ///              must match the constraints spelled out earlier in this
    ///              documentation.
    UDIPE_NON_NULL_ARGS
    void distribution_create_bin(distribution_builder_t* builder,
                                 size_t pos,
                                 int64_t value);

    /// Insert a value into a distribution
    ///
    /// This inserts a new occurence of `value` into the distribution histogram,
    /// creating a new bin if needed to make room for it.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_builder_t that has previously
    ///             been set up via distribution_initialize() and hasn't yet
    ///             been turned into a \ref distribution_t via
    ///             distribution_build().
    /// \param value is the value to be inserted.
    UDIPE_NON_NULL_ARGS
    static inline void distribution_insert(distribution_builder_t* builder,
                                           int64_t value) {
        // Determine the histogram's memory layout
        distribution_t* dist = &builder->inner;
        distribution_layout_t layout = distribution_layout(dist);
        const size_t end_pos = dist->num_bins;
        tracef("Asked to insert value %zd into an histogram with %zu bins.",
               value, dist->num_bins);

        // Handle the empty histogram edge case
        if (dist->num_bins == 0) {
            trace("Histogram is empty, will create first bin.");
            distribution_create_bin(builder, end_pos, value);
            return;
        }

        // Handle values at or above the histogram's maximum value
        const size_t last_pos = end_pos - 1;
        const int64_t last_value = layout.sorted_values[last_pos];
        if (value > last_value) {
            tracef("Value is past the end of histogram %zd, "
                   "will become new last bin #%zu.",
                   last_value, end_pos);
            distribution_create_bin(builder, end_pos, value);
            return;
        } else if (value == last_value) {
            tracef("Value belongs to last bin #%zu, will increment it.",
                   last_pos);
            ++layout.counts[last_pos];
            return;
        }
        assert(value < last_value);

        // Handle values at or below the histogram's minimum value
        const size_t first_pos = 0;
        const int64_t first_value = layout.sorted_values[first_pos];
        if (value < first_value) {
            tracef("Value is before the start of histogram %zd, "
                   "will become new first bin.",
                   first_value);
            distribution_create_bin(builder, first_pos, value);
            return;
        } else if (value == first_value) {
            trace("Value belongs to first bin, will increment it.");
            ++layout.counts[first_pos];
            return;
        }
        assert(value > first_value);

        // At this point, we have established the following:
        // - There are at least two bins in the histogram
        // - The input value is strictly greater than the minimum value
        // - The input value is strictly smaller than the maximum value
        //
        // Use binary search to locate either the bin where the input value
        // belongs or the pair of bins between which it should be inserted.
        size_t below_pos = first_pos;
        size_t above_pos = last_pos;
        trace("Value belongs to the middle of the histogram, "
              "will now find where via binary search...");
        while (above_pos - below_pos > 1) {
            assert(below_pos < above_pos);
            const int64_t below_value = layout.sorted_values[below_pos];
            assert(below_value < value);
            const int64_t above_value = layout.sorted_values[above_pos];
            assert(above_value > value);
            tracef("- Value is in range ]%zd; %zd[ from bins ]%zu; %zu[.",
                   below_value, above_value, below_pos, above_pos);

            const size_t middle_pos = below_pos + (above_pos - below_pos) / 2;
            assert(below_pos <= middle_pos);
            assert(middle_pos < above_pos);
            const int64_t middle_value = layout.sorted_values[middle_pos];
            assert(below_value <= middle_value);
            assert(middle_value < above_value);
            tracef("- Investigating middle value %zd from bin #%zu...",
                   middle_value, middle_pos);

            if (middle_value > value) {
                trace("- It's larger: can eliminate all subsequent bins.");
                above_pos = middle_pos;
                continue;
            } else if (middle_value < value) {
                trace("- It's smaller: can eliminate all previous bins.");
                below_pos = middle_pos;
                continue;
            } else {
                assert(middle_value == value);
                trace("- It's a bin for this value: increment count and return.");
                ++layout.counts[middle_pos];
                return;
            }
        }

        // Narrowed down a pair of bins between which the value belongs, insert
        // a new bin at the appropriate position
        tracef("Narrowed search interval to 1-bin gap ]%zu; %zu[: "
               "must insert new bin at position #%zu.",
               below_pos, above_pos, above_pos);
        assert(above_pos == below_pos + 1);
        assert(value > layout.sorted_values[below_pos]);
        assert(value < layout.sorted_values[above_pos]);
        distribution_create_bin(builder, above_pos, value);
    }

    /// Log the current state of a distribution builder
    ///
    /// This is typically done right before calling distribution_build(), to
    /// check out the final state of the distribution after performing all
    /// insertions.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param builder must be a \ref distribution_builder_t that has previously
    ///                received at least one value via distribution_insert() and
    ///                hasn't yet been turned into a \ref distribution_t via
    ///                distribution_build().
    /// \param level is the log level at which the distribution state should be
    ///              logged.
    /// \param header is a string that will precede the distribution display in
    ///                logs.
    UDIPE_NON_NULL_ARGS
    void distribution_log(distribution_builder_t* builder,
                          udipe_log_level_t level,
                          const char* header);

    /// Switch a distribution from the building stage to the sampling stage
    ///
    /// This can only be done after at least one value has been inserted into
    /// the distribution via distribution_insert().
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param builder must be a \ref distribution_builder_t that has previously
    ///                received at least one value via distribution_insert() and
    ///                hasn't yet been turned into a \ref distribution_t via
    ///                distribution_build().
    ///
    /// \returns a distribution that can be sampled via distribution_sample()
    UDIPE_NON_NULL_ARGS
    distribution_t distribution_build(distribution_builder_t* builder);

    /// Discard a distribution builder without building the associated
    /// distribution
    ///
    /// The distribution builder must not be used after calling this function.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param builder must be a \ref distribution_builder_t that has previously
    ///                received at least one value via distribution_insert() and
    ///                hasn't yet been turned into a \ref distribution_t via
    ///                distribution_build().
    UDIPE_NON_NULL_ARGS
    void distribution_discard(distribution_builder_t* builder);


    /// Number of values that were inserted into a \ref distribution_builder_t
    /// before the associated \ref distribution_t was built
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't yet been recycled via
    ///             distribution_reset() or destroyed via
    ///             distribution_finalize().
    ///
    /// \returns the number of values that were inserted into the \ref
    ///          distribution_builder_t from which this `distribution_t` was
    ///          built.
    UDIPE_NON_NULL_ARGS
    static inline size_t distribution_len(const distribution_t* dist) {
        assert(dist->num_bins >= (size_t)1);
        distribution_layout_t layout = distribution_layout(dist);
        return layout.end_indices[dist->num_bins - 1];
    }

    /// Sample a value from a duration-based distribution
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't yet been recycled via
    ///             distribution_reset() or destroyed via
    ///             distribution_finalize().
    ///
    /// \returns One of the values that was previously inserted into the
    ///          distribution via distribution_insert().
    UDIPE_NON_NULL_ARGS
    static inline int64_t distribution_sample(const distribution_t* dist) {
        // Determine the histogram's memory layout
        const distribution_layout_t layout = distribution_layout(dist);

        // Sample one value as the index that this value would have in a dense
        // array of duplicated values
        assert(dist->num_bins >= (size_t)1);
        const size_t num_values = distribution_len(dist);
        const size_t value_idx = rand() % num_values;
        tracef("Sampling %zu-th value from a distribution containing %zu values, "
               "spread across %zu bins.",
               value_idx, num_values, dist->num_bins);

        // Handle the case where the value is in the first histogram bin
        const size_t first_pos = 0;
        const size_t first_end_idx = layout.end_indices[first_pos];
        if (value_idx < first_end_idx) {
            const int64_t first_value = layout.sorted_values[first_pos];
            tracef("This is value %zd from first bin with end index %zu.",
                   first_value, first_end_idx);
            return first_value;
        }

        // At this point, we have established the following:
        // - There are at least two bins in the histogram, because value_idx
        //   must belong to at least one bin and it's not the first one
        // - value_idx does not belong to the first bin
        // - value_idx is in bounds, so it must belong to the last bin or one of
        //   the previous bins
        //
        // Use binary search to locate the bin to which value_idx belongs, which
        // is the first bin whose end index is strictly greater than value_idx.
        size_t below_pos = first_pos;
        size_t above_pos = dist->num_bins - 1;
        trace("Value belongs to the middle of the histogram, "
              "will now find where via binary search...");
        while (above_pos - below_pos > 1) {
            assert(below_pos < above_pos);
            const size_t below_end_idx = layout.end_indices[below_pos];
            assert(below_end_idx <= value_idx);
            const size_t above_end_idx = layout.end_indices[above_pos];
            assert(above_end_idx > value_idx);
            tracef("- Value index is in range [%zd; %zd[ from bins ]%zu; %zu].",
                   below_end_idx, above_end_idx, below_pos, above_pos);

            const size_t middle_pos = below_pos + (above_pos - below_pos) / 2;
            assert(below_pos <= middle_pos);
            assert(middle_pos < above_pos);
            const size_t middle_end_idx = layout.end_indices[middle_pos];
            assert(below_end_idx <= middle_end_idx);
            assert(middle_end_idx < above_end_idx);
            tracef("- Investigating middle end index %zd from bin #%zu...",
                   middle_end_idx, middle_pos);

            if (middle_end_idx > value_idx) {
                trace("- It's larger: can eliminate all subsequent bins.");
                above_pos = middle_pos;
            } else {
                trace("- It's smaller: can eliminate all previous bins.");
                assert(middle_end_idx <= value_idx);
                below_pos = middle_pos;
            }
        }

        // Narrowed down the pair of bins to which value_idx belongs
        const size_t below_end_idx = layout.end_indices[below_pos];
        const size_t above_end_idx = layout.end_indices[above_pos];
        tracef("Narrowed search to single bin ]%zu; %zu]: "
               "value_idx must come from bin #%zu.",
               below_pos, above_pos, above_pos);
        assert(above_pos == below_pos + 1);
        assert(value_idx >= below_end_idx);
        assert(value_idx < above_end_idx);
        return layout.sorted_values[above_pos];
    }

    /// Estimate a distribution of `left - right` differences
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param builder is a distribution builder within which output data will
    ///                be inserted, which should initially be empty (either
    ///                freshly built via distribution_initialize() or freshly
    ///                recycled via distribution_reset()). It will be turned
    ///                into the output distribution returned by this function,
    ///                and therefore cannot be used after calling this function.
    /// \param left is the distribution from which data points associated with
    ///             the left hand side of the subtraction will be taken.
    /// \param right is the distribution from which data points associated with
    ///              the right hand side of the subtraction will be taken.
    ///
    /// \returns an estimated distribution of `left - right` differences
    UDIPE_NON_NULL_ARGS
    distribution_t distribution_sub(distribution_builder_t* builder,
                                    const distribution_t* left,
                                    const distribution_t* right);

    /// Estimate a distribution of `num * factor / denom` differences
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param builder is a distribution builder within which output data will
    ///                be inserted, which should initially be empty (either
    ///                freshly built via distribution_initialize() or freshly
    ///                recycled via distribution_reset()). It will be turned
    ///                into the output distribution returned by this function,
    ///                and therefore cannot be used after calling this function.
    /// \param num is the distribution from which data points associated with
    ///            the numerator of the ratio will be taken.
    /// \param factor is a constant factor by which every data point from `num`
    ///               will be multiplied before division by the data point from
    ///               `denom` occurs.
    /// \param denom is the distribution from which data points associated with
    ///              the denominator of the ratio will be taken.
    ///
    /// \returns an estimated distribution of `num * factor / denom` scaled
    ///          ratios
    UDIPE_NON_NULL_ARGS
    distribution_t distribution_scaled_div(distribution_builder_t* builder,
                                           const distribution_t* num,
                                           int64_t factor,
                                           const distribution_t* denom);

    /// Recycle a duration-based distribution for data recording
    ///
    /// This discards all data points from a distribution and switches it back
    /// to the \ref distribution_builder_t state where data points can be
    /// inserted into it again.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't yet been recycled via
    ///             distribution_reset() or destroyed via
    ///             distribution_finalize(). It will be turned into the output
    ///             distribution builder returned by this function, and
    ///             therefore cannot be used after calling this function.
    ///
    /// \returns an empty \ref distribution_builder_t that reuses the storage
    ///          allocation of `dist`.
    UDIPE_NON_NULL_ARGS
    distribution_builder_t distribution_reset(distribution_t* dist);

    /// Destroy a duration-based distribution
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't yet been recycled via
    ///             distribution_reset() or destroyed via
    ///             distribution_finalize(). It will be liberated by this
    ///             function, and therefore cannot be used after calling it.
    UDIPE_NON_NULL_ARGS
    void distribution_finalize(distribution_t* dist);

    /// \}


    /// \name Statistical analysis of duration-based data
    /// \{

    /// Result of the statistical analysis of a duration-based dataset
    ///
    /// This provides the most likely value and x% confidence interval of some
    /// quantity that originates from benchmark clock measurements. We use
    /// bootstrap resampling techniques to avoid assuming that duration
    /// measurements are normally distributed (which is totally false).
    ///
    /// To maximize statistical analysis code sharing between the code paths
    /// associated with different clocks and different stages of the benchmark
    /// harness setup and usage process, the quantity that is being measured
    /// (nanoseconds, clock ticks, TSC frequency...) and the width of the
    /// confidence interval are purposely left unspecified.
    typedef struct stats_s {
        /// Most likely value of the duration-based measurement
        ///
        /// This value is determined by repeatedly drawing a small amount of
        /// duration samples from the duration dataset, taking their median
        /// value (which eliminates outliers), and then taking the median of a
        /// large number of these median values (which determines the most
        /// likely small-window median duration value).
        int64_t center;

        /// Lower bound of the confidence interval
        ///
        /// This value is determined by taking the specified lower quantile of
        /// the bootstrap median timing distribution discussed above:
        ///
        /// - For 95% confidence intervals, this is the 2.5% quantile of the
        ///   median timing distribution.
        /// - For 99% confidence intervals, this is the 0.5% quantile of the
        ///   median timing distribution.
        ///
        /// Bear in mind that larger confidence intervals require more
        /// measurements and median value computations to converge reasonably
        /// close to their true statistical asymptote.
        int64_t low;

        /// Higher bound of the confidence interval
        ///
        /// This provides the higher bound of the confidence interval using the
        /// same conventions as `low`:
        ///
        /// - For 95% confidence intervals, this is the 97.5% quantile of the
        ///   median timing distribution.
        /// - For 99% confidence intervals, this is the 99.5% quantile of the
        ///   median timing distribution.
        int64_t high;
    } stats_t;

    /// Statistical analyzer for duration-based data
    ///
    /// We will typically end up analyzing many timing datasets with the same
    /// confidence interval, which means that it is beneficial to keep around
    /// the associated memory allocation and layout information.
    typedef struct stats_analyzer_s {
        int64_t* medians;  ///< Storage for median duration samples
        size_t num_medians;  ///< Number of samples within `medians`
        size_t low_idx;  ///< Confidence interval start location
        size_t center_idx;  ///< Median location
        size_t high_idx;  ///< Confidence interval end location
    } stats_analyzer_t;

    /// Set up a statistical analyzer
    ///
    /// Given a confidence interval, get ready to analyze duration-based data
    /// with this confidence interval.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param confidence is the desired width of confidence intervals in
    ///                   percentage points (i.e. between 0.0 and 100.0,
    ///                   excluding both bounds)
    stats_analyzer_t stats_analyzer_initialize(float confidence);

    /// Statistically analyze duration-based data data
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param analyzer is a statistical analyzer that has been previously set
    ///                 up via stats_analyzer_initialize() and hasn't been
    ///                 destroyed via stats_analyzer_finalize() yet
    /// \param dist is the distribution that you want to analyze
    ///
    /// \returns statistics associated with the data from `dist`
    UDIPE_NON_NULL_ARGS
    stats_t stats_analyze(stats_analyzer_t* analyzer,
                          const distribution_t* dist);

    /// Destroy a statistical analyzer
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param analyzer is a duration analyzer that has been previously set up
    ///                 via stats_analyzer_initialize() and hasn't been
    ///                 destroyed via stats_analyzer_finalize() yet
    UDIPE_NON_NULL_ARGS
    void stats_analyzer_finalize(stats_analyzer_t* analyzer);

    /// \}


    /// \name Common clock properties
    /// \{

    /// Signed version of \ref udipe_duration_ns_t
    ///
    /// Most clocks guarantee that if two timestamps t1 and t2 were taken in
    /// succession, t2 cannot be "lesser than" t1 and therefore t2 - t1 must be
    /// a positive or zero duration. But this monotonicity property is
    /// unfortunately partially lost we attempt to compute true user code
    /// durations, i.e. the time that elapsed between the end of the now() at
    /// the beginning of a benchmark workload and the start of now() at the end
    /// of a benchmark workload. There are two reasons for this:
    ///
    /// - Computing the user workload duration requires us to subtract the clock
    ///   access delay, which is not perfectly known but estimated by
    ///   statistical means (and may indeed fluctuate one some uncommon hardware
    ///   configurations). If we over-estimate the clock access delay, then
    ///   negative duration measurements may happen.
    /// - Clocks do not guarantee that a timestamp will always be acquired at
    ///   the same time between the start and the end of the call to now(), and
    ///   this introduces an uncertainty window over the position of time
    ///   windows that can be as large as the clock access delay in the worst
    ///   case (though it will usually be smaller). If we take t the true
    ///   duration and dt the clock access time, the corrected duration `t2 - t1
    ///   - dt` may therefore be anywhere within the `[t - dt; t + dt]` range.
    ///   This means that in the edge case where `t < dt`, the computed duration
    ///   may also be negative.
    ///
    /// As a consequence of this, negative durations may pop up in intermediate
    /// computations of performance benchmarks, though they should never
    /// remain around in the final output of the computation if the benchmark
    /// was carried out correctly with workload durations that far exceed the
    /// clock access delay.
    typedef int64_t signed_duration_ns_t;

    /// \}


    /// \name Operating system clock
    /// \{

    /// Raw system clock timestamp
    ///
    /// This type is OS-specific and its values should not be used directly.
    /// Instead they are meant to be read with os_now() during a benchmark,
    /// buffered for a while, then post-processed using the os_duration()
    /// function which computes duration estimates from pairs of timestamps.
    #ifdef _POSIX_TIMERS
        typedef struct timespec os_timestamp_t;
    #elif defined(_WIN32)
        typedef LONG_INTEGER os_timestamp_t;
    #else
        #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
    #endif

    /// Check if two OS timestamps are equal
    ///
    /// If two timestamps that have been measured at different times turn out to
    /// be equal, it means that the system clock access time is smaller than the
    /// clock resolution (smallest nonzero difference between two clock
    /// readouts).
    ///
    /// When this happens, clock resolution is likely to be the factor that will
    /// limit OS clock timing precision. This is not as common as it was back in
    /// the days where clocks had a milisecond or microsecond time resolution,
    /// but it may still happen if e.g. one uses the clock() C library function
    /// as the timing backend in a microbenchmarking library.
    static inline bool os_timestamp_eq(os_timestamp_t t1, os_timestamp_t t2) {
        #if defined(_POSIX_TIMERS)
            return t1.tv_sec == t2.tv_sec && t1.tv_nsec == t2.tv_nsec;
        #elif defined(_WIN32)
            return t1.QuadPart == t2.QuadPart;
        #else
            #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
        #endif
    }

    /// Check if OS timestamp `t1` is lesser than or equal to timestamp `t2`
    ///
    /// This is a common sanity check in timing code, used to ensure that the
    /// clocks used for benchmarking are monotonic i.e. their timestamps never
    /// go back in time and can only go up (though they may remain constant).
    static inline bool os_timestamp_le(os_timestamp_t t1, os_timestamp_t t2) {
        #if defined(_POSIX_TIMERS)
            return t1.tv_sec < t2.tv_sec
                   || (t1.tv_nsec == t2.tv_sec && t1.tv_nsec <= t2.tv_nsec);
        #elif defined(_WIN32)
            return t1.QuadPart <= t2.QuadPart;
        #else
            #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
        #endif
    }

    /// Operating system clock
    ///
    /// This contains a cache of anything needed to (re)calibrate the operating
    /// system clock and use it for duration measurements.
    typedef struct os_clock_s {
        #ifdef _WIN32
            /// Frequency of the Win32 performance counter in ticks/second
            ///
            /// This is just the cached output of
            /// QueryPerformanceFrequency() in 64-bit form.
            ///
            /// To convert performance counter ticks to nanoseconds, multiply
            /// the number of ticks by \ref UDIPE_SECOND then divide it by this
            /// number.
            uint64_t win32_frequency;
        #endif

        /// Clock offset distribution in nanoseconds
        ///
        /// This is the offset that must be subtracted from OS clock durations
        /// in order to get an unbiased estimator of the duration of the code
        /// that is being benchmarked, excluding the cost of os_now() itself.
        ///
        /// You do not need to perform this offset subtraction yourself,
        /// os_duration() and os_clock_measure() will take care of it for you.
        distribution_t offsets;

        /// Empty loop iteration count at which the best relative precision on
        /// the loop iteration duration is achieved
        ///
        /// This is a useful starting point when recalibrating the system clock,
        /// or when calibrating a different clock based on the system clock.
        size_t best_empty_iters;

        /// Empty loop duration distribution in nanoseconds
        ///
        /// This field contains the distribution of execution times for the best
        /// empty loop (as defined above). It can be used to calibrate the tick
        /// rate of another clock like the x86 TSC clock by making said other
        /// clock measure the same loop immediately afterwards then computing
        /// the tick rate as a ticks-to-seconds ratio.
        distribution_t best_empty_durations;

        /// Duration statistics for `best_empty_dist`
        ///
        /// This is used when calibrating the duration of a benchmark run
        /// towards the region where the system clock is most precise.
        stats_t best_empty_stats;

        /// Unused \ref distribution_builder_t
        ///
        /// The clock calibration process uses one more \ref
        /// distribution_builder_t than is required by the calibrated clock at
        /// the end therefore this \ref distribution_builder_t remains around,
        /// and can be reused to momentarily store user durations during the
        /// benchmarking process as long as it is reset in the end.
        distribution_builder_t builder;

        /// Timestamp buffer
        ///
        /// This is used for timestamp storage during OS clock measurements. It
        /// contains enough storage for `num_durations + 1` timestamps.
        os_timestamp_t* timestamps;

        /// Duration buffer capacity
        ///
        /// See individual buffer descriptions for more information about how
        /// buffer capacities derive from this quantity.
        size_t num_durations;
    } os_clock_t;

    /// Set up the system clock
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param analyzer should have been initialized with
    ///                 stats_analyzer_initialize() and not have been finalized
    ///                 yet
    ///
    /// \returns a system clock context that must later be finalized using
    ///          os_clock_finalize()
    UDIPE_NON_NULL_ARGS
    os_clock_t os_clock_initialize(stats_analyzer_t* calibration_analyzer);

    /// Read the system clock
    ///
    /// The output of this function is OS-specific and unrelated to any time
    /// base you may be familiar with like UTC or local time. To minimize
    /// measurement condition drift, you should only buffer these timestamps
    /// during the measurement cycle, then post-process pairs of them into
    /// duration estimates using os_duration().
    ///
    /// \returns a timestamp representing the current time at some point between
    ///          the moment where os_now() was called and the moment where the
    ///          call to os_now() returned.
    static inline os_timestamp_t os_now() {
        os_timestamp_t timestamp;
        #if defined(_POSIX_TIMERS)
            clockid_t clock;
            #ifdef __linux__
                clock = CLOCK_MONOTONIC_RAW;
            #elif defined(_POSIX_MONOTONIC_CLOCK)
                clock = CLOCK_MONOTONIC;
            #else
                clock = CLOCK_REALTIME;
            #endif
            int result = clock_gettime(clock, &timestamp);
            assert(result == 0);
        #elif defined(_WIN32)
            bool result = QueryPerformanceCounter(&timestamp);
            assert(result);
        #else
            #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
        #endif
        return timestamp;
    }

    /// Estimate the elapsed time between two system clock readouts
    ///
    /// Given the `start` and `end` timestamps returned by two calls to now(),
    /// where `start` was measured before `end`, this estimates the amount of
    /// time that elapsed between the end of the call to os_now() that returned
    /// `start` and the beginning of the call to os_now() that returned `end`.
    ///
    /// \param clock is a set of clock parameters that were previously measured
    ///              via os_clock_initialize() and haven't been finalized yet.
    /// \param start is the timestamp that was measured using os_now() at the
    ///              start of the time span of interest.
    /// \param end is the timestamp that was measured using os_now() at the end
    ///            of the time span of interest (and therefore after `start`).
    ///
    /// \returns an offset-corrected estimate of the amount of time that elapsed
    ///          between `start` and `end`, in nanoseconds.
    UDIPE_NON_NULL_ARGS
    static inline signed_duration_ns_t os_duration(os_clock_t* clock,
                                                   os_timestamp_t start,
                                                   os_timestamp_t end) {
        assert(os_timestamp_le(start, end));
        signed_duration_ns_t uncorrected_ns;
        #if defined(_POSIX_TIMERS)
            const int64_t secs = (int64_t)end.tv_sec - (int64_t)start.tv_sec;
            uncorrected_ns = secs * UDIPE_SECOND;
            uncorrected_ns += (int64_t)end.tv_nsec - (int64_t)start.tv_nsec;
        #elif defined(_WIN32)
            assert(clock->win32_frequency > 0);
            uncorrected_ns = (end.QuadPart - start.QuadPart) * UDIPE_SECOND / clock->win32_frequency;
        #else
            #error "Sorry, we don't support your operating system yet. Please file a bug report about it!"
        #endif
        return uncorrected_ns - distribution_sample(&clock->offsets);
    }

    /// Measure the execution duration of `workload` using the OS clock
    ///
    /// This call `workload` repeatedly `num_runs` times with timing calls
    /// interleaved between each call. Usual micro-benchmarking precautions must
    /// be taken to avoid compiler over-optimization:
    ///
    /// - If `workload` always processes the same inputs, then
    ///   UDIPE_ASSUME_ACCESSED() should be used to make the compiler assume
    ///   that these inputs change from one execution to another.
    /// - If `workload` emits outputs, then UDIPE_ASSUME_READ() should be used
    ///   to make the compiler assume that these outputs are being used.
    /// - If `workload` is just an artificial empty loop (as used during
    ///   calibration), then UDIPE_ASSUME_READ() should be used on the loop
    ///   counter to preserve the number of loop iterations.
    ///
    /// `num_runs` controls how many timed calls to `workload` will occur, it
    /// should be tuned such that...
    ///
    /// - Results are reproducible enough across benchmark executions (what
    ///   constitutes "reproducible enough" is context dependent, a parameter
    ///   autotuning loop can typically work with less steady timing data than
    ///   the final benchmark measurement).
    /// - Execution time, which grows roughly linearly with `num_runs`,
    ///   remains reasonable.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param clock is the benchmark clock that is going to be used. This
    ///              routine can be used before said clock is fully initialized,
    ///              but it must be at minimum initialized enough to allow for
    ///              offset-biased OS clock measurements (i.e. on Windows
    ///              `win32_frequency` must have been queried already, and on
    ///              all OSes `offset` must be zeroed out if not known yet).
    /// \param workload is the workload whose duration should be measured. For
    ///                 minimal overhead/bias, its definition should be visible
    ///                 to the compiler at the point where this function is
    ///                 called, so that it can be inlined into it.
    /// \param context encodes the parameters that should be passed to
    ///                `workload`, if any.
    /// \param warmup indicates how long the code should be continuously
    ///               executed before duration measurements are taken, giving
    ///               the CPU some time to reach a steady performance state.
    /// \param num_runs indicates how many timed calls to `workload` should
    ///                 be performed. It must be strictly greater than zero, see
    ///                 above for tuning advice.
    /// \param builder is a distribution builder within which output data will
    ///                be inserted, which should initially be empty (either
    ///                freshly built via distribution_initialize() or freshly
    ///                recycled via distribution_reset()). It will be turned
    ///                into the output distribution returned by this function,
    ///                and therefore cannot be used after calling this function.
    /// \param analyzer is a statistical analyzer that has been previously set
    ///                 up via stats_analyzer_initialize() and hasn't been
    ///                 destroyed via stats_analyzer_finalize() yet.
    ///
    /// \returns the distribution of measured execution times in nanoseconds
    ///
    /// \internal
    ///
    /// This function is marked as `static inline` to encourage the compiler to
    /// make one copy of it per `workload` and inline `workload` into it,
    /// assuming the caller did their homework on their side by exposing the
    /// definition of `workload` at the point where os_clock_measure() is called.
    UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 6, 7)
    static inline distribution_t os_clock_measure(
        os_clock_t* clock,
        void (*workload)(void*),
        void* context,
        udipe_duration_ns_t warmup,
        size_t num_runs,
        distribution_builder_t* result_builder,
        stats_analyzer_t* analyzer
    ) {
        assert(num_runs > 0);
        if (num_runs > clock->num_durations) {
            trace("Reallocating storage from %zu to %zu durations...");
            realtime_liberate(
                clock->timestamps,
                (clock->num_durations+1) * sizeof(os_timestamp_t)
            );
            clock->num_durations = num_runs;
            clock->timestamps =
                realtime_allocate((num_runs+1) * sizeof(os_timestamp_t));
        }

        trace("Warming up...");
        os_timestamp_t* timestamps = clock->timestamps;
        udipe_duration_ns_t elapsed = 0;
        os_timestamp_t start = os_now();
        do {
            workload(context);
            os_timestamp_t now = os_now();
            elapsed = os_duration(clock, start, now);
        } while(elapsed < warmup);

        tracef("Performing %zu timed runs...", num_runs);
        timestamps[0] = os_now();
        for (size_t run = 0; run < num_runs; ++run) {
            UDIPE_ASSUME_READ(timestamps);
            workload(context);
            timestamps[run+1] = os_now();
        }
        UDIPE_ASSUME_READ(timestamps);

        // TODO: Deduplicate wrt x86_run_measure
        trace("Seeding duration outlier filter...");
        int64_t initial_window[TEMPORAL_WINDOW];
        for (size_t run = 0; run < TEMPORAL_WINDOW; ++run) {
            initial_window[run] = os_duration(clock,
                                              timestamps[run],
                                              timestamps[run+1]);
        }
        temporal_filter_t filter = temporal_filter_initialize(initial_window);

        trace("Building duration distribution...");
        size_t num_normal_runs = 0;
        size_t num_initially_rejected = 0;
        size_t num_reclassified = 0;
        distribution_builder_t reject_builder;
        distribution_builder_t reclassify_builder;
        if (log_enabled(UDIPE_DEBUG)) {
            reject_builder = distribution_initialize();
            reclassify_builder = distribution_initialize();
        }
        TEMPORAL_FILTER_FOREACH_NORMAL(&filter, duration, {
            distribution_insert(result_builder, duration);
            ++num_normal_runs;
        });
        // There can be at most one outlier per input window
        ensure_le(TEMPORAL_WINDOW - num_normal_runs, (uint16_t)1);
        for (size_t run = TEMPORAL_WINDOW; run < num_runs; ++run) {
            const int64_t duration = os_duration(clock,
                                                 timestamps[run],
                                                 timestamps[run+1]);
            const temporal_filter_result_t result =
                temporal_filter_apply(&filter, duration);
            if (result.previous_not_outlier) {
                tracef("- Reclassified previous outlier duration %zd as non-outlier",
                       result.previous_input);
                for (size_t pos = 0; pos < TEMPORAL_WINDOW; ++pos) {
                    const size_t idx = (filter.next_idx + pos) % TEMPORAL_WINDOW;
                    const size_t age = TEMPORAL_WINDOW - 1 - pos;
                    tracef("  * duration[%zu aka -%zu] is %zd",
                           run - age,
                           age,
                           filter.window[idx]);
                }
                distribution_insert(result_builder, result.previous_input);
                ++num_normal_runs;
                if (log_enabled(UDIPE_DEBUG)) {
                    distribution_insert(&reclassify_builder, result.previous_input);
                    ++num_reclassified;
                }
            }
            if (!result.current_is_outlier) {
                distribution_insert(result_builder, duration);
                ++num_normal_runs;
            } else if (log_enabled(UDIPE_DEBUG)) {
                distribution_insert(&reject_builder, duration);
                ++num_initially_rejected;
            }
            ensure_le(num_normal_runs, run + 1);
            ensure_le(num_reclassified, num_initially_rejected);
        }
        if (log_enabled(UDIPE_DEBUG)) {
            if (num_initially_rejected > 0) {
                distribution_log(&reject_builder,
                                 UDIPE_DEBUG,
                                 "Durations initially rejected as outliers");
            }
            distribution_discard(&reject_builder);
            if (num_reclassified > 0) {
                distribution_log(&reclassify_builder,
                                 UDIPE_DEBUG,
                                 "Durations later reclassified to non-outlier");
                debugf("Reclassified %zu/%zu durations from outlier to normal.",
                       num_reclassified, num_runs);
            }
            distribution_discard(&reclassify_builder);
            if (num_normal_runs < num_runs) {
                const size_t num_outliers = num_runs - num_normal_runs;
                debugf("Eventually rejected %zu/%zu durations.",
                       num_outliers, num_runs);
            }
            distribution_log(result_builder,
                             UDIPE_DEBUG,
                             "Accepted durations");
        }
        distribution_t result = distribution_build(result_builder);
        assert(distribution_len(&result) == num_runs);
        return result;
    }

    /// Destroy the system clock
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param clock is a system clock that has been previously set up
    ///              via os_clock_initialize() and hasn't been destroyed via
    ///              os_clock_finalize() yet
    UDIPE_NON_NULL_ARGS
    void os_clock_finalize(os_clock_t* clock);

    /// \}


    #ifdef X86_64
        /// \name TSC clock (x86-specific for now)
        /// \{

        /// x86 TSC clock context
        ///
        /// This contains a cache of anything needed to (re)calibrate the x86
        /// TimeStamp Counter and use it for duration measurements.
        typedef struct x86_clock_s {
            /// Clock offset distribution in TSC ticks
            ///
            /// This is the offset that must be subtracted from TSC timestamp
            /// differences in order to get an unbiased estimator of the
            /// duration of the code that is being benchmarked, excluding the
            /// cost of x86_timer_start()/x86_timer_end() itself.
            ///
            /// You do not need to perform this offset subtraction yourself,
            /// x86_clock_measure() will take care of it for you.
            distribution_t offsets;

            /// Empty loop duration statistics in TSC ticks
            ///
            /// This summarizes the execution times for the best empty loop (as
            /// defined in \ref os_clock_t). It is used when calibrating the
            /// duration of a benchmark run towards the region where the TSC
            /// clock exhibits best relative precision.
            stats_t best_empty_stats;

            /// TSC clock frequency distribution in ticks/second
            ///
            /// This is calibrated against the OS clock, enabling us to turn
            /// RDTSC readings into nanoseconds in the same way that
            /// `win32_frequency` lets us turn Windows performance counter ticks
            /// into durations.
            ///
            /// Because this frequency is derived from an OS clock measurement,
            /// it is not perfectly known, as highlighted by the fact that this
            /// is a distribution and not an absolute number. This means that
            /// precision-sensitive computations should ideally be performed in
            /// terms of TSC ticks, not nanoseconds.
            distribution_t frequencies;

            /// Timestamp buffer
            ///
            /// This is used for timestamp storage during TSC measurements. It
            /// contains enough storage for `2*num_durations` timestamps.
            ///
            /// In terms of layout, it begins with all the `num_durations` start
            /// timestamps, followed by all the `num_durations` end timestamps,
            /// which ensures optimal SIMD processing.
            ///
            /// Because the timing thread is pinned to a single CPU core, we do
            /// not need to keep the CPU IDs around, only to check that the
            /// pinning is effective at keeping these constant in debug builds.
            /// Therefore we extract the instant values from the timestamps and
            /// only keep that around.
            x86_instant* instants;

            /// Duration buffer capacity
            ///
            /// See individual buffer descriptions for more information about
            /// how buffer capacities derive from this quantity.
            size_t num_durations;
        } x86_clock_t;

        /// Set up the TSC clock
        ///
        /// The TSC is calibrated against the OS clock, which must therefore be
        /// calibrated first before the TSC can be calibrated.
        ///
        /// TSC calibration should ideally happen immediately after system clock
        /// setup, so that \ref os_clock_t::best_empty_stats is maximally up to
        /// date (e.g. CPU clock frequency did not have any time to drift to a
        /// different value).
        ///
        /// This function must be called within the scope of with_logger().
        ///
        /// \param os is a system clock context that was freshly initialized
        ///           with os_clock_initialize(), ideally right before calling
        ///           this function, and hasn't been used for any other purpose
        ///           or finalized with os_clock_finalize() yet.
        /// \param analyzer should have been initialized with
        ///                 stats_analyzer_initialize() and not have been
        ///                 finalized yet
        ///
        /// \returns a TSC clock context that must later be finalized using
        ///          x86_clock_finalize().
        UDIPE_NON_NULL_ARGS
        x86_clock_t
        x86_clock_initialize(os_clock_t* os,
                             stats_analyzer_t* analyzer);

        /// Measure the execution duration of `workload` using the TSC clock
        ///
        /// This works a lot like os_clock_measure(), but it uses the TSC clock
        /// instead of the system clock, which changes a few things:
        ///
        /// - The timing thread that calls this function must have been pinned
        ///   to a specific CPU core to avoid CPU migrations. This is implicitly
        ///   taken care of by udipe_benchmark_initialize() before calling
        ///   benchmark_clock_initialize() and also by udipe_benchmark_run()
        ///   before calling the user-provided benchmarking routine.
        /// - Output measurements are provided in clock ticks not nanoseconds.
        ///   To convert them into nanoseconds, you must use
        ///   clock->frequency_stats, taking care to widen the output confidence
        ///   interval based on the associated TSC frequency uncertainty. The
        ///   x86_duration() function can be used to perform this conversion.
        ///
        /// This function must be called within the scope of with_logger().
        ///
        /// \param clock mostly works as in os_clock_measure(), except it wants
        ///              a TSC clock context not an OS clock context
        /// \param workload works as in os_clock_measure()
        /// \param context works as in os_clock_measure()
        /// \param warmup works as in os_clock_measure()
        /// \param num_runs works as in os_clock_measure()
        /// \param result_builder works as in os_clock_measure()
        /// \param analyzer works as in os_clock_measure()
        ///
        /// \returns the distribution of measured execution times in TSC ticks
        ///
        /// \internal
        ///
        /// This function is `static inline` for the same reason that
        /// os_clock_measure() is `static inline`.
        UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 6, 7)
        static inline distribution_t x86_clock_measure(
            x86_clock_t* xclock,
            void (*workload)(void*),
            void* context,
            udipe_duration_ns_t warmup,
            size_t num_runs,
            distribution_builder_t* result_builder,
            stats_analyzer_t* analyzer
        ) {
            assert(num_runs > 0);
            if (num_runs > xclock->num_durations) {
                trace("Reallocating storage from %zu to %zu durations...");
                realtime_liberate(
                    xclock->instants,
                    2 * xclock->num_durations * sizeof(x86_instant)
                );
                xclock->num_durations = num_runs;
                xclock->instants =
                    realtime_allocate(2 * num_runs * sizeof(x86_instant));
            }

            trace("Setting up measurement...");
            x86_instant* starts = xclock->instants;
            x86_instant* ends = xclock->instants + num_runs;
            const bool strict = false;
            x86_timestamp_t timestamp = x86_timer_start(strict);
            const x86_cpu_id initial_cpu_id = timestamp.cpu_id;

            trace("Warming up...");
            udipe_duration_ns_t elapsed = 0;
            clock_t start = clock();
            do {
                timestamp = x86_timer_start(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                UDIPE_ASSUME_READ(timestamp.ticks);

                workload(context);

                timestamp = x86_timer_end(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                UDIPE_ASSUME_READ(timestamp.ticks);

                clock_t now = clock();
                elapsed = (udipe_duration_ns_t)(now - start)
                          * UDIPE_SECOND / CLOCKS_PER_SEC;
            } while(elapsed < warmup);

            tracef("Performing %zu timed runs...", num_runs);
            for (size_t run = 0; run < num_runs; ++run) {
                timestamp = x86_timer_start(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                starts[run] = timestamp.ticks;
                UDIPE_ASSUME_READ(starts);

                workload(context);

                timestamp = x86_timer_end(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                ends[run] = timestamp.ticks;
                UDIPE_ASSUME_READ(ends);
            }

            // TODO deduplicate wrt os_clock_measure()
            trace("Seeding duration outlier filter...");
            int64_t initial_window[TEMPORAL_WINDOW];
            for (size_t run = 0; run < TEMPORAL_WINDOW; ++run) {
                int64_t ticks = ends[run] - starts[run];
                ticks -= distribution_sample(&xclock->offsets);
                initial_window[run] = ticks;
            }
            temporal_filter_t filter = temporal_filter_initialize(initial_window);

            trace("Building duration distribution...");
            size_t num_normal_runs = 0;
            TEMPORAL_FILTER_FOREACH_NORMAL(&filter, ticks, {
                distribution_insert(result_builder, ticks);
                ++num_normal_runs;
            });
            // There can be at most one outlier per input window
            ensure_le(TEMPORAL_WINDOW - num_normal_runs, (uint16_t)1);
            for (size_t run = TEMPORAL_WINDOW; run < num_runs; ++run) {
                int64_t ticks = ends[run] - starts[run];
                ticks -= distribution_sample(&xclock->offsets);
                const temporal_filter_result_t result =
                    temporal_filter_apply(&filter, ticks);
                if (result.previous_not_outlier) {
                    tracef("- Reclassified previous outlier duration %zd as non-outlier",
                           result.previous_input);
                    for (size_t pos = 0; pos < TEMPORAL_WINDOW; ++pos) {
                        const size_t idx = (filter.next_idx + pos) % TEMPORAL_WINDOW;
                        const size_t age = TEMPORAL_WINDOW - 1 - pos;
                        tracef("  * duration[%zu aka -%zu] is %zd",
                               run - age,
                               age,
                               filter.window[idx]);
                    }
                    distribution_insert(result_builder, result.previous_input);
                    ++num_normal_runs;
                }
                if (!result.current_is_outlier) {
                    distribution_insert(result_builder, ticks);
                    ++num_normal_runs;
                }
                ensure_le(num_normal_runs, run + 1);
            }
            if (num_normal_runs < num_runs) {
                const size_t num_outliers = num_runs - num_normal_runs;
                debugf("Rejected %zu/%zu durations as high outliers",
                       num_outliers, num_runs);
            }
            distribution_t result = distribution_build(result_builder);
            assert(distribution_len(&result) == num_runs);
            return result;
        }

        /// Estimate real time duration statistics from a TSC clock ticks
        /// distribution
        ///
        /// \param clock must be a TSC clock context that was initialized
        ///              with x86_clock_initialize() and hasn't been finalized
        ///              with x86_clock_finalize() yet
        /// \param tmp_builder is a distribution builder within which duration
        ///                    data will be temporarily stored. It should
        ///                    initially be empty (either freshly built via
        ///                    distribution_initialize() or freshly recycled via
        ///                    distribution_reset()). The resulting distribution
        ///                    is only used temporarily for the purpose of
        ///                    computing statistics, and therefore the builder
        ///                    will be restituted to the caller upon return.
        /// \param ticks is the distribution of TSC clock ticks from which
        ///              durations will be estimated.
        /// \param analyzer is the statistical analyzer that will be applied to
        ///                 the output TSC clock ticks, encoding the desired
        ///                 width of output confidence intervals.
        ///
        /// \returns estimated statistics over the timing distribution that
        ///          `ticks` corresponds to, in nanoseconds, with a confidence
        ///          interval given by `analyzer`.
        UDIPE_NON_NULL_ARGS
        stats_t x86_duration(x86_clock_t* clock,
                             distribution_builder_t* tmp_builder,
                             const distribution_t* ticks,
                             stats_analyzer_t* analyzer);

        /// Destroy the TSC clock
        ///
        /// This function must be called within the scope of with_logger().
        ///
        /// \param clock is a TSC clock context that has been previously set up
        ///              via x86_clock_initialize() and hasn't been destroyed
        ///              via x86_clock_finalize() yet
        UDIPE_NON_NULL_ARGS
        void x86_clock_finalize(x86_clock_t* clock);

        /// \}
    #endif  // X86_64


    /// \name Benchmark clock
    /// \{

    /// Benchmark clock
    ///
    /// This is a unified interface to the operating system and CPU clocks,
    /// which attempts to pick the best clock available on the target operating
    /// system and CPU architecture.
    typedef struct benchmark_clock_s {
        #ifdef X86_64
            /// TSC clock context
            ///
            /// This contains everything needed to recalibrate and use the x86
            /// TimeStamp Counter clock.
            x86_clock_t x86;
        #endif

        /// Statistical analyzer for benchmark measurements
        ///
        /// This represents a confidence interval of CONFIDENCE.
        stats_analyzer_t analyzer;

        /// System clock context
        ///
        /// This contains everything needed to recalibrate and use the operating
        /// system clock.
        os_clock_t os;
    } benchmark_clock_t;

    /// Set up the benchmark clock
    ///
    /// Since operating systems do not expose many useful properties of their
    /// high-resolution clocks, these properties must unfortunately be manually
    /// calibrated by applications through a set of measurements, which will
    /// take some time.
    ///
    /// Furthermore, some aspects of this initial calibration may not remain
    /// correct forever, as system operation conditions can change during
    /// long-running benchmarks. It is therefore strongly recommended to call
    /// benchmark_clock_recalibrate() between two sets of measurements, so that
    /// the benchmark clock gets automatically recalibrated whenever necessary.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \returns a benchmark clock configuration that is meant to be integrated
    ///          into \ref udipe_benchmark_t, and eventually destroyed with
    ///          benchmark_clock_finalize().
    benchmark_clock_t benchmark_clock_initialize();

    /// Check if the benchmark clock needs recalibration, if so recalibrate it
    ///
    /// This recalibration process mainly concerns the `best_empty_stats` of
    /// each clock, which may evolve as the system background workload changes.
    /// But it is also a good occasion to sanity-check that other clock
    /// parameters still seem valid.
    ///
    /// It should be run at the time where execution shifts from one benchmark
    /// workload to another, as performing statistics over measurements which
    /// were using different clock calibrations is fraught with peril.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param clock must be a benchmark clock configuration that was
    ///              initialized with benchmark_clock_initialize() and hasn't
    ///             been destroyed with benchmark_clock_finalize() yet.
    UDIPE_NON_NULL_ARGS
    void benchmark_clock_recalibrate(benchmark_clock_t* clock);

    /// Destroy the benchmark clock
    ///
    /// After this is done, the benchmark clock must not be used again.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param clock must be a benchmark clock configuration that was
    ///              initialized with benchmark_clock_initialize() and hasn't
    ///             been destroyed with benchmark_clock_finalize() yet.
    UDIPE_NON_NULL_ARGS
    void benchmark_clock_finalize(benchmark_clock_t* clock);

    /// \}


    /// \copydoc udipe_benchmark_t
    struct udipe_benchmark_s {
        /// Harness logger
        ///
        /// The benchmark harness implementation will use this logger to explain
        /// what it's doing. However, measurements are a benchmark binary's
        /// primary output. They should therefore be emitted over stdout or as
        /// structured data for programmatic manipulation, not as logs.
        logger_t logger;

        /// Benchmark name filter
        ///
        /// Used by udipe_benchmark_run() to decide which benchmarks should run.
        name_filter_t filter;

        /// hwloc topology
        ///
        /// Used to pin timing measurement routines on a single CPU core so that
        /// TSC timing works reliably.
        hwloc_topology_t topology;

        /// Timing thread cpuset
        ///
        /// Probed at benchmark harness initialization time and used to ensure
        /// that timing measurement routines remain pinned to the same CPU core
        /// from then on.
        hwloc_cpuset_t timing_cpuset;

        /// Benchmark clock
        ///
        /// Used in the adjustment of benchmark parameters and interpretation of
        /// benchmark results.
        benchmark_clock_t clock;
    };


    #ifdef UDIPE_BUILD_TESTS
        /// Unit tests
        ///
        /// This function runs all the unit tests for this module. It must be called
        /// within the scope of with_logger().
        void benchmark_unit_tests();
    #endif

#endif  // UDIPE_BUILD_BENCHMARKS