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
    //! unimodal distribution does not have a dispersion figure of merit that's
    //! easy to compute.
    //!
    //! When visualizing the distribution of raw timing data, even when
    //! considering multi-modal laws, outliers have two important
    //! characteristics:
    //!
    //! - When measuring very short durations that fluctuate by an amount
    //!   smaller than the timer resolution, identical durations tend to pile
    //!   up, whereas non-identical durations do not do so.
    //! - Non-outlier durations are further away from normal values and each
    //!   other than normal values are from each other.
    //!
    //! By giving each distribution bin a weight that is sensitive to these two
    //! parameters of value count and neighbor proximity, we can get a metric
    //! that is sensitive to the density of data points. The neighbor weighting
    //! logic is similar to that of a kernel density estimator in statistics
    //! (using a power decay law as a kernel and the shortest inter-bin distance
    //! as a decay distance), therefore we call the resulting outlier filter a
    //! density filter.

    #include <udipe/pointer.h>

    #include "distribution.h"

    #include "../error.h"

    #include <math.h>
    #include <stddef.h>
    #include <stdint.h>


    /// \name Type definitions
    /// \{

    /// Recyclable distribution from a \ref density_filter_t
    ///
    /// This union starts in the `empty_builder` state. As a result of applying
    /// the host filter to a user dataset, this union may transition to the
    /// `distribution` state. It will transition back to the `empty_builder`
    /// state transiently during density_filter_apply() calls.
    typedef struct recyclable_distribution_s {
        union {
            /// Distribution builder that is guaranteed not to contain any data
            /// and can be used to store a \ref density_filter_t output
            distribution_builder_t empty_builder;

            /// Distribution that describes some aspect of the latest `target`
            /// that the surrounding \ref density_filter_t has been applied to.
            distribution_t distribution;
        };

        /// Truth that the above union is in the `distribution` state
        bool is_built;
    } recyclable_distribution_t;

    /// Density filter for \ref distribution_t values
    ///
    /// This filter classifies values from \ref distribution_builder_t as
    /// outliers or non-outliers using a density-based criterion.
    typedef struct density_filter_s {
        /// Relative weight of each bin from the last `target`
        ///
        /// This allocation contains enough storage for `bin_capacity` bins.
        /// When the density filter is applied to a new `target`...
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
        /// If this density filter is attached to a distribution with more bins,
        /// then `bin_weights` must be reallocated accordingly.
        size_t bin_capacity;

        /// Distribution of density scores from the last `target`, if any,
        /// before the filter was applied
        ///
        /// The density score is a fixed-point approximation of the base-2
        /// logarithm of the `bin_weights`.
        ///
        /// To be more specific, it is said base-2 logarithm scaled by an
        /// internal `LOG2_SCALE` factor to improve mantissa resolution at the
        /// expense of exponent range and value readability, then saturated to a
        /// `INT64_MIN` to allow double-to-int64_t conversion.
        ///
        /// This member contains is the distribution of this score for each
        /// value (not each bin, although the computation is obviously bin-based
        /// for efficiency) that `target` used to contain before the density
        /// filter was applied to it.
        recyclable_distribution_t last_scores;

        /// Rejected values from the last `target`, if any
        ///
        /// This is the distribution of the values that were removed from the
        /// last `target` that this filter has been applied to. If no value was
        /// removed, this distribution remains in the empty builder state (i.e.
        /// `is_built` is false).
        recyclable_distribution_t last_rejections;
    } density_filter_t;


    /// \name Public API
    /// \{

    // TODO docs
    density_filter_t density_filter_initialize();

    // TODO docs, target must not be empty
    UDIPE_NON_NULL_ARGS
    void density_filter_apply(density_filter_t* filter,
                              distribution_builder_t* target);

    // TODO docs
    // TODO output pointer is only valid until finalize() and should not
    //      be manipulated by another thread concurrently with an apply() call
    UDIPE_NON_NULL_ARGS
    UDIPE_NON_NULL_RESULT
    static inline
    const distribution_t*
    density_filter_last_scores(const density_filter_t* filter) {
        ensure(filter->last_scores.is_built);
        return &filter->last_scores.distribution;
    }

    // TODO docs
    // TODO output pointer is only valid until finalize() and should not
    //      be manipulated by another thread concurrently with an apply() call
    // TODO output pointer may be NULL if no value was rejected
    UDIPE_NON_NULL_ARGS
    static inline
    const distribution_t*
    density_filter_last_rejections(const density_filter_t* filter) {
        if (filter->last_rejections.is_built) {
            return &filter->last_rejections.distribution;
        } else {
            return NULL;
        }
    }

    // TODO docs + implementation
    UDIPE_NON_NULL_ARGS
    void density_filter_finalize(density_filter_t* filter);

    /// \}


    /// \name Implementation details
    /// \{

    /// Fill the `bin_weights` with data from `input`
    ///
    /// This function must be called within the scope of with_logger().
    //
    // TODO finish docs, target must not be empty
    UDIPE_NON_NULL_ARGS
    void compute_rel_weights(density_filter_t* filter,
                             const distribution_builder_t* target);

    /// Scaling factor to apply to the log2 of relative densities before
    /// truncating them to integers to produce a score
    ///
    /// Larger values improve the precision of internal computations at the
    /// expense of reducing exponent range and making displays less readable.
    #define LOG2_SCALE ((double)1000.0)

    /// Convert a relative weight to an integral score
    // TODO finish docs
    static inline int64_t rel_weight_to_score(double rel_weight) {
        assert(rel_weight >= 0.0 && rel_weight <= 1.0);
        const double unbounded_score = round(LOG2_SCALE*log2(rel_weight));
        assert(unbounded_score <= 0);
        // The conversion from INT64_MIN to double is lossless because INT64_MIN
        // is -2^63 and this power of two is losslessly convertible to double.
        return (int64_t)fmax(unbounded_score, (double)INT64_MIN);
    }

    /// Convert an integral score back to a relative weight
    // TODO finish docs
    static inline double score_to_rel_weight(int64_t score) {
        assert(score <= 0);
        const double rel_weight = exp2((double)score / LOG2_SCALE);
        assert(rel_weight >= 0.0 && rel_weight <= 1.0);
        return rel_weight;
    }

    /// Fill the `last_scores` with data from `target` and `bin_weights`
    ///
    /// This function must be called after compute_rel_weights() has been called
    /// on the same `target`.
    ///
    /// It must also be called within the scope of with_logger().
    //
    // TODO finish docs
    UDIPE_NON_NULL_ARGS
    void compute_scores(density_filter_t* filter,
                        const distribution_builder_t* target);

    /// Determine the relative weight cutoff of `filter` based on `last_scores`
    /// and internal configuration
    ///
    /// This function must be called after compute_scores().
    ///
    /// It must also be called within the scope of with_logger().
    //
    // TODO finish docs
    UDIPE_NON_NULL_ARGS
    double compute_weight_threshold(const density_filter_t* filter);

    /// Move bins of `target` below relative weight cutoff `threshold` to
    /// `last_rejections`, then build the associated distribution if non-empty
    ///
    /// This function must be called after compute_rel_weights() has been called
    /// on the same `target`.
    ///
    /// It must also be called within the scope of with_logger().
    //
    // TODO finish docs
    UDIPE_NON_NULL_ARGS
    void reject_bins(density_filter_t* filter,
                     distribution_builder_t* target,
                     double threshold);

    /// \}


    // TODO: Add unit tests

#endif  // UDIPE_BUILD_BENCHMARKS