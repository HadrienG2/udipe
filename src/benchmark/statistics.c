#ifdef UDIPE_BUILD_BENCHMARKS

    #include "statistics.h"

    #include <udipe/pointer.h>

    #include "distribution.h"
    #include "numeric.h"

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
        trace("Calibrating quantiles...");
        const double quantile_sym_dispersion_start = (1.0 - DISPERSION_WIDTH) / 2;
        const double quantile_low_dispersion_bound = 1.0 - DISPERSION_WIDTH;
        const double quantile_high_dispersion_bound = DISPERSION_WIDTH;
        const double quantile_sym_dispersion_end = (1.0 + DISPERSION_WIDTH) / 2;

        trace("Performing bootstrap resampling...");
        for (size_t run = 0; run < NUM_RESAMPLES; ++run) {
            tracef("- Performing resample #%zu/%zu", run, NUM_RESAMPLES);
            distribution_t resample =
                distribution_resample(&analyzer->resample_builder, dist);
            trace("  * Computing mean...");
            analyzer->statistics[MEAN][run] = analyze_mean(analyzer, &resample);
            trace("  * Computing symmetric dispersion start...");
            const int64_t sym_dispersion_start =
                distribution_quantile(&resample, quantile_sym_dispersion_start);
            analyzer->statistics[SYM_DISPERSION_START][run] =
                sym_dispersion_start;
            trace("  * Computing low dispersion bound...");
            const int64_t low_dispersion_bound =
                distribution_quantile(&resample, quantile_low_dispersion_bound);
            analyzer->statistics[LOW_DISPERSION_BOUND][run] =
                low_dispersion_bound;
            trace("  * Computing high dispersion bound...");
            const int64_t high_dispersion_bound =
                distribution_quantile(&resample, quantile_high_dispersion_bound);
            analyzer->statistics[HIGH_DISPERSION_BOUND][run] =
                high_dispersion_bound;
            trace("  * Computing symmetric dispersion end...");
            const int64_t sym_dispersion_end =
                distribution_quantile(&resample, quantile_sym_dispersion_end);
            analyzer->statistics[SYM_DISPERSION_END][run] =
                sym_dispersion_end;
            trace("  * Computing symmetric dispersion width...");
            analyzer->statistics[SYM_DISPERSION_WIDTH][run] =
                sym_dispersion_end - sym_dispersion_start;
            trace("  * Resetting resampling buffer...");
            analyzer->resample_builder = distribution_reset(&resample);
        }

        trace("Estimating population statistics...");
        estimate_t estimates[NUM_STATISTICS];
        for (size_t stat = 0; stat < NUM_STATISTICS; ++stat) {
            estimates[stat] = analyze_estimate(analyzer, (statistic_id_t)stat);
        }
        return (statistics_t){
            .mean = estimates[MEAN],
            .sym_dispersion_start = estimates[SYM_DISPERSION_START],
            .low_dispersion_bound = estimates[LOW_DISPERSION_BOUND],
            .high_dispersion_bound = estimates[HIGH_DISPERSION_BOUND],
            .sym_dispersion_end = estimates[SYM_DISPERSION_END],
            .sym_dispersion_width = estimates[SYM_DISPERSION_WIDTH]
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

        trace("Collecting mean contributions...");
        const distribution_layout_t layout = distribution_layout(dist);
        const size_t len = distribution_len(dist);
        const double len_norm = 1.0 / (double)len;
        tracef("- Distribution contains %zu values, corresponding to norm %g.",
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
            tracef("- Bin #%zu: value %zd with end_rank %zd (prev+%zu, "
                   "  %.3g%% max count) => contribution %g.",
                   bin, value, curr_end_rank, count,
                   rel_count * 100.0, mean_accumulators[bin]);
        }

        trace("Computing the mean...");
        return sum_f64(mean_accumulators, num_bins);
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
        trace("Sorting statistic values...");
        qsort(analyzer->statistics[stat],
              NUM_RESAMPLES,
              sizeof(double),
              compare_f64);

        trace("Deducing population statistic estimate...");
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