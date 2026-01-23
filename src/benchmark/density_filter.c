#ifdef UDIPE_BUILD_BENCHMARKS

    #include "density_filter.h"

    #include <udipe/pointer.h>

    #include "distribution.h"

    #include "../error.h"
    #include "../log.h"
    #include "../memory.h"

    #include <stddef.h>
    #include <stdint.h>


    // === Configuration constants ===

    /// Relative contribution of nearest neighbors to bin weights (0.0 to 1.0)
    ///
    /// At one extreme, 0.0 means that only the amount of values within the
    /// current distribution bin is taken into account for outlier
    /// classification. This is unadvisable as it will not work when measuring
    /// wildly fluctuating durations or using small sample sizes in such a way
    /// that identical durations do not start piling up.
    ///
    /// At the other extreme, 1.0 means that the amount of values within the
    /// current distribution bin is not taken into account. This is also
    /// unadvisable as value occurence counts are a very useful indicator of
    /// outlier-ness when operating in the right experimental conditions.
    static const double NEIGHBOR_CONTRIBUTION = 1.0 / 4.0;

    /// Rate of distance-driven neighbour contribution decay
    ///
    /// This should be strictly higher than 0.0. The higher this is, the more
    /// quickly neighbour contributions will go down as neighbour distance
    /// increases with respect to the smallest inter-neighbour distance.
    static const double NEIGHBOR_DECAY = 2.0;

    /// Relative weight below which distribution bins should be suspected of
    /// containing outliers
    static const double OUTLIER_THRESHOLD = 0.005;

    /// Maximum fraction of distribution values that can be rejected as outliers
    ///
    /// This serves as a last-chance safety feature in case \ref
    /// OUTLIER_THRESHOLD turns out to be tuned too high for a particular
    /// distribution, but hitting this threshold is generally suspicious.
    static const double MAX_OUTLIER_FRACTION = 0.05;


    // === Public API ===

    density_filter_t density_filter_initialize() {
        const size_t bin_capacity = get_page_size() / sizeof(double);
        double* const bin_weights = malloc(bin_capacity * sizeof(double));
        debugf("Allocated bin weight storage @ %p...", bin_weights);

        const recyclable_distribution_t last_scores = (recyclable_distribution_t){
            .empty_builder = distribution_initialize()
        };
        const recyclable_distribution_t last_rejections = (recyclable_distribution_t){
            .empty_builder = distribution_initialize()
        };

        return (density_filter_t) {
            .bin_weights = bin_weights,
            .bin_capacity = bin_capacity,
            .last_scores = last_scores,
            .last_rejections = last_rejections,
            .applied = false
        };
    }

    UDIPE_NON_NULL_ARGS
    void density_filter_apply(density_filter_t* filter,
                              distribution_builder_t* target) {
        compute_rel_weights(filter, target);
        compute_scores(filter, target);
        const double threshold = compute_weight_threshold(filter);
        reject_bins(filter, target, threshold);
        filter->applied = true;
    }


    UDIPE_NON_NULL_ARGS
    void density_filter_finalize(density_filter_t* filter) {
        debugf("Liberating bin weight storage @ %p...", filter->bin_weights);
        free(filter->bin_weights);
        filter->bin_weights = NULL;
        filter->bin_capacity = 0;

        trace("Liberating inner distributions...");
        if (filter->applied) {
            distribution_finalize(&filter->last_scores.distribution);
            distribution_finalize(&filter->last_rejections.distribution);
        } else {
            distribution_discard(&filter->last_scores.empty_builder);
            distribution_discard(&filter->last_rejections.empty_builder);
        }

        trace("Poisoning the rest of the density filter...");
        filter->applied = false;
    }


    // === Implementation details ===

    UDIPE_NON_NULL_ARGS
    void compute_rel_weights(density_filter_t* filter,
                             const distribution_builder_t* target) {
        const size_t num_bins = target->inner.num_bins;
        if (filter->bin_capacity < num_bins) {
            debugf("Reallocating weights storage @ %p "
                   "to make room for %zu bins...",
                   filter->bin_weights,
                   num_bins);
            free(filter->bin_weights);
            filter->bin_weights = (double*)malloc(num_bins * sizeof(double));
            debugf("Bin weights storage is now located @ %p",
                   filter->bin_weights);
            filter->bin_capacity = num_bins;
        }

        ensure_ge(num_bins, (size_t)1);
        if (num_bins == 1) {
            trace("Encountered 1-bin special case: "
                  "Single value must have max relative weight 1.0.");
            ensure_ge(filter->bin_capacity, (size_t)1);
            filter->bin_weights[0] = 1.0;
            return;
        }

        ensure_ge(num_bins, (size_t)2);
        const distribution_layout_t target_layout =
            distribution_layout(&target->inner);
        trace("Calibrating score metric...");
        uint64_t min_distance = UINT64_MAX;
        size_t max_count = target_layout.counts[0];
        int64_t previous_value = target_layout.sorted_values[0];
        tracef("- Initialized from bin #0 with value %zd and count %zu.",
               previous_value, max_count);
        for (size_t bin = 1; bin < num_bins; ++bin) {
            const int64_t current_value = target_layout.sorted_values[bin];
            const size_t count = target_layout.counts[bin];
            if (count > max_count) max_count = count;
            tracef("- Integrating bin #%zu with value %zd and count %zu...",
                   bin, current_value, count);

            assert(current_value > prev_value);
            const uint64_t distance = current_value - previous_value;
            tracef("- Distance from previous bin is %zd.", distance);
            if (distance < min_distance) min_distance = distance;
            previous_value = current_value;
        }
        const double count_norm = 1.0 / max_count;
        const double distance_norm = 1.0 / min_distance;
        tracef("Distribution has max count %zu (count norm %.3g) "
               "and min distance %zu (distance norm %g)",
               max_count, count_norm,
               min_distance, distance_norm);

        trace("Weighting distribution bins...");
        const double neighbor_share = NEIGHBOR_CONTRIBUTION / 2;
        const double self_share = 1.0 - NEIGHBOR_CONTRIBUTION;
        int64_t current_value = target_layout.sorted_values[0];
        size_t current_count = target_layout.counts[0];
        double rel_current_count = count_norm * current_count;
        double rel_prev_count = 0.0;
        uint64_t distance_to_prev = UINT64_MAX;
        double rel_prev_distance = distance_norm * distance_to_prev;
        double max_weight = -INFINITY;
        for (size_t bin = 1; bin < num_bins; ++bin) {
            tracef("- Processing bin #%zu with value %zd "
                   "and count %zu (%.3g%%).",
                   bin - 1, current_value,
                   current_count, rel_current_count * 100.0);

            if (rel_prev_count) {
                tracef("  * Previous bin had value %zd (%.1fx min distance) "
                       "and relative count %.3g%%.",
                       current_value - distance_to_prev, rel_prev_distance,
                       rel_prev_count * 100.0);
            }

            const int64_t next_value = target_layout.sorted_values[bin];
            assert(next_value > current_value);
            const uint64_t distance_to_next = (uint64_t)(next_value - current_value);
            const double rel_next_distance = distance_norm * distance_to_next;
            const size_t next_count = target_layout.counts[bin];
            const double rel_next_count = count_norm * next_count;
            tracef("  * Next bin #%zu has value %zd (%.1fx min distance) "
                   "and count %zu (%.3g%%).",
                   bin, next_value, rel_next_distance,
                   next_count, rel_next_count * 100.0);

            const double prev_weight =
                rel_prev_count * pow(rel_prev_distance, -NEIGHBOR_DECAY);
            const double next_weight =
                rel_next_count * pow(rel_next_distance, -NEIGHBOR_DECAY);
            const double current_weight =
                neighbor_share * prev_weight
                + self_share * rel_current_count
                + neighbor_share * next_weight;
            tracef("  * Current bin weight is therefore %.3g.", current_weight);
            filter->bin_weights[bin-1] = current_weight;
            if (current_weight > max_weight) max_weight = current_weight;

            trace("  * Preparing for next bin...");
            current_value = next_value;
            current_count = next_count;
            rel_prev_count = rel_current_count;
            rel_current_count = rel_next_count;
            distance_to_prev = distance_to_next;
            rel_prev_distance = rel_next_distance;
        }
        const double prev_weight =
            rel_prev_count * pow(rel_prev_distance, -NEIGHBOR_DECAY);
        const double last_weight =
            neighbor_share * prev_weight
            + self_share * rel_current_count;
        tracef("- No bin remaining: last bin weight is %.3g.", last_weight);
        filter->bin_weights[num_bins - 1] = last_weight;
        if (last_weight > max_weight) max_weight = last_weight;

        const double weight_norm = 1.0 / max_weight;
        tracef("Maximum weight is %.3g: "
               "will now apply norm %.3g to get relative weight...",
               max_weight, weight_norm);
        for (size_t bin = 0; bin < num_bins; ++bin) {
            filter->bin_weights[bin] *= weight_norm;
        }
    }

    UDIPE_NON_NULL_ARGS
    void compute_scores(density_filter_t* filter,
                        const distribution_builder_t* target) {
        if (filter->applied) {
            trace("Resetting last scores distribution...");
            filter->last_scores.empty_builder =
                distribution_reset(&filter->last_scores.distribution);
        }
        distribution_builder_t* score_builder =
            &filter->last_scores.empty_builder;

        const size_t num_bins = target->inner.num_bins;
        ensure_le(num_bins, filter->bin_capacity);
        const distribution_layout_t target_layout =
            distribution_layout(&target->inner);
        for (size_t bin = 0; bin < num_bins; ++bin) {
            const double rel_weight = filter->bin_weights[bin];
            assert(rel_weight >= 0.0 && rel_weight <= 1.0);
            tracef("- Processing bin #%zu with relative weight %.3g... ",
                   bin, rel_weight);

            const int64_t score = rel_weight_to_score(rel_weight);
            tracef("  * ...which corresponds to a score of %zd.", score);

            const size_t count = target_layout.counts[bin];
            tracef("  * Recording it with occurence count %zu...", count);
            distribution_insert_copies(score_builder,
                                       score,
                                       count);
        }
        filter->last_scores.distribution = distribution_build(score_builder);
    }

    UDIPE_NON_NULL_ARGS
    double compute_weight_threshold(const density_filter_t* filter) {
        ensure_gt(OUTLIER_THRESHOLD, 0.0);
        ensure_lt(OUTLIER_THRESHOLD, 1.0);
        const int64_t outlier_score = rel_weight_to_score(OUTLIER_THRESHOLD);
        tracef("Looking for outlier bins with rel weight <= %.2g (score <= %zd).",
               OUTLIER_THRESHOLD, outlier_score);

        const distribution_t* scores = &filter->last_scores.distribution;
        const distribution_layout_t scores_layout = distribution_layout(scores);
        const ptrdiff_t last_outlier_pos =
            distribution_bin_by_value(scores, outlier_score, BIN_BELOW);
        if (last_outlier_pos == PTRDIFF_MIN) {
            trace("All bins are above score threshold: "
                  "will not cut any data point.");
            return 0.0;
        }

        ensure_ge(last_outlier_pos, 0);
        const size_t last_outlier_bin = (size_t)last_outlier_pos;
        const size_t num_outliers = scores_layout.end_ranks[last_outlier_bin];
        const size_t num_inputs = distribution_len(scores);
        const double outlier_fraction = num_outliers / (double)num_inputs;
        tracef("That's %zu/%zu outlier values (%.3g%%), "
               "corresponding to score bins up to #%zu.",
               num_outliers, num_inputs, outlier_fraction * 100.0,
               last_outlier_bin);

        ensure_gt(MAX_OUTLIER_FRACTION, 0.0);
        ensure_lt(MAX_OUTLIER_FRACTION, 1.0);
        const size_t max_outliers = floor(MAX_OUTLIER_FRACTION * num_inputs);
        if (num_outliers <= max_outliers) {
            const int64_t max_score = scores_layout.sorted_values[last_outlier_bin];
            const double max_rel_weight = score_to_rel_weight(max_score);
            tracef("Those values have rel weight <= %.2g (score <= %zd).",
                   max_rel_weight, max_score);
            return OUTLIER_THRESHOLD;
        }

        warnf("There are %zu/%zu values below the outlier threshold, "
              "but we can only cut %.3g%% of the dataset (%zu values). "
              "Adjusting outlier threshold to stay in tolerance...",
              num_outliers, num_inputs,
              MAX_OUTLIER_FRACTION * 100.0, max_outliers);
        size_t max_bin = distribution_bin_by_rank(scores, max_outliers);
        if (scores_layout.end_ranks[max_bin] > max_outliers) {
            if (max_bin == 0) {
                trace("Even the first score has too many associated values: "
                      "won't cut any data point.");
                return 0.0;
            }
            --max_bin;
        }

        const int64_t max_score = scores_layout.sorted_values[max_bin];
        ensure_le(max_score, 0);
        const double max_rel_weight = score_to_rel_weight(max_score);
        ensure_ge(max_rel_weight, 0.0);
        ensure_le(max_rel_weight, 1.0);
        warnf("Will only drop the first %zu score bins, corresponding to "
              "%zu data points with rel weight <= %.3g (score <= %zd).",
              max_bin + 1,
              scores_layout.end_ranks[max_bin],
              max_rel_weight,
              max_score);
        return max_rel_weight;
    }

    UDIPE_NON_NULL_ARGS
    void reject_bins(density_filter_t* filter,
                     distribution_builder_t* target,
                     double threshold) {
        if (filter->applied) {
            trace("Resetting rejections distribution...");
            filter->last_rejections.empty_builder =
                distribution_reset(&filter->last_rejections.distribution);
        }
        distribution_builder_t* rejections_builder =
            &filter->last_rejections.empty_builder;

        const size_t num_input_bins = target->inner.num_bins;
        ensure_le(num_input_bins, filter->bin_capacity);
        tracef("Rejecting bins with relative weight <= %.3g "
               "from our %zu-bins dataset.",
               threshold,
               num_input_bins);

        const distribution_layout_t target_layout =
            distribution_layout(&target->inner);
        int64_t* sorted_values = target_layout.sorted_values;
        size_t* counts = target_layout.counts;
        size_t num_deleted_bins = 0;
        for (size_t input_bin = 0; input_bin < num_input_bins; ++input_bin) {
            const int64_t value = sorted_values[input_bin];
            const size_t count = counts[input_bin];
            const double rel_weight = filter->bin_weights[input_bin];
            tracef("- Processing bin #%zu "
                   "containing %zu occurences of value %zd "
                   "with relative weight %.3g.",
                   input_bin, count, value, rel_weight);

            if (rel_weight > threshold) {
                if (num_deleted_bins > 0) {
                    const size_t output_bin = input_bin - num_deleted_bins;
                    tracef("  * Packed to new bin position #%zu.", output_bin);
                    sorted_values[output_bin] = value;
                    counts[output_bin] = count;
                } else {
                    trace("  * Nothing to do.");
                }
            } else {
                trace("  * Moving bin to rejected value distribution...");
                distribution_insert_copies(rejections_builder,
                                           value,
                                           count);
                ++num_deleted_bins;
            }
        }
        target->inner.num_bins -= num_deleted_bins;

        trace("Finalizing rejected value distribution...");
        filter->last_rejections.distribution =
            distribution_build(rejections_builder);
    }

#endif  // UDIPE_BUILD_BENCHMARKS