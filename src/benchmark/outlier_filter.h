#ifdef UDIPE_BUILD_BENCHMARKS

    #pragma once

    //! \file
    //! \brief Density-based data point filtering
    //!
    //! Duration datasets from software performance benchmarks typically contain
    //! high outliers, which come from CPU interrupts caused by the OS scheduler
    //! and hardware notifications. Those are environment-dependent and break
    //! many statistics, so they are best eliminated. But we need some objective
    //! criterion in order to perform this elimination.
    //!
    //! Further complicating the matter, benchmark duration probability laws
    //! frequently have multiple modes, which breaks many common
    //! dispersion-based criteria for outlier detection and removal as a
    //! multi-modal distribution does not have a single easy dispersion figure
    //! of merit like standard deviation.
    //!
    //! When visualizing the distribution of raw timing data, even when
    //! considering multi-modal laws, outliers can be distinguished from normal
    //! measurements in two ways:
    //!
    //! - When measuring short durations that fluctuate by an amount smaller
    //!   than the timer resolution, identical durations tend to pile up,
    //!   whereas outlier durations tend to have a dispersion greater than the
    //!   timer resolution and thus have a much smaller tendency to do so.
    //! - Outlier durations are further away from normal durations and each
    //!   other than normal durations are from each other.
    //!
    //! By giving each distribution bin a weight that is sensitive to these two
    //! parameters of value count and neighbor proximity, we can get a metric
    //! that is sensitive to the density of data points, which can be used to
    //! separate low-density outliers from high-density normal measurements.

    #include <udipe/pointer.h>

    #include "distribution.h"

    #include "../error.h"

    #include <math.h>
    #include <stddef.h>
    #include <stdint.h>


    /// \name Type definitions
    /// \{

    /// Recyclable distribution from a \ref outlier_filter_t
    ///
    /// This union starts in the `empty_builder` state. As a result of applying
    /// the host filter to a user dataset, this union may transition to the
    /// `distribution` state. It will transition back to the `empty_builder`
    /// state during the processing of outlier_filter_apply() calls.
    typedef struct recyclable_distribution_s {
        union {
            /// Distribution builder that is guaranteed not to contain any data
            /// and can be used to store a \ref outlier_filter_t output
            distribution_builder_t empty_builder;

            /// Distribution that describes some aspect of the latest `target`
            /// that the surrounding \ref outlier_filter_t has been applied to.
            distribution_t distribution;
        };

        /// Truth that the above union is in the `distribution` state
        bool is_built;
    } recyclable_distribution_t;

    /// Outlier filter for \ref distribution_builder_t
    ///
    /// This filter classifies values from \ref distribution_builder_t as
    /// outlier or normal using a density-based criterion.
    typedef struct outlier_filter_s {
        /// Relative weight of each bin from the last `target`
        ///
        /// This allocation contains enough storage for `bin_capacity` bins.
        /// When the outlier filter is applied to a new `target`...
        ///
        /// - `bin_weights` is reallocated as necessary so that it has at least
        ///   as many bins as the `target`.
        /// - A first algorithmic pass fills `bin_weights` with absolute bin
        ///   weights, while tracking the maximum absolute weight seen so far.
        ///   This yields absolute weights > 0.0.
        /// - A second algorithmic pass normalizes `bin_weights` by the
        ///   previously computed largest absolute weight, yielding relative
        ///   weights between 0.0 (exclusive) and 1.0 (inclusive).
        ///
        /// It is these relative weights that are then used to build
        /// `last_scores` and eventually filter out bins of `target` according
        /// to the resulting weight distribution.
        double* bin_weights;

        /// Capacity of `bin_weights` in bins
        ///
        /// If this outlier filter is attached to a distribution with more bins,
        /// then `bin_weights` must be reallocated accordingly.
        size_t bin_capacity;

        /// Distribution of scores from the last `target`, if any, before the
        /// filter was applied
        ///
        /// The score is a fixed-point representation of the base-2 logarithm of
        /// the `bin_weights`. To be more specific, it is said base-2 logarithm
        /// scaled by an internal `LOG2_SCALE` factor to improve mantissa
        /// resolution at the expense of exponent range and value readability,
        /// then saturated to a `INT64_MIN` to allow double-to-int64_t
        /// conversion.
        ///
        /// We use this fixed-point representation because...
        ///
        /// - Integers are easier to work with and reason about than floats.
        /// - Supporting both would be a pain in C due to lack of generics.
        /// - Integers are good enough for the purpose of outlier scoring.
        ///
        /// This member contains is the distribution of this score for each
        /// value (not each bin, although the computation is obviously bin-based
        /// for efficiency) that `target` used to contain before the outlier
        /// filter was applied to it.
        recyclable_distribution_t last_scores;

        /// Rejected values from the last `target`, if any
        ///
        /// This is the distribution of the values that were removed from the
        /// last `target` that this filter has been applied to. If no value was
        /// removed, this distribution remains in the empty builder state (i.e.
        /// `is_built` is false).
        recyclable_distribution_t last_rejections;
    } outlier_filter_t;

    /// \}


    /// \name Public API
    /// \{

    /// Set up an outlier filter
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \returns an \ref outlier_filter_t that can be applied to measurements
    ///          using outlier_filter_apply().
    outlier_filter_t outlier_filter_initialize();

    /// Apply an outlier filter to measurements
    ///
    /// This function classifies the measurements from `target` (which must not
    /// be empty) into normal values and outliers. Normal values are kept, while
    /// outliers are moved to an internal rejections distribution that can later
    /// be queried via outlier_filter_last_rejections().
    ///
    /// All distribution pointers from `outlier_filter_last_` methods are
    /// invalidated by this function and must not be used during and after the
    /// call to this function. Instead, you should query the new distribution
    /// using the corresponding accessort.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param filter must be an \ref outlier_filter_t that has been initialized
    ///        with outlier_filter_initialize() and has not been destroyed with
    ///        outlier_filter_finalize() yet.
    /// \param target must be a non-empty \ref distribution_builder_t,
    ///        containing data which must be classified as outlier or normal.
    ///        Data which is classified as outliers will be removed from this
    ///        distribution.
    UDIPE_NON_NULL_ARGS
    void outlier_filter_apply(outlier_filter_t* filter,
                              distribution_builder_t* target);

    /// Distribution of value scores from the last `target` that was passed to
    /// outlier_filter_apply()
    ///
    /// Scores are an internal metric which goes from 0 for the values which are
    /// least likely to be an outlier, to negative values that grows lower as a
    /// value is more and more likely to be an outlier. The exact definition of
    /// this metric is purposely left underspecified as it may change without
    /// warning in the future. But the score distribution is nonetheless
    /// publicly exposed as it does little harm to do so and eyeballing it is
    /// very useful when fine-tuning the outlier filter.
    ///
    /// \param filter must be an \ref outlier_filter_t that has been applied to
    ///        at least one dataset via outlier_filter_apply() and has not been
    ///        destroyed with outlier_filter_finalize() since.
    ///
    /// \returns a score distribution that can be used until the next call to
    ///          outlier_filter_apply().
    UDIPE_NON_NULL_ARGS
    UDIPE_NON_NULL_RESULT
    static inline
    const distribution_t*
    outlier_filter_last_scores(const outlier_filter_t* filter) {
        ensure(filter->last_scores.is_built);
        return &filter->last_scores.distribution;
    }

    /// Distribution of values that were classified as outliers and removed from
    /// the last `target` that by outlier_filter_apply(), if any
    ///
    /// If no value from `target` was classified as an outlier, this function
    /// will return `NULL`.
    ///
    /// \param filter must be an \ref outlier_filter_t that has been applied to
    ///        at least one dataset via outlier_filter_apply() and has not been
    ///        destroyed with outlier_filter_finalize() since.
    ///
    /// \returns the distribution of rejected value, or `NULL` if no value was
    ///          rejected by the last call to outlier_filter_apply().
    UDIPE_NON_NULL_ARGS
    static inline
    const distribution_t*
    outlier_filter_last_rejections(const outlier_filter_t* filter) {
        if (filter->last_rejections.is_built) {
            return &filter->last_rejections.distribution;
        } else {
            return NULL;
        }
    }

    /// Destroy an outlier filter
    ///
    /// `filter` must not be used again after calling this function.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param filter must be an \ref outlier_filter_t that has been initialized
    ///        with outlier_filter_initialize() and has not been destroyed with
    ///        outlier_filter_finalize() yet.
    UDIPE_NON_NULL_ARGS
    void outlier_filter_finalize(outlier_filter_t* filter);

    /// \}


    /// \name Implementation details
    /// \{

    /// Fill the `bin_weights` with data from `target`
    ///
    /// This function is a part of the implementation of outlier_filter_apply(),
    /// which gives each bin from `target` a relative weight between 0.0 and 1.0
    /// depending on its value count and distance to neighboring bins.
    ///
    /// Said weights, which are stored in `filter->bin_weights`, will later be
    /// used to score bins and eventually classify them as outlier or normal.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param filter must be an \ref outlier_filter_t that has been initialized
    ///        with outlier_filter_initialize() and has not been destroyed with
    ///        outlier_filter_finalize() yet.
    /// \param target must be a non-empty \ref distribution_builder_t,
    ///        containing data which must be classified as outlier or normal.
    UDIPE_NON_NULL_ARGS
    void compute_rel_weights(outlier_filter_t* filter,
                             const distribution_builder_t* target);

    /// Scaling factor to apply to the log2 of relative densities before
    /// truncating them to integers to produce a score
    ///
    /// Larger values improve the precision of internal computations at the
    /// expense of reducing exponent range and making displays less readable.
    #define LOG2_SCALE ((double)1000.0)

    /// Convert a relative weight to an integral score
    ///
    /// \param rel_weight is the relative weight to be converted into a score,
    ///        which should be in range [0.0; 1.0].
    ///
    /// \returns the integral score associated with `rel_weight`.
    static inline int64_t rel_weight_to_score(double rel_weight) {
        assert(rel_weight >= 0.0 && rel_weight <= 1.0);
        const double unbounded_score = round(LOG2_SCALE*log2(rel_weight));
        assert(unbounded_score <= 0.0);
        // The conversion from INT64_MIN to double is lossless because INT64_MIN
        // is -2^63 and this power of two is losslessly convertible to double.
        return (int64_t)fmax(unbounded_score, (double)INT64_MIN);
    }

    /// Convert an integral score back to a relative weight
    ///
    /// \param score is the integral score to be converted into a relative
    ///        weight, which should be negative or zero.
    ///
    /// \returns the relative weight associated with `score`.
    static inline double score_to_rel_weight(int64_t score) {
        assert(score <= 0);
        const double rel_weight = exp2((double)score / LOG2_SCALE);
        assert(rel_weight >= 0.0 && rel_weight <= 1.0);
        return rel_weight;
    }

    /// Fill the `last_scores` with data from `target` and `bin_weights`
    ///
    /// This function is a part of the implementation of outlier_filter_apply(),
    /// which is meant to be called after compute_rel_weights() has been called
    /// on the same `target`.
    ///
    /// It converts the relative weights from `bin_weights` into integral
    /// scores, whose distribution is collected into `last_scores`.
    ///
    /// Said distribution can later be analyzed with compute_weight_threshold()
    /// to determine an appropriate outlier weight cutoff for the `target`
    /// distribution.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param filter must be an \ref outlier_filter_t that has freshly computed
    ///        weights for `target` via compute_rel_weights() and has not been
    ///        destroyed or applied to different data since.
    /// \param target must the same \ref distribution_builder_t that was passed
    ///        to the preceding call to compute_rel_weights(), unmodified.
    UDIPE_NON_NULL_ARGS
    void compute_scores(outlier_filter_t* filter,
                        const distribution_builder_t* target);

    /// Determine the relative weight cutoff of `filter` based on `last_scores`
    /// and internal configuration
    ///
    /// This function is a part of the implementation of outlier_filter_apply(),
    /// which is meant to be called after compute_scores().
    ///
    /// It analyzes the distribution of scores and the associated `bin_weights`
    /// to determine a bin weight cutoff that is most likely to reject outliers,
    /// without any risk of rejecting too many valid values.
    ///
    /// It must be called within the scope of with_logger().
    ///
    /// \param filter must be an \ref outlier_filter_t that has freshly computed
    ///        scores for some `target` via compute_scores() and has not been
    ///        destroyed or applied to different data since.
    UDIPE_NON_NULL_ARGS
    double compute_weight_threshold(const outlier_filter_t* filter);

    /// Move bins of `target` below relative weight cutoff `threshold` to
    /// `last_rejections`, then build the associated distribution if non-empty
    ///
    /// This function is a part of the implementation of outlier_filter_apply(),
    /// which is meant to be called after compute_rel_weights() has been
    /// called on the same `target`.
    ///
    /// It must be called within the scope of with_logger().
    ///
    /// \param filter must be an \ref outlier_filter_t that has freshly computed
    ///        weights for `target` via compute_rel_weights() and has not been
    ///        destroyed or applied to different data since.
    /// \param target must the same \ref distribution_builder_t that was passed
    ///        to the preceding call to compute_rel_weights(), unmodified.
    UDIPE_NON_NULL_ARGS
    void reject_bins(outlier_filter_t* filter,
                     distribution_builder_t* target,
                     double threshold);

    /// \}


    // TODO: Add unit tests

#endif  // UDIPE_BUILD_BENCHMARKS