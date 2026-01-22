#ifdef UDIPE_BUILD_BENCHMARKS

    #pragma once

    //! \file
    //! \brief Distribution bin density
    //!
    //! Duration datasets from software performance benchmarks typically contain
    //! high outliers, which come from CPU interrupts caused by the OS scheduler
    //! and hardware notifications. Those are environment-dependent and break
    //! many statistics, so they are best eliminated. But we need some objective
    //! criterion in order to perform this elimination.
    //!
    //! Further complicating the matter, benchmark duration probability laws
    //! frequently have multiple modes, which breaks many common
    //! dispersion-based criteria for outlier detection and removal.
    //!
    //! When visualizing the distribution of raw timing data, even when
    //! considering multi-modal laws, outliers have two important
    //! characteristics:
    //!
    //! - When we have enough data samples and a timer with a coarse enough
    //!   timing resolution that identical values start piling up, outliers
    //!   stand out as values which occur much less often than normal values.
    //! - An objective criterion for "having an unusually high value" that
    //!   should not break for multimodal distributions is that outliers tend to
    //!   be spaced apart from normal values and each other a lot more than
    //!   normal values are spaced from each other.
    //!
    //! Taking both of these into account, a possible approach for quantifying
    //! the "outlier-ness" of some distribution bin is to study the ratio of a
    //! distribution bin's value count by its nearest neighbor distance. This
    //! provides a metric, which we call the bin density, that grows higher and
    //! higher as a value becomes less and less likely to be an outlier. And if
    //! we normalize by the highest value of this metric on a particular
    //! distribution, we get a number between 0 and 1 where 1 is least likely to
    //! be an outlier and 0 is most likely to be one.
    //!
    //! This module provides utilities for computing this bin density and
    //! eliminating low-density bins.

    #include <udipe/pointer.h>

    #include "distribution.h"

    #include "../error.h"

    #include <stddef.h>
    #include <stdint.h>


    /// \name Type definitions
    /// \{

    /// Recyclable distribution from a \ref density_filter_t
    ///
    /// This union starts in the `empty_builder` state. After the filter has
    /// been applied to at least one user dataset, this union defaults to the
    /// `distribution` state and only switches back to the `empty_builder` state
    /// transiently during density_filter_apply() calls.
    typedef union recyclable_distribution_u {
        /// Distribution builder that is guaranteed not to contain any data and
        /// can be used to store a \ref density_filter_t output
        distribution_builder_t empty_builder;

        /// Distribution that describes some aspect of the latest `target` that
        /// the surrounding \ref density_filter_t has been applied to.
        distribution_t distribution;
    } recyclable_distribution_t;

    /// Density-based filter for \ref distribution_t values
    ///
    /// This filter classifies values from \ref distribution_builder_t as
    /// outliers or non-outliers using a density-based criterion.
    typedef struct density_filter_s {
        /// Relative density of each bin from the last `target`
        ///
        /// This allocation contains enough storage for `capacity` bins. When
        /// the density filter is applied to a new `target`...
        ///
        /// - `bin_density` is reallocated as necessary so that it has at least
        ///   as many bins as the `target`.
        /// - A first algorithmic pass fills `bin_density` with absolute bin
        ///   densities i.e. value count / nearest neighbor distance ratios from
        ///   each bin of `target`, while tracking the maximum absolute density
        ///   seen so far. This yields absolute density values > 0.0.
        /// - A second algorithmic pass normalizes `bin_density` by the
        ///   previously computed largest absolute density value (TODO: multiply
        ///   by inverse), yielding relative density values between 0.0
        ///   (exclusive) and 1.0 (inclusive).
        ///
        /// It is these relative density values that are then used to build
        /// `last_scores` and eventually filter out bins of `target` according
        /// to the resulting density distribution.
        double* bin_density;

        /// Capacity of `bin_density` in bins
        ///
        /// If this density filter is attached to a distribution with more bins,
        /// then `bin_density` must be reallocated accordingly.
        size_t capacity;

        /// Distribution of density scores from the last `target`, if any,
        /// before the filter was applied
        ///
        /// The density score is a fixed-point approximation of the base-2
        /// logarithm of the `bin_density`.
        ///
        /// To be more specific, it is said base-2 logarithm scaled by an
        /// internal `LOG2_SCALE` factor to improve mantissa resolution at the
        /// expense of exponent range and value readability, then saturated to a
        /// value around `INT64_MIN` to allow double-to-int64_t conversion
        /// (TODO: saturate to -0b1.1111111...2^62).
        ///
        /// This member contains is the distribution of this score for each
        /// value (not each bin, although the computation is obviously bin-based
        /// for efficiency) that `target` used to contain before the density
        /// filter was applied to it.
        recyclable_distribution_t last_scores;

        /// Rejected values from the last `target`, if any
        ///
        /// This is the distribution of the values that were removed from the
        /// last `target` that this filter has been applied to.
        recyclable_distribution_t last_rejections;

        /// Truth that this filter has been applied to at least one `target`
        ///
        /// If this is true, then each inner \ref recyclable_distribution_t is
        /// guaranteed be in the `distribution` state between two calls to
        /// user-facing methods.
        ///
        /// If this is false then these will be in the `empty_builder` state
        /// until the first call to density_filter_apply(), which will
        /// transition them to the `distribution` state and set this to true.
        bool applied;
    } density_filter_t;


    /// \name Public API
    /// \{

    // TODO docs + implementation
    density_filter_t density_filter_initialize();

    // TODO docs + implementation, beware that now target is a builder
    // TODO hardcode max rejection fraction and and desired min relative density
    // TODO: remove bins from target and put them in last_rejections after
    //       resetting it. Remember to shift remaining bins left and adjust num_bins
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
        ensure(filter->applied);
        return &filter->last_scores.distribution;
    }

    // TODO docs
    // TODO output pointer is only valid until finalize() and should not
    //      be manipulated by another thread concurrently with an apply() call
    UDIPE_NON_NULL_ARGS
    UDIPE_NON_NULL_RESULT
    static inline
    const distribution_t*
    density_filter_last_rejections(const density_filter_t* filter) {
        ensure(filter->applied);
        return &filter->last_rejections.distribution;
    }

    // TODO docs + implementation
    UDIPE_NON_NULL_ARGS
    void density_filter_finalize(density_filter_t* filter);

    /// \}


    /// \name Implementation details
    /// \{

    // TODO docs
    // TODO remove and adapt client code once new API is ready
    UDIPE_NON_NULL_ARGS
    distribution_t
    distribution_compute_log2_density(distribution_builder_t* empty_builder,
                                      const distribution_t* input);

    // TODO docs
    UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2)
    void for_each_density(const distribution_t* input,
                          void (*callback)(void* /* context */,
                                           double /* density */,
                                           size_t /* count */),
                          void* context);

    /// \}


    // TODO: Add unit tests

#endif  // UDIPE_BUILD_BENCHMARKS