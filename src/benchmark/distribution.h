#ifdef UDIPE_BUILD_BENCHMARKS

    #pragma once

    //! \file
    //! \brief Sample distribution of duration-based values
    //!
    //! This module provides a specialized data structure for handling sets of
    //! benchmark run durations and quantities which are directly derived from
    //! such durations such as differences of durations etc. It allows you to
    //! efficiently perform various statistical operations over such datasets,
    //! from random sampling to quantile computations.

    #include <udipe/pointer.h>

    #include "../error.h"
    #include "../log.h"

    #include <assert.h>
    #include <math.h>
    #include <stdalign.h>
    #include <stddef.h>
    #include <stdint.h>


    /// Sample distribution of duration-based values
    ///
    /// This encodes a set of duration-based values with a sparse histogram/CDF
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
    /// measurement...), the measurement unit of inner values is purposely left
    /// unspecified.
    ///
    /// A \ref distribution_t has a multi-stage lifecycle, which is modeled
    /// using the typestate pattern at the code level:
    ///
    /// - At first, distribution_initialize() is called, returning an empty \ref
    ///   distribution_builder_t.
    /// - Values are then added into this \ref distribution_builder_t using
    ///   distribution_insert().
    /// - Once all values have been inserted, distribution_build() is called,
    ///   turning the \ref distribution_builder_t into a `distribution_t` that
    ///   can e.g. be sampled with distribution_sample().
    /// - Once the distribution is no longer needed, it can be turned back into
    ///   an empty \ref distribution_builder_t using distribution_reset().
    ///
    /// At each of these stages, the distribution can also be liberated, using
    /// distribution_finalize() for \ref distribution_t or
    /// distribution_discard() for \ref distribution_builder_t. After this is
    /// done, it cannot be used again.
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

    /// \ref distribution_t wrapper used during data recording
    ///
    /// This is a thin wrapper around \ref distribution_t that is used to
    /// detect incorrect usage at compilation time:
    ///
    /// - Functions which assume that the inner allocation tracks raw value
    ///   occurence counts take an argument of type \ref distribution_builder_t
    ///   and will therefore not accept a \ref distribution_t which violates
    ///   this property.
    /// - Functions which assume that the inner allocation tracks cumulative
    ///   value occurence counts should similarly take an argument of type \ref
    ///   distribution_t.
    /// - Internal functions which can work at both stages of the distribution
    ///   lifecycle because they do not care about value occurence counts, are
    ///   explicitly documented as such and take a \ref distribution_t. It is
    ///   okay to pass the `inner` field of distribution builders to these
    ///   functions (and only them).
    typedef struct distribution_builder_s {
        distribution_t inner;  ///< Internal data collection backend
    } distribution_builder_t;


    /// \name Internal utilities
    /// \{

    // === Common tooling for distribution_t and distribution_builder_t ===

    /// Allocate a \ref distribution_t that can hold `capacity` distinct values
    ///
    /// This is an implementation detail of other methods, you should use
    /// distribution_initialize() instead of calling this method directly.
    ///
    /// \internal
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param capacity is the number of bins that the distribution should be
    ///                 able to hold internally before reallocating. It cannot
    ///                 be 0.
    ///
    /// \returns a distribution that must later be liberated using
    ///          distribution_finalize().
    distribution_t distribution_allocate(size_t capacity);

    /// Memory layout of a \ref distribution_builder_t or \ref distribution_t
    ///
    /// This layout information is be computed using distribution_layout().
    /// Users should be cautious that this information is not permanently valid
    /// and can be invalidated by various distribution operations.
    ///
    /// In the case of \ref distribution_builder_t, it is invalidated when...
    ///
    /// - The inner allocation is grown because distribution_insert() ran out
    ///   of inner storage capacity in the process of creating a new bin.
    /// - The distribution builder was turned into a \ref distribution_t via
    ///   distribution_build() or destroyed via distribution_discard().
    ///
    /// In the case of \ref distribution_t, it is invalidated when...
    ///
    /// - The distribution was turned back into a \ref distribution_builder_t
    ///   via distribution_reset() or destroyed via distribution_finalize().
    typedef struct distribution_layout_s {
        /// Sorted list of previously inserted values
        int64_t* sorted_values;

        /// Matching value occurence counts or cumulative sum thereof
        union {
            /// Value occurence counts from a \ref distribution_builder_t
            size_t* counts;

            /// Cumulative occurence counts from all bins up to and including
            /// the current bin of a \ref distribution_t.
            ///
            /// This is the cumulative sum of the `counts` from the \ref
            /// distribution_builder_t that was used to build a \ref
            /// distribution_t.
            ///
            /// If all values that were previously inserted into the
            /// distribution builder were to dumped into a flat array and sorted
            /// in ascending order, this is would be the past-the-end index
            /// after the run of identical values corresponding to each bin,
            /// i.e. its 1-based mathematical rank in this sorted order.
            size_t* end_ranks;
        };
    } distribution_layout_t;

    /// Determine the memory layout of a \ref distribution_t
    ///
    /// \param dist is a \ref distribution_t, which can be the `inner`
    ///             distribution of a \ref distribution_builder_t.
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

    /// Rounding direction used by distribution_bin_by_value()
    ///
    /// This controls how distribution_bin_by_value() behaves when the
    /// value of interest is not present in the distribution.
    typedef enum bin_direction_e {
        BIN_BELOW = -1,  ///< Find the first bin below the value of interest
        BIN_NEAREST,  ///< Find the bin closest to the value of interest
        BIN_ABOVE,  ///< Find the first bin above the value of interest
    } bin_direction_t;

    /// Find the bin of `dist` closest to `value`
    ///
    /// If `value` is present in `dist`, this returns the index of the
    /// distribution bin that contains it. Otherwise, this searches for a nearby
    /// bin according to the logic specified by `direction`:
    ///
    /// - In \ref BIN_BELOW mode, we search for the closest bin with a value
    ///   smaller than `value` and return `PTRDIFF_MIN` if there is no bin with
    ///   a smaller value.
    /// - In \ref BIN_ABOVE mode, we search for the closest bin with a value
    ///   greater than `value` and return `PTRDIFF_MAX` if there is no bin with
    ///   larger value.
    /// - In \ref BIN_NEAREST mode, we search for the closest bin. This will
    ///   succeed unless called on a \ref distribution_builder_t where no data
    ///   point was inserted yet, in which case `PTRDIFF_MIN` is returned.
    ///
    /// A common property of all these operating modes is that if the
    /// distribution contains a set of at least one value with range `[min;
    /// max]` and the input `value` belongs to this range, then this function is
    /// guaranteed return a valid bin index.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist is a \ref distribution_t, which can be the `inner`
    ///             distribution of a \ref distribution_builder_t.
    /// \param value is the value that must be searched within `dist`.
    /// \param direction is the direction in which a nearest bin must be
    ///                  searched for if `value` is not prsent in `dist`.
    ///
    /// \returns a bin index or sentinel value computed using the logic
    ///          described above.
    UDIPE_NON_NULL_ARGS
    static inline
    ptrdiff_t distribution_bin_by_value(const distribution_t* dist,
                                        int64_t value,
                                        bin_direction_t direction) {
        // Determine the distribution's memory layout
        //
        // Since this function works with both distribution_builder_t and the
        // final distribution_t, it can only use the sorted_values field.
        const int64_t* sorted_values = distribution_layout(dist).sorted_values;
        const size_t end_pos = dist->num_bins;
        tracef("Searching for a bin with a value around %zd (direction %d) "
               "within a distribution with %zu bins.",
               value, (int)direction, dist->num_bins);

        // Handle the empty distribution edge case
        if (dist->num_bins == 0) {
            trace("Distribution is empty: return placeholder bin index.");
            switch (direction) {
            case BIN_BELOW:
            case BIN_NEAREST:
                return PTRDIFF_MIN;
            case BIN_ABOVE:
                return PTRDIFF_MAX;
            }
        }

        // Handle values at or above the last value
        const size_t last_pos = end_pos - 1;
        const int64_t last_value = sorted_values[last_pos];
        if (value > last_value) {
            tracef("Input value is above last bin value %zd.", last_value);
            switch (direction) {
            case BIN_BELOW:
            case BIN_NEAREST:
                return last_pos;
            case BIN_ABOVE:
                return PTRDIFF_MAX;
            }
        } else if (value == last_value) {
            tracef("Input value is equal to last bin value %zd.", last_value);
            return last_pos;
        }
        assert(value < last_value);

        // Handle values at or below the first value
        const size_t first_pos = 0;
        const int64_t first_value = sorted_values[first_pos];
        if (value < first_value) {
            tracef("Input value is below first bin value %zd.", first_value);
            switch (direction) {
            case BIN_BELOW:
                return PTRDIFF_MIN;
            case BIN_NEAREST:
            case BIN_ABOVE:
                return first_pos;
            }
        } else if (value == first_value) {
            tracef("Input value is equal to first bin value %zd.", first_value);
            return first_pos;
        }
        assert(value > first_value);

        // At this point, we have established the following:
        // - There are at least two bins in the histogram
        // - The input value is strictly greater than the minimum value
        // - The input value is strictly smaller than the maximum value
        //
        // Use binary search to locate either the bin to which the input value
        // belongs or the pair of bins between which it resides.
        size_t below_pos = first_pos;
        int64_t below_value = first_value;
        size_t above_pos = last_pos;
        int64_t above_value = last_value;
        tracef("Input value belongs to central range ]%zd; %zd[, "
               "will now locate bin via binary search...",
               below_value, above_value);
        while (above_pos - below_pos > 1) {
            assert(below_pos < above_pos);
            assert(below_value < value);
            assert(above_value > value);
            tracef("- Input value is in range ]%zd; %zd[ from bins ]%zu; %zu[.",
                   below_value, above_value, below_pos, above_pos);

            const size_t middle_pos = below_pos + (above_pos - below_pos) / 2;
            assert(below_pos <= middle_pos);
            assert(middle_pos < above_pos);
            const int64_t middle_value = sorted_values[middle_pos];
            assert(below_value <= middle_value);
            assert(middle_value < above_value);
            tracef("- Investigating middle value %zd from bin #%zu...",
                   middle_value, middle_pos);

            if (middle_value > value) {
                trace("- It's larger: can eliminate all subsequent bins.");
                above_pos = middle_pos;
                above_value = middle_value;
                continue;
            } else if (middle_value < value) {
                trace("- It's smaller: can eliminate all previous bins.");
                below_pos = middle_pos;
                below_value = middle_value;
                continue;
            } else {
                assert(middle_value == value);
                trace("- Found a bin equal to our input value, we're done.");
                return middle_pos;
            }
        }

        // Narrowed down a pair of bins between which the value belongs, insert
        // a new bin at the appropriate position
        tracef("Narrowed search interval to 1-bin gap ]%zu; %zu[.",
               below_pos, above_pos);
        assert(above_pos == below_pos + 1);
        assert(value > below_value);
        assert(value < above_value);
        switch (direction) {
        case BIN_BELOW:
            return below_pos;
        case BIN_NEAREST:
            if (value - below_value <= above_value - value) {
                return below_pos;
            } else {
                return above_pos;
            }
        case BIN_ABOVE:
            return above_pos;
        }
        exit_with_error("Control should never reach this point!");
    }

    // === Specific to distribution_builder_t ===

    /// Create a new histogram bin within a distribution builder
    ///
    /// This is an implementation detail of distribution_insert() that should
    /// not be used directly.
    ///
    /// \internal
    ///
    /// This creates a new histogram bin associated with value `value` at
    /// position `pos`, with an occurence count of `count`. It reallocates
    /// storage and moves existing data around as needed to make room for this
    /// new bin.
    ///
    /// The caller of this function must honor the following preconditions:
    ///
    /// - `pos` must be in range `[0; dist->num_bins]`, i.e. it must either
    ///   correspond to the position of an existing bin or lie one bin past the
    ///   end of the histogram.
    /// - `value` must be strictly larger than the value associated with the
    ///   existing histogram bin at position `pos - 1`, if any.
    /// - `value` must be strictly smaller than the value assocated with the
    ///   histogram bin that was formerly at position `pos`, if any.
    /// - `count` must not be zero.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_builder_t that has previously
    ///             been set up via distribution_initialize() and hasn't been
    ///             turned into a \ref distribution_t or destroyed since.
    /// \param pos is the index at which the newly inserted bin will be
    ///            inserted in the internal bin array. It must match the
    ///            constraints spelled out earlier in this documentation.
    /// \param value is the value that will be inserted at this position. It
    ///              must match the constraints spelled out earlier in this
    ///              documentation.
    /// \param count is the initial occurence count that `value` will have.
    UDIPE_NON_NULL_ARGS
    void distribution_create_bin(distribution_builder_t* builder,
                                 size_t pos,
                                 int64_t value,
                                 size_t count);

    /// Insert `count` copies of `value` into `builder`
    ///
    /// This is an implementation detail of other methods that should not be
    /// used directly.
    ///
    /// \internal
    ///
    /// This has the same effect as calling distribution_insert() `count` times
    /// but will be more efficient.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_builder_t that has previously
    ///             been set up via distribution_initialize() and hasn't been
    ///             turned into a \ref distribution_t or destroyed since.
    /// \param value is the value to be inserted.
    /// \param count is the number of times this value should be inserted.
    UDIPE_NON_NULL_ARGS
    static inline
    void distribution_insert_copies(distribution_builder_t* builder,
                                    int64_t value,
                                    size_t count) {
        distribution_t* dist = &builder->inner;
        tracef("Asked to insert %zu copies of value %zd "
               "into a distribution with %zu bins.",
               count, value,
               dist->num_bins);

        // Find index of closest bin >= value, if any
        const int64_t bin_pos = distribution_bin_by_value(dist,
                                                          value,
                                                          BIN_ABOVE);

        // Handle past-the-end case
        const distribution_layout_t layout = distribution_layout(dist);
        if (bin_pos == PTRDIFF_MAX) {
            const size_t end_pos = dist->num_bins;
            const size_t last_pos = end_pos - 1;
            const int64_t last_value = layout.sorted_values[last_pos];
            tracef("Value is past the end of histogram %zd, "
                   "will become new last bin #%zu.",
                   last_value, end_pos);
            distribution_create_bin(builder, end_pos, value, count);
            return;
        }
        assert(bin_pos >= 0);
        assert((size_t)bin_pos < dist->num_bins);

        // Got a bin above or equal to the value, find out which
        const int64_t bin_value = layout.sorted_values[bin_pos];
        if (value == bin_value) {
            tracef("Found matching bin #%zu, add value to it.", bin_pos);
            assert(layout.counts[bin_pos] <= SIZE_MAX - count);
            layout.counts[bin_pos] += count;
        } else {
            tracef("Found upper neighbour %zd in bin #%zu, insert bin here.",
                   bin_value, bin_pos);
            distribution_create_bin(builder, bin_pos, value, count);
        }
    }

    /// Largest amount of values in any \ref distribution_builder_t bin
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param builder must be a \ref distribution_builder_t that has previously
    ///                been set up via distribution_initialize() and hasn't been
    ///                turned into a \ref distribution_t or destroyed since.
    ///
    /// \returns the largest amount of values in any bin of the distribution
    ///          builder, or 0 if no value has been inserted yet.
    UDIPE_NON_NULL_ARGS
    static inline
    size_t distribution_max_count(const distribution_builder_t* builder) {
        const distribution_layout_t layout = distribution_layout(&builder->inner);
        const size_t num_bins = builder->inner.num_bins;
        size_t max_count = 0;
        for (size_t bin = 0; bin < num_bins; ++bin) {
            const size_t count = layout.counts[bin];
            assert(count > 0);
            if (count > max_count) max_count = count;
        }
        return max_count;
    }

    // === Specific to distribution_t ===

    // Forward declaration of distribution_len()
    UDIPE_NON_NULL_ARGS
    static inline size_t distribution_len(const distribution_t* dist);

    /// Find the bin that contains the `value_rank`-th value of `dist` by sorted
    /// rank
    ///
    /// This is an implementation detail of other methods like
    /// distribution_nth() that should not be used directly.
    ///
    /// \internal
    ///
    /// This function uses the same value rank convention as
    /// distribution_nth(), but it returns the raw bin position instead of the
    /// value, which is useful for some internal computations.
    ///
    /// It must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't yet been turned back into a
    ///             \ref distribution_builder_t or destroyed since.
    /// \param value_rank specifies the value of interest by rank, as in
    ///                   distribution_nth().
    ///
    /// \returns the position of the bin that contains the `value_rank`-th value
    ///          of `dist` by sorted rank.
    UDIPE_NON_NULL_ARGS
    static inline
    size_t distribution_bin_by_rank(const distribution_t* dist,
                                    size_t value_rank) {
        // Handle the case where the value is in the first histogram bin
        tracef("Searching for the bin matching value rank %zu "
               "within a distribution with %zu bins.",
               value_rank, dist->num_bins);
        assert(value_rank < distribution_len(dist));
        const distribution_layout_t layout = distribution_layout(dist);
        const size_t first_pos = 0;
        const size_t first_end_rank = layout.end_ranks[first_pos];
        if (value_rank < first_end_rank) {
            const int64_t first_value = layout.sorted_values[first_pos];
            tracef("This is value %zd from first bin with end_rank %zu.",
                   first_value, first_end_rank);
            return first_pos;
        }

        // At this point, we have established the following:
        // - value_rank does not belong to the first bin
        // - There are at least two bins in the histogram, because value_rank
        //   must map to at least one other bin
        // - value_rank is in bounds, so it must belong to the last bin or one
        //   of the previous bins
        //
        // Use binary search to locate the bin to which value_rank belongs,
        // which is the first bin whose end_rank is strictly greater than
        // value_rank.
        size_t below_pos = first_pos;
        size_t below_end_rank = layout.end_ranks[below_pos];
        size_t above_pos = dist->num_bins - 1;
        size_t above_end_rank = layout.end_ranks[above_pos];
        tracef("Input rank belongs to central rank range [%zd; %zd[, "
               "will now locate bin via binary search...",
               below_end_rank, above_end_rank);
        while (above_pos - below_pos > 1) {
            assert(below_pos < above_pos);
            assert(below_end_rank <= value_rank);
            assert(above_end_rank > value_rank);
            tracef("- Rank is in range [%zd; %zd[ from bins ]%zu; %zu].",
                   below_end_rank, above_end_rank, below_pos, above_pos);

            const size_t middle_pos = below_pos + (above_pos - below_pos) / 2;
            assert(below_pos <= middle_pos);
            assert(middle_pos < above_pos);
            const size_t middle_end_rank = layout.end_ranks[middle_pos];
            assert(below_end_rank <= middle_end_rank);
            assert(middle_end_rank < above_end_rank);
            tracef("- Investigating end_rank %zd from middle bin #%zu...",
                   middle_end_rank, middle_pos);

            if (middle_end_rank > value_rank) {
                trace("- It's larger: can eliminate subsequent bins.");
                above_pos = middle_pos;
                above_end_rank = layout.end_ranks[above_pos];
            } else {
                trace("- It's smaller: can eliminate previous bins.");
                assert(middle_end_rank <= value_rank);
                below_pos = middle_pos;
                below_end_rank = layout.end_ranks[below_pos];
            }
        }

        // Narrowed down the pair of bins to which value_rank belongs
        tracef("Narrowed search to single bin ]%zu; %zu]: "
               "value must come from bin #%zu.",
               below_pos, above_pos, above_pos);
        assert(above_pos == below_pos + 1);
        assert(value_rank >= below_end_rank);
        assert(value_rank < above_end_rank);
        return above_pos;
    }

    /// Mark a distribution as poisoned so it cannot be used anymore
    ///
    /// This is used when a distribution is either liberated or moved to a
    /// different variable, in order to ensure that incorrect
    /// user-after-free/move can be detected.
    UDIPE_NON_NULL_ARGS
    static inline void distribution_poison(distribution_t* dist) {
        *dist = (distribution_t){
            .allocation = NULL,
            .num_bins = 0,
            .capacity = 0
        };
    }

    /// \}


    /// \name Building distributions from a stream of values
    /// \{

    /// Set up a distribution builder
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \returns a \ref distribution_builder_t that can be filled with values
    ///          via distribution_insert(), then turned into a \ref
    ///          distribution_t via distribution_build().
    distribution_builder_t distribution_initialize();

    /// Truth that no value has been inserted into a \ref distribution_builder_t
    ///
    /// \param dist must be a \ref distribution_builder_t that has previously
    ///             been set up via distribution_initialize() and hasn't been
    ///             turned into a \ref distribution_t or destroyed since.
    ///
    /// \returns the truth that no value has been inserted into this builder
    ///          since it was last initialized or reset.
    UDIPE_NON_NULL_ARGS
    static inline
    bool distribution_empty(const distribution_builder_t* builder) {
        return builder->inner.num_bins == 0;
    }

    /// Insert a value into a distribution
    ///
    /// This inserts a new occurence of `value` into the distribution histogram,
    /// creating a new bin if needed to make room for it.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_builder_t that has previously
    ///             been set up via distribution_initialize() and hasn't been
    ///             turned into a \ref distribution_t or destroyed since.
    /// \param value is the value to be inserted.
    UDIPE_NON_NULL_ARGS
    static inline void distribution_insert(distribution_builder_t* builder,
                                           int64_t value) {
        distribution_insert_copies(builder, value, 1);
    }

    /// Turn a \ref distribution_builder_t into a \ref distribution_t
    ///
    /// This can only be done after at least one value has been inserted into
    /// the distribution via distribution_insert(), and should generally be done
    /// after all data of interest has been inserted into the distribution.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param non_empty_builder must be a \ref distribution_builder_t that has
    ///                          previously received at least one value via
    ///                          distribution_insert() and hasn't been turned
    ///                          into a \ref distribution_t or destroyed since.
    ///                          It will be consumed by this function and cannot
    ///                          be used again.
    ///
    /// \returns a \ref distribution_t that can be used to efficiently query
    ///          various statistical properties of previously inserted values.
    UDIPE_NON_NULL_ARGS
    distribution_t distribution_build(distribution_builder_t* non_empty_builder);

    /// Destroy a distribution builder
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param builder must be a \ref distribution_builder_t that was previously
    ///                built using distribution_initialize() and hasn't been
    ///                turned into a \ref distribution_t or destroyed since. It
    ///                will be destroyed by this function and cannot be used
    ///                again.
    UDIPE_NON_NULL_ARGS
    void distribution_discard(distribution_builder_t* builder);

    /// \}


    /// \name Building distributions from other distributions
    /// \{

    /// Resample a distribution into another distribution of identical length
    ///
    /// This produces the same result as producing `distribution_len()` data
    /// points by calling `distribution_choose()`, but may be implemented more
    /// efficiently.
    ///
    /// That strange operation is the foundation of a statistical analysis
    /// technique called bootstrap resampling, which can estimate confidence
    /// intervals around any statistic without making any assumptions about the
    /// underlying probability law, other than assuming we have collected enough
    /// data for the empirical sample distribution to have a shape that is very
    /// close to that of the underlying probability distribution.
    ///
    /// See \ref statistics.h for more information about how and why we use
    /// bootstrap resampling in our internal statistics.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param empty_builder must be a \ref distribution_builder_t that was
    ///                      freshly built via distribution_initialize() or
    ///                      distribution_reset() and hasn't been subjected to
    ///                      any other operation since. It will be consumed by
    ///                      this function and cannot be used again.
    /// \param dist is the distribution from which data points are extracted.
    ///
    /// \returns a distribution that is resampled from `dist`.
    UDIPE_NON_NULL_ARGS
    distribution_t distribution_resample(distribution_builder_t* empty_builder,
                                         const distribution_t* dist);

    /// Build the distribution of `factor * x` for each `x` from `dist`
    ///
    /// This should produce the same result as calling
    /// `distribution_insert(builder, factor * distribution_nth(i))` for each `0
    /// <= i < distribution_len(dist)`, then calling `distribution_build()`, but
    /// with a much more efficient implementation.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param empty_builder must be a \ref distribution_builder_t that was
    ///                      freshly built via distribution_initialize() or
    ///                      distribution_reset() and hasn't been subjected to
    ///                      any other operation since. It will be consumed by
    ///                      this function and cannot be used again.
    /// \param factor is the factor applied to each data point from `dist`.
    /// \param dist is the distribution from which data points are extracted.
    ///
    /// \returns the distribution of scaled data points.
    UDIPE_NON_NULL_ARGS
    distribution_t distribution_scale(distribution_builder_t* empty_builder,
                                      int64_t factor,
                                      const distribution_t* dist);

    /// Estimate a distribution of `minuend - subtrahend` differences
    ///
    /// Given the empirical distribution of two quantities `minuend` and
    /// `subtrahend`, this estimates the distribution of their difference, i.e.
    /// the distribution of `m - s` where `m` is a random data point from
    /// `minuend` and `s` is a random data point from `subtrahend`.
    ///
    /// For a high-quality estimate, you will want...
    ///
    /// - `minuend` and `subtrahend` distributions of similar length.
    /// - Large distribution lengths, ideally 50-100x larger than the minimum
    ///   amount of values needed for the empirical distribution to be
    ///   a good approximation of the underlying true probability distribution.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param empty_builder must be a \ref distribution_builder_t that was
    ///                      freshly built via distribution_initialize() or
    ///                      distribution_reset() and hasn't been subjected to
    ///                      any other operation since. It will be consumed by
    ///                      this function and cannot be used again.
    /// \param minuend is the distribution from which the left hand side of the
    ///                subtraction will be taken.
    /// \param subtrahend is the distribution from which the right hand side of
    ///                   the subtraction will be taken.
    ///
    /// \returns an estimated distribution of `minuend - subtrahend` differences.
    UDIPE_NON_NULL_ARGS
    distribution_t distribution_sub(distribution_builder_t* empty_builder,
                                    const distribution_t* minuend,
                                    const distribution_t* subtrahend);

    /// Estimate a distribution of `num * factor / denom` scaled ratios
    ///
    /// Given the empirical distribution of two quantities `num` and `denom`,
    /// this estimates the distribution of their ratio scaled by `factor`, i.e.
    /// the distribution of `n * factor / d` where `n` is a random data point
    /// from `num` and `d` is a random data point from `denom`.
    ///
    /// See the documentation of distribution_sub() for suggestions on how to
    /// measure `num` and `denom` to achieve a high-quality estimate.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param empty_builder must be a \ref distribution_builder_t that was
    ///                      freshly built via distribution_initialize() or
    ///                      distribution_reset() and hasn't been subjected to
    ///                      any other operation since. It will be consumed by
    ///                      this function and cannot be used again.
    /// \param num is the distribution from which the numerator of the
    ///            scaled ratio will be taken.
    /// \param factor is a constant factor by which every data point from `num`
    ///               will be multiplied before division by the data point from
    ///               `denom` occurs.
    /// \param num is the distribution from which the denominator of the
    ///            scaled ratio will be taken.
    ///
    /// \returns an estimated distribution of `num * factor / denom` scaled
    ///          ratios
    UDIPE_NON_NULL_ARGS
    distribution_t distribution_scaled_div(distribution_builder_t* empty_builder,
                                           const distribution_t* num,
                                           int64_t factor,
                                           const distribution_t* denom);

    /// \}


    /// \name Querying distributions
    /// \{

    /// Number of (possibly duplicated) values inside of a \ref distribution_t
    ///
    /// This is the number of values that were inserted into the parent \ref
    /// distribution_builder_t using distribution_insert() before the input \ref
    /// distribution_t was built.
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't been turned back into a \ref
    ///             distribution_builder_t or destroyed since.
    ///
    /// \returns the number of values that were inserted into the \ref
    ///          distribution_builder_t from which this `distribution_t` was
    ///          built.
    UDIPE_NON_NULL_ARGS
    static inline size_t distribution_len(const distribution_t* dist) {
        assert(dist->num_bins >= (size_t)1);
        distribution_layout_t layout = distribution_layout(dist);
        return layout.end_ranks[dist->num_bins - 1];
    }

    /// Extract the `rank`-th value of `dist` by sorted rank
    ///
    /// In C indexing tradition, rank `0` designates the smallest value and
    /// `distribution_len(dist) - 1` designates the largest value.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't been turned back into a \ref
    ///             distribution_builder_t or destroyed since.
    /// \param rank specifies the rank of the value of interest, which should
    ///             be in the valid range from 0 inclusive to
    ///             `distribution_len(dist)` exclusive.
    ///
    /// \returns the `rank`-th value of `dist` by sorted rank.
    UDIPE_NON_NULL_ARGS
    static inline
    int64_t distribution_nth(const distribution_t* dist, size_t rank) {
        assert(dist->num_bins >= 1);
        const size_t bin = distribution_bin_by_rank(dist, rank);
        const distribution_layout_t layout = distribution_layout(dist);
        return layout.sorted_values[bin];
    }

    /// Determine how many values of `dist` are smaller than `value`, possibly
    /// including `value` itself if it is present
    ///
    /// If `value` is present in `dist`, then `include_equal = false` returns
    /// the rank of the first occurence of this value (as understood by
    /// distribution_nth()) and `include_equal = false` returns the rank of the
    /// last occurence plus one.
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't been turned back into a \ref
    ///             distribution_builder_t or destroyed since.
    /// \param value is the value which you want to position with respect to the
    ///              values inside of `dist`.
    /// \param include_equal specifies if values from `dist` that are equal to
    ///                      `value` should be included in the output count or
    ///                      not.
    ///
    /// \returns the number of values inside of `dist` that are smaller than
    ///          (and possibly equal to) `value`.
    UDIPE_NON_NULL_ARGS
    static inline size_t distribution_count_below(const distribution_t* dist,
                                                  int64_t value,
                                                  bool include_equal) {
        const distribution_layout_t layout = distribution_layout(dist);
        const ptrdiff_t pos = distribution_bin_by_value(dist,
                                                        value,
                                                        BIN_BELOW);
        if (pos < 0) return 0;
        const int64_t bin_value = layout.sorted_values[pos];
        if (bin_value < value || include_equal) {
            return layout.end_ranks[pos];
        } else {
            return (pos == 0) ? 0 : layout.end_ranks[pos - 1];
        }
    }

    /// Evaluate the quantile function of `dist` for some `probability`
    ///
    /// This returns the lowest value `x` from `dist` such that the probability
    /// of observing a value that is lower than or equal to `x` while randomly
    /// sampling the distribution is greater than or equal to `probability`
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_t that has previously been
    ///             generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't been turned back into a \ref
    ///             distribution_builder_t or destroyed since.
    /// \param probability is the probability associated with the quantile to be
    ///                    computed, which must be between 0.0 and 1.0.
    ///
    /// \returns the quantile function of `dist` evaluated at the specified
    ///          `probability`.
    UDIPE_NON_NULL_ARGS
    static inline
    int64_t distribution_quantile(const distribution_t* dist,
                                  double probability) {
        assert(probability >= 0.0 && probability <= 1.0);
        const size_t len = distribution_len(dist);
        const size_t min_values_below = ceil(probability * len);
        const size_t rank = (min_values_below == 0) ? 0 : min_values_below - 1;
        return distribution_nth(dist, rank);
    }

    /// Smallest value from `dist`
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_t that has previously been
    ///             generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't been turned back into a \ref
    ///             distribution_builder_t or destroyed since.
    ///
    /// \returns the smallest value that was previously inserted into `dist`.
    UDIPE_NON_NULL_ARGS
    static inline int64_t distribution_min_value(const distribution_t* dist) {
        assert(dist->num_bins >= 1);
        const distribution_layout_t layout = distribution_layout(dist);
        return layout.sorted_values[0];
    }

    /// Largest value from `dist`
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't been turned back into a \ref
    ///             distribution_builder_t or destroyed since.
    ///
    /// \returns the largest value that was previously inserted into `dist`.
    UDIPE_NON_NULL_ARGS
    static inline int64_t distribution_max_value(const distribution_t* dist) {
        assert(dist->num_bins >= 1);
        const distribution_layout_t layout = distribution_layout(dist);
        return layout.sorted_values[dist->num_bins - 1];
    }

    /// Smallest difference between two values of `dist`, if any, else
    /// `UINT64_MAX`
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist is a \ref distribution_t, which can be the `inner`
    ///             distribution of a \ref distribution_builder_t.
    ///
    /// \returns the smallest nonzero difference between two values of `dist`.
    ///          If all inner values are equal or there is no inner value (which
    ///          is in some sense a special case of the former), `UINT64_MAX`
    ///          can be returned. But 0 will never be returned.
    UDIPE_NON_NULL_ARGS
    static inline
    uint64_t distribution_min_difference(const distribution_t* dist) {
        const size_t num_bins = dist->num_bins;
        if (num_bins == 0) {
            trace("No value, will return UINT64_MAX");
            return UINT64_MAX;
        }

        const distribution_layout_t layout = distribution_layout(dist);
        uint64_t min_difference = UINT64_MAX;
        int64_t prev_value = layout.sorted_values[0];
        for (size_t bin = 1; bin < num_bins; ++bin) {
            const int64_t curr_value = layout.sorted_values[bin];
            assert(curr_value > prev_value);
            const uint64_t difference = curr_value - prev_value;
            if (difference < min_difference) min_difference = difference;
        }
        assert(min_difference > (uint64_t)0);
        return min_difference;
    }

    /// Smallest difference between the values of two different distributions,
    /// if any, else `UINT64_MAX`
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param d1 is a \ref distribution_t, which can be the `inner`
    ///           distribution of a \ref distribution_builder_t.
    /// \param d2 is a \ref distribution_t, which can be the `inner`
    ///           distribution of a \ref distribution_builder_t.
    ///
    /// \returns the smallest difference between two values of `d1` and `d2`. In
    ///          some edge cases where only 0 or 1 value is present in d1/d2 and
    ///          any single value is equal, `UINT64_MAX` can be returned. But 0
    ///          will never be returned.
    UDIPE_NON_NULL_ARGS
    uint64_t distribution_min_difference_with(const distribution_t* d1,
                                              const distribution_t* d2);

    /// Randomly choose a value from a \ref distribution_t
    ///
    /// This picks one of the values that were previously inserted into `dist`
    /// at random. The probability for each value to come out is given by its
    /// duplicate count divided by the total number of values that were inserted
    /// (which can be queried via distribution_len()).
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't been turned back into a \ref
    ///             distribution_builder_t or destroyed since.
    ///
    /// \returns a randomly picked values amongst those  was previously inserted into the
    ///          distribution via distribution_insert().
    UDIPE_NON_NULL_ARGS
    static inline int64_t distribution_choose(const distribution_t* dist) {
        assert(dist->num_bins >= (size_t)1);
        const size_t num_values = distribution_len(dist);
        const size_t value_rank = rand() % num_values;
        tracef("Sampling %zu-th value from a distribution containing %zu values, "
               "spread across %zu bins.",
               value_rank, num_values, dist->num_bins);
        return distribution_nth(dist, value_rank);
    }

    /// Recycle a \ref distribution_t for data recording
    ///
    /// This discards all data points from a distribution and switches it back
    /// to the \ref distribution_builder_t state where data points can be
    /// inserted into it again.
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't been turned back into a \ref
    ///             distribution_builder_t or destroyed since. It will be
    ///             consumed by this function and cannot be used again.
    ///
    /// \returns an empty \ref distribution_builder_t that reuses the former
    ///          storage allocation of `dist`.
    UDIPE_NON_NULL_ARGS
    distribution_builder_t distribution_reset(distribution_t* dist);

    /// Destroy a \ref distribution_t
    ///
    /// `dist` must not be used again after calling this function.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't been turned back into a \ref
    ///             distribution_builder_t or destroyed since. It will be
    ///             destroyed by this function and cannot be used again.
    UDIPE_NON_NULL_ARGS
    void distribution_finalize(distribution_t* dist);

    /// \}


    #ifdef UDIPE_BUILD_TESTS
        /// Unit tests
        ///
        /// This function runs all the unit tests for this module. It must be called
        /// within the scope of with_logger().
        void distribution_unit_tests();
    #endif

#endif  // UDIPE_BUILD_BENCHMARKS