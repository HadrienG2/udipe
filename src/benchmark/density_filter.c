#ifdef UDIPE_BUILD_BENCHMARKS

    #include "density_filter.h"

    #include <udipe/pointer.h>

    #include "distribution.h"

    #include "../error.h"
    #include "../log.h"

    #include <stddef.h>
    #include <stdint.h>


    // === Configuration constants ===

    /// Scaling factor to apply to the log2 of accepted densities before
    /// truncating them to integers
    ///
    /// Larger values improve the precision of internal computations at the
    /// expense of reducing range and making displays less readable.
    static const int64_t LOG2_SCALE = 1000;


    // === Public API ===

    // TODO doc
    UDIPE_NON_NULL_ARGS
    static inline void update_max_density(void* context,
                                          double density,
                                          size_t /* count */) {
        double* const max_density = (double*)context;
        if (density > *max_density) {
            trace("That's the new maximum absolute density.");
            *max_density = density;
        }
    }

    // TODO doc
    typedef struct density_inserter_s {
        distribution_builder_t* builder;
        double max_density;
    } density_inserter_t;
    //
    // TODO doc
    static inline void insert_log2_density(void* context,
                                           double density,
                                           size_t count) {
        density_inserter_t* const inserter = (density_inserter_t*)context;
        distribution_builder_t* const builder = inserter->builder;
        const double max_density = inserter->max_density;

        assert(density <= max_density);
        const double relative_density = density / max_density;
        //
        // TODO: Experiment further with applying a scaling factor to the log2
        const int64_t log2_density = floor(LOG2_SCALE*log2(relative_density));
        assert(log2_density <= 0);
        tracef("That's a relative density of %g, "
               "which corresponds to log2 bin %zd. Inserting it...",
               relative_density, log2_density);

        distribution_insert_copies(builder, log2_density, count);
    }

    UDIPE_NON_NULL_ARGS
    distribution_t
    distribution_compute_log2_density(distribution_builder_t* empty_builder,
                                      const distribution_t* input) {
        ensure_eq(empty_builder->inner.num_bins, (size_t)0);
        distribution_builder_t* builder = empty_builder;
        empty_builder = NULL;
        tracef("Computing the density of a distribution with %zu bins...",
               input->num_bins);

        ensure_ge(input->num_bins, (size_t)1);
        if (input->num_bins <= 2) {
            trace("Encountered 1/2 bins special case: "
                  "all values are at max relative_density.");
            ensure_ge(builder->inner.capacity, (size_t)1);
            distribution_layout_t builder_layout = distribution_layout(&builder->inner);
            builder_layout.sorted_values[0] = 0;
            builder_layout.counts[0] = distribution_len(input);
            builder->inner.num_bins = 1;
            return distribution_build(builder);
        }

        trace("Computing maximum absolute bin density...");
        double max_density = 0.0;
        for_each_density(input, &update_max_density, (void*)&max_density);
        ensure_gt(max_density, 0.0);
        tracef("Maximum absolute bin density is %g.", max_density);

        // TODO: It's better to have a two-pass algorithm: first compute the
        //       relative density for all bins, then build the scaled log2
        //       distribution for visualization and cut calculation. This way,
        //       when we eventually apply the cut, we can reuse the previously
        //       computed density table instead of starting over with the two
        //       passes of maximum density computation and relative density
        //       computation.
        trace("Building distribution of log2(relative density)...");
        density_inserter_t inserter = {
            .builder = builder,
            .max_density = max_density
        };
        for_each_density(input, &insert_log2_density, (void*)&inserter);
        ensure_ge(builder->inner.num_bins, (size_t)1);
        return distribution_build(builder);
    }


    // === Implementation details ===

    UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2)
    void for_each_density(const distribution_t* input,
                          void (*callback)(void* /* context */,
                                           double /* density */,
                                           size_t /* count */),
                          void* context) {
        ensure_ge(input->num_bins, (size_t)2);
        const distribution_layout_t input_layout = distribution_layout(input);
        int64_t current_value = input_layout.sorted_values[0];
        size_t current_count = input_layout.end_ranks[0];
        size_t current_end_rank = current_count;
        uint64_t distance_to_prev = UINT64_MAX;
        for (size_t input_pos = 1; input_pos < input->num_bins; ++input_pos) {
            tracef("- Processing bin %zu with value %zd (previous+%zu) and count %zu.",
                   input_pos - 1, current_value, distance_to_prev, current_count);

            const int64_t next_value = input_layout.sorted_values[input_pos];
            assert(next_value > current_value);
            const uint64_t distance_to_next = (uint64_t)(next_value - current_value);
            tracef("  * Next bin %zu has value %zd (current+%zd).",
                   input_pos, next_value, distance_to_next);

            const uint64_t distance_to_nearest =
                (distance_to_prev <= distance_to_next)
                    ? distance_to_prev
                    : distance_to_next;
            tracef("  * Nearest bin distance is %zu.", distance_to_nearest);

            const double density = current_count / (double)distance_to_nearest;
            assert(density > 0.0);
            tracef("  * Invoking user callback with absolute density %g "
                   "and value count %zu...",
                   density, current_count);
            callback(context, density, current_count);

            trace("  * Switching to next bin...");
            current_value = next_value;
            current_count = input_layout.end_ranks[input_pos] - current_end_rank;
            current_end_rank = input_layout.end_ranks[input_pos];
            distance_to_prev = distance_to_next;
        }
        const double last_density = current_count / (double)distance_to_prev;
        ensure_gt(last_density, 0.0);
        tracef("  * Invoking user callback with last density %g "
               "and value count %zu...",
               last_density, current_count);
        callback(context, last_density, current_count);
    }

#endif  // UDIPE_BUILD_BENCHMARKS