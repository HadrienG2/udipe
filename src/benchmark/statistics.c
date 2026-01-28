#ifdef UDIPE_BUILD_BENCHMARKS

    #include "statistics.h"

    #include <udipe/pointer.h>

    #include "distribution.h"

    #include "../error.h"
    #include "../log.h"
    #include "../memory.h"

    #include <assert.h>
    #include <stdlib.h>


    // === Public API ===

    analyzer_t analyzer_initialize() {
        debug("Setting up a statistical analyzer...");
        analyzer_t analyzer = { 0 };
        analyzer.resample_builder = distribution_initialize();
        analyzer.mean_capacity = get_page_size() / sizeof(double);
        ensure_ne(analyzer.mean_capacity, (size_t)0);
        analyzer.mean_accumulators = malloc(analyzer.mean_capacity * sizeof(double));
        debugf("Allocated %zu mean accumulators @ %p",
               analyzer.mean_capacity, analyzer.mean_accumulators);
        return analyzer;
    }

    UDIPE_NON_NULL_ARGS
    statistics_t analyzer_apply(analyzer_t* analyzer,
                                const distribution_t* dist) {
        trace("Performing bootstrap resampling...");
        for (size_t run = 0; run < NUM_RESAMPLES; ++run) {
            tracef("- Performing resample #%zu/%zu", run, NUM_RESAMPLES);
            distribution_t resample =
                distribution_resample(&analyzer->resample_builder, dist);
            trace("  * Computing mean...");
            analyzer->statistics[MEAN][run] =
                analyze_mean(analyzer, &resample);
            trace("  * Computing 5%% percentile...");
            const int64_t p5 = distribution_quantile(&resample, 0.05);
            analyzer->statistics[P5][run] = p5;
            trace("  * Computing 95%% percentile...");
            const int64_t p95 = distribution_quantile(&resample, 0.95);
            analyzer->statistics[P95][run] = p95;
            trace("  * Computing 95%%-5%% spread...");
            analyzer->statistics[P5_TO_P95][run] = p95 - p5;
            trace("  * Resetting resampling buffer...");
            analyzer->resample_builder = distribution_reset(&resample);
        }

        // TODO downgrade to trace
                debug("Estimating population statistics...");
        estimate_t estimates[NUM_STATISTICS];
        for (size_t stat = 0; stat < NUM_STATISTICS; ++stat) {
            estimates[stat] = analyze_estimate(analyzer, (statistic_id_t)stat);
        }
        return (statistics_t){
            .mean = estimates[MEAN],
            .p5 = estimates[P5],
            .p95 = estimates[P95],
            .p5_to_p95 = estimates[P5_TO_P95]
        };
    }

    UDIPE_NON_NULL_ARGS
    void analyzer_finalize(analyzer_t* analyzer) {
        debug("Destroying a statistical analyzer...");
        distribution_discard(&analyzer->resample_builder);

        debugf("Liberating %zu mean accumulators @ %p...",
               analyzer->mean_capacity, analyzer->mean_accumulators);
        analyzer->mean_capacity = 0;
        free(analyzer->mean_accumulators);
        analyzer->mean_accumulators = NULL;
    }


    // === Implementation details ===

    // TODO extract to header + docs
    UDIPE_NON_NULL_ARGS
    static inline size_t find_accumulator_position(const analyzer_t* analyzer,
                                                   double accumulator,
                                                   size_t contrib_bin,
                                                   size_t num_bins) {
        // TODO add logs
        const double* mean_accumulators = analyzer->mean_accumulators;
        const size_t first_bin = contrib_bin;
        const double first_value = mean_accumulators[first_bin];
        assert(first_value < accumulator);

        const size_t last_bin = num_bins - 1;
        const double last_value = mean_accumulators[last_bin];
        if (last_value <= accumulator) {
            return last_bin;
        }

        size_t bin_below = first_bin;
        double value_below = first_value;
        size_t bin_above = last_bin;
        double value_above = last_value;
        while (bin_above - bin_below > 1) {
            assert(bin_below < bin_above);
            assert(value_below <= value_above);
            const size_t bin_center = bin_below + (bin_above - bin_below) / 2;
            assert(bin_below <= bin_center);
            assert(bin_above > bin_center);
            const double value_center = mean_accumulators[bin_center];
            if (value_center < accumulator) {
                bin_below = bin_center;
                value_below = value_center;
                continue;
            } else if (value_center > accumulator) {
                bin_above = bin_center;
                value_above = value_center;
                continue;
            } else {
                assert(value_center == accumulator);
                break;
            }
        }
        assert(bin_below <= bin_above);
        assert(bin_below == bin_above - 1 || value_below == accumulator);
        return bin_below;
    }

    /// Like compare_f64() but based on magnitude rather than raw value
    UDIPE_NON_NULL_ARGS
    static inline int compare_f64_abs(const void* v1, const void* v2) {
        const double abs1 = fabs(*((const double*)v1));
        const double abs2 = fabs(*((const double*)v2));
        if (abs1 < abs2) return -1;
        if (abs1 > abs2) return 1;
        return 0;
    }

    UDIPE_NON_NULL_ARGS
    double analyze_mean(analyzer_t* analyzer,
                        const distribution_t* dist) {
        const size_t num_bins = dist->num_bins;
        ensure_ne(num_bins, (size_t)0);
        if (analyzer->mean_capacity < num_bins) {
            debugf("Reallocating %zu mean accumulators @ %p...",
                   analyzer->mean_capacity, analyzer->mean_accumulators);
            free(analyzer->mean_accumulators);
            analyzer->mean_accumulators = malloc(num_bins * sizeof(double));
            analyzer->mean_capacity = num_bins;
            debugf("...done, we now have %zu mean accumulators @ %p.",
                   analyzer->mean_capacity, analyzer->mean_accumulators);
        }

        // TODO downgrade to trace
        debug("Collecting mean contributions...");
        const distribution_layout_t layout = distribution_layout(dist);
        const size_t len = distribution_len(dist);
        const double len_norm = 1.0 / (double)len;
        // TODO downgrade to trace
                debugf("- Distribution contains %zu values, corresponding to norm %g.",
               len, len_norm);
        double* const mean_accumulators = analyzer->mean_accumulators;
        size_t prev_end_rank = 0;
        for (size_t bin = 0; bin < num_bins; ++bin) {
            const int64_t value = layout.sorted_values[bin];

            const size_t curr_end_rank = layout.end_ranks[bin];
            const size_t count = curr_end_rank - prev_end_rank;
            prev_end_rank = curr_end_rank;
            const double rel_count = (double)count * len_norm;

            mean_accumulators[bin] = rel_count * (double)value;
            // TODO downgrade to trace
                    debugf("- Bin #%zu: value %zd with end_rank %zd (prev+%zu, %.3g%% max count) => contribution %g.", bin, value, curr_end_rank, count, rel_count * 100.0, mean_accumulators[bin]);
        }

        /*
            TODO: Try this new algorithmic approach:

            - There is an outer loop, which runs until only 1 accumulator is left
            - On each iteration, we want to sum some accumulators with other
              accumulators, under the following constraints:
              * Prefer summing accumulators of comparable magnitude. This is
                where floating point summation is most accurate.
              * Prefer summing positive and negative accumulators as early as
                possible. Cancelation essentially discards the upper mantissa
                bits, so the lower mantissa bits better be as accurate as
                possible, and this is achieved by summing terms which have went
                through as few sums as possible.
            - We achieve this in the following way:
              1. Initially sort numbers by exponent then by sign then by
                 mantissa (increasing exponent, within each exponent negative
                 sign then positive sign, and within each sign increasing
                 mantissa).
              2. Iterate or bsearch over the first values to get...
                 - The position of the negative/positive cutoff point
                   first_positive_bin.
                 - The boundary between the first and second exponent
                   next_exponent_bin and the value of the exponent on each side
                   (first_exponent and next_exponent).
              3. Deduce the number of negative values num_negatives =
                 first_positive_bin and the number of positive values
                 num_positives = next_exponent_bin - first_positive_bin.
              4. If there is at least one negative and one positive value,
                 priorize early cancelation to reduce precision hit.
                 - Check which of num_negatives and num_positives is larger
                   * If num_positives is larger, sum values 0..num_negatives
                     into values
                     first_positive_bin..first_positive_bin+num_negatives.
                   * If num_negatives is larger, sum values
                     first_positive_bin..next_exponent_bin into values
                     0..num_positives then shift values 0..num_negatives to
                     positions next_negative_bin-num_positives..next_negative_bin.
                 - At the end, values min(num_negatives,
                   num_positives)..2*min(num_negatives, num_positives) will have
                   decreased to some lower exponent and values whereas values
                   2*min(num_negatives, num_positives)..next_exponent_bin have
                   not changed and should therefore have a higher magnitude.
                 - Perform sort by exponent -> sign -> mantissa of the new
                   values at position min(num_negatives,
                   num_positives)..2*min(num_negatives, num_positives), shift
                   array start by +min(num_negatives, num_positives), decrease
                   array length by -min(num_negatives, num_positives), and
                   repeat from step 2.
              5. If there are >= 2 values with the same sign, pick the midpoint
                 rounded down (midpoint = next_exponent_bin/2), sum values
                 0..midpoint into values
                 next_exponent_bin-midpoint..next_exponent_bin, shift array
                 start by +midpoint and array length by -midpoint, and repeat
                 from step 2.
              6. Otherwise, there is only 1 remaining value with this magnitude.
                 In that case...
                  - Start over from 2. but with a starting bin of 1 and a last
                    bin of len-1.
                  - Once we know what the first positive and negative values of
                    the next exponent (if any) are, figure out if one of them
                    has a sign that's opposite from our leftover value.
                     * If our leftover value is positive and there is a negative
                       value with the next exponent, sum it into the first
                       negative value (which is the one with the smallest
                       mantissa/magnitude). This will reduce the mantissa of
                       what is already the lowest-mantissa negative number, so
                       the array deprived of its first value has the expected
                       ordering, we are done processing the previous exponent
                       magnitude, and we can resume processing the new exponent
                       magnitude as in step 3+.
                     * If our leftover value is negative and there is a positive
                       value with the next exponent, then sum it into that. This
                       works for the same reason as the opposite case.
                     * If our leftover value has the same sign as the other
                       values, then we...
                        - Sum it into the value with the smallest mantissa,
                          which is the one at next_exponent_bin.
                        - bsearch the point where the resulting value with
                          higher mantissa and possibly higher exponent should be
                          inserted within range 1..length.
                        - Insert the value at this position.
                        - ...and at this point we're back with an array of
                          values with the same exponent and sign (which may or
                          may not include the current value depending on if the
                          summation made it switch to the next exponent. So we
                          can resume as in step 5/6.

            If this works well, delete find_accumulator_position() and
            compare_f64_abs().
        */

        trace("Sorting mean contributions...");
        qsort(mean_accumulators,
              num_bins,
              sizeof(double),
              compare_f64_abs);

        debug("Computing the mean...");
        const size_t last_bin = num_bins - 1;
        double accumulator = mean_accumulators[0];
        for (size_t contrib_bin = 1; contrib_bin < num_bins; ++contrib_bin) {
            const double contribution = mean_accumulators[contrib_bin];
            // TODO: downgrade to trace
            debugf("- At bin #%zu: accumulator %g += contribution %g...",
                   contrib_bin, accumulator, contribution);

            if (fabs(accumulator) <= fabs(2.0 * contribution)) {
                trace("  * That's precise enough as accumulator is still <= contribution.");
                accumulator += contribution;
                continue;
            }
            assert(fabs(accumulator) > fabs(2.0 * contribution));

            // TODO downgrade to trace, remove numbers.
            debug("  * Accumulator > 2x contribution, this may be bad for precision. "
                  "Do we have better acccumulation options?");
            if (contrib_bin + 1 == num_bins) {
                // TODO downgrade to trace
                debug("    - No other option as it's the last contribution.");
                accumulator += contribution;
                break;
            }
            assert(contrib_bin + 1 < num_bins);

            const double next_contribution = mean_accumulators[contrib_bin+1];
            if (next_contribution/contribution > accumulator/contribution) {
                // TODO downgrade to trace
                debug("    - Next contribution would be a worse peer than the accumulator.");
                accumulator += contribution;
                continue;
            }

            // TODO Downgrade to trace
            debug("    - Ok, contribution will thus become the new accumulator...");

            const size_t accumulator_bin =
                find_accumulator_position(analyzer,
                                          accumulator,
                                          contrib_bin,
                                          num_bins);
            // TODO downgrade to trace
                    debugf("    - Migrating accumulator to position %zu...",
                   accumulator_bin);
            for (size_t src_bin = contrib_bin + 2; src_bin <= accumulator_bin; ++src_bin) {
                const size_t dst_bin = src_bin - 1;
                mean_accumulators[dst_bin] = mean_accumulators[src_bin];
            }
            mean_accumulators[accumulator_bin] = accumulator;

            assert(contribution <= next_contribution);
            // TODO downgrade to trace
                    debugf("    - Using former contribution %g as new accumulator "
                   "to integrate new contribution %g...",
                   contribution, next_contribution);
            accumulator = contribution + next_contribution;
        }
        return accumulator;
    }

    /// Comparison function for applying qsort() to double[] where it is assumed
    /// that all inner numbers are normal (not NAN)
    UDIPE_NON_NULL_ARGS
    static inline int compare_f64(const void* v1, const void* v2) {
        const double d1 = *((const double*)v1);
        const double d2 = *((const double*)v2);
        if (d1 < d2) return -1;
        if (d1 > d2) return 1;
        return 0;
    }

    UDIPE_NON_NULL_ARGS
    estimate_t analyze_estimate(analyzer_t* analyzer,
                                statistic_id_t stat) {
        // TODO downgrade to trace
                debug("Sorting statistic values...");
        qsort(analyzer->statistics[stat],
              NUM_RESAMPLES,
              sizeof(double),
              compare_f64);

        // TODO downgrade to trace
                debug("Deducing population statistic estimate...");
        const size_t last_idx = NUM_RESAMPLES - 1;
        const size_t center_idx = last_idx / 2;
        const size_t low_idx = round((1.0 - CONFIDENCE) / 2.0 * last_idx);
        const size_t high_idx = round((1.0 + CONFIDENCE) / 2.0 * last_idx);
        return (estimate_t){
            .center = analyzer->statistics[stat][center_idx],
            .low = analyzer->statistics[stat][low_idx],
            .high = analyzer->statistics[stat][high_idx]
        };
    }

#endif   // UDIPE_BUILD_BENCHMARKS