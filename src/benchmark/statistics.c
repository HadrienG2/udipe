#ifdef UDIPE_BUILD_BENCHMARKS

    #include "statistics.h"

    #include <udipe/pointer.h>

    #include "distribution.h"
    #include "numeric.h"

    #include "../error.h"
    #include "../log.h"
    #include "../memory.h"

    #include <assert.h>
    #include <math.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>


    // === Quantiles used in dispersion studies ===

    /// Lower quantile used when studying a distribution's central dispersion
    ///
    /// See \ref statistics_t::center_start for more information.
    #define CENTER_START_QUANTILE  (DISPERSION_EXCLUDED_FRACTION / 2.0)

    /// Higher quantile used when studying a distribution's central dispersion
    ///
    /// See \ref statistics_t::center_end for more information.
    #define CENTER_END_QUANTILE  (1.0 - DISPERSION_EXCLUDED_FRACTION / 2.0)

    /// Quantile used when studying a distribution's left tail
    ///
    /// See \ref statistics_t::low_tail_bound for more information.
    #define LOW_TAIL_QUANTILE  DISPERSION_EXCLUDED_FRACTION

    /// Quantile used when studying a distribution's right tail
    ///
    /// See \ref statistics_t::high_tail_bound for more information.
    #define HIGH_TAIL_QUANTILE  (1.0 - DISPERSION_EXCLUDED_FRACTION)


    // === Public estimate_t API ===

    UDIPE_NON_NULL_ARGS
    void log_estimate(udipe_log_level_t level,
                      const char header[],
                      estimate_t estimate,
                      const char mean_difference[],
                      const char unit[]) {
        // Find the smallest fluctuation around the center
        double min_spread = fabs(estimate.sample - estimate.low);
        const double high_spread = fabs(estimate.high - estimate.sample);
        if (min_spread > high_spread || min_spread == 0.0) {
            min_spread = high_spread;
        }

        // Deduce how many significant digits should be displayed
        int precision = 1;
        if (fabs(estimate.sample) != 0.0) {
            precision += floor(log10(fabs(estimate.sample)));
        }
        assert(min_spread >= 0.0);
        if (min_spread > 0.0) {
            precision += 1 - floor(log10(min_spread));
        }

        // Quantify the relative fluctuation with respect to the sample value
        double rel_width = (estimate.high - estimate.low) / fabs(estimate.sample);
        char rel_width_display[32] = { 0 };
        if (isfinite(rel_width)) {
            assert(rel_width >= 0.0);
            const int precision = (rel_width < 1.0) ? 2 : 4;
            int len = snprintf(rel_width_display, sizeof(rel_width_display),
                               " (rel width %.*g%%)",
                               precision, rel_width * 100.0);
            ensure_gt(len, 0);
            ensure_lt((size_t)len, sizeof(rel_width_display));
        }

        // Display the estimate
        udipe_logf(level,
                   "%s: %.*g %s%s with %g%% CI [%.*g; %.*g]%s.",
                   header,
                   precision,
                   estimate.sample,
                   unit,
                   mean_difference,
                   CONFIDENCE * 100.0,
                   precision,
                   estimate.low,
                   precision,
                   estimate.high,
                   rel_width_display);
    }


    // === Public statistics_t API ===

    UDIPE_NON_NULL_ARGS
    void log_statistics(udipe_log_level_t level,
                        const char title[],
                        const char bullet[],
                        statistics_t stats,
                        const char unit[]) {
        // Give the set of estimates an overarching title
        udipe_logf(level, "%s:", title);

        // Prepare to display estimates in a bullet list
        char bullet_with_space[16];
        int bullet_with_space_len =
            snprintf(bullet_with_space, sizeof(bullet_with_space),
                     "%s ", bullet);
        ensure_gt(bullet_with_space_len, 0);
        ensure_lt((size_t)bullet_with_space_len, sizeof(bullet_with_space));

        // Display the start of the central region
        log_quantile_estimate(level,
                              bullet_with_space,
                              CENTER_START_QUANTILE,
                              stats.center_start,
                              stats.mean.sample,
                              unit);

        // Display the boundary of the low tail region
        log_quantile_estimate(level,
                              bullet_with_space,
                              LOW_TAIL_QUANTILE,
                              stats.low_tail_bound,
                              stats.mean.sample,
                              unit);

        // Display the distribution mean
        char header[48];
        int len = snprintf(header, sizeof(header),
                           "%s Mean",
                           bullet);
        ensure_gt(len, 0);
        ensure_lt((size_t)len, sizeof(header));
        log_estimate(level,
                     header,
                     stats.mean,
                     "",
                     unit);

        // Display the boundary of the high tail region
        log_quantile_estimate(level,
                              bullet_with_space,
                              HIGH_TAIL_QUANTILE,
                              stats.high_tail_bound,
                              stats.mean.sample,
                              unit);

        // Display the end of the central region
        log_quantile_estimate(level,
                              bullet_with_space,
                              CENTER_END_QUANTILE,
                              stats.center_end,
                              stats.mean.sample,
                              unit);

        // Display the width of the central region
        len = write_percentile_header(header,
                                      sizeof(header),
                                      bullet_with_space,
                                      CENTER_END_QUANTILE);
        ensure_gt(len, 0);
        ensure_lt((size_t)len, sizeof(header));
        size_t remaining = sizeof(header) - (size_t)len;
        len = write_percentile_header(header + len,
                                      remaining,
                                      "-",
                                      CENTER_START_QUANTILE);
        ensure_gt(len, 0);
        ensure_lt((size_t)len, remaining);
        char mean_difference[32];
        write_mean_difference(mean_difference,
                              sizeof(mean_difference),
                              stats.center_width,
                              FRACTION,
                              stats.mean.sample);
        log_estimate(level,
                     header,
                     stats.center_width,
                     mean_difference,
                     unit);
    }


    // === Public analyzer_t API ===

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
        trace("Computing sample statistics...");
        statistics_t result;
        result.center_start.sample =
            distribution_quantile(dist, CENTER_START_QUANTILE);
        result.low_tail_bound.sample =
            distribution_quantile(dist, LOW_TAIL_QUANTILE);
        result.mean.sample = analyze_mean(analyzer, dist);
        result.high_tail_bound.sample =
            distribution_quantile(dist, HIGH_TAIL_QUANTILE);
        result.center_end.sample =
            distribution_quantile(dist, CENTER_END_QUANTILE);
        result.center_width.sample =
            result.center_end.sample - result.center_start.sample;

        trace("Performing bootstrap resampling...");
        for (size_t run = 0; run < NUM_RESAMPLES; ++run) {
            tracef("- Performing resample #%zu/%zu", run, NUM_RESAMPLES);
            distribution_t resample =
                distribution_resample(&analyzer->resample_builder, dist);
            trace("  * Computing mean...");
            analyzer->statistics[MEAN][run] = analyze_mean(analyzer, &resample);
            trace("  * Computing center start...");
            const int64_t center_start =
                distribution_quantile(&resample, CENTER_START_QUANTILE);
            analyzer->statistics[CENTER_START][run] = center_start;
            trace("  * Computing low tail bound...");
            const int64_t low_tail_bound =
                distribution_quantile(&resample, LOW_TAIL_QUANTILE);
            analyzer->statistics[LOW_TAIL_BOUND][run] = low_tail_bound;
            trace("  * Computing high tail bound...");
            const int64_t high_tail_bound =
                distribution_quantile(&resample, HIGH_TAIL_QUANTILE);
            analyzer->statistics[HIGH_TAIL_BOUND][run] = high_tail_bound;
            trace("  * Computing center end...");
            const int64_t center_end =
                distribution_quantile(&resample, CENTER_END_QUANTILE);
            analyzer->statistics[CENTER_END][run] = center_end;
            trace("  * Computing center width...");
            analyzer->statistics[CENTER_WIDTH][run] = center_end - center_start;
            trace("  * Resetting resampling buffer...");
            analyzer->resample_builder = distribution_reset(&resample);
        }

        trace("Estimating confidence intervals from resamples...");
        set_result_confidence(analyzer,
                              CENTER_START,
                              &result.center_start);
        set_result_confidence(analyzer,
                              LOW_TAIL_BOUND,
                              &result.low_tail_bound);
        set_result_confidence(analyzer,
                              MEAN,
                              &result.mean);
        set_result_confidence(analyzer,
                              HIGH_TAIL_BOUND,
                              &result.high_tail_bound);
        set_result_confidence(analyzer,
                              CENTER_END,
                              &result.center_end);
        set_result_confidence(analyzer,
                              CENTER_WIDTH,
                              &result.center_width);
        return result;
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
    size_t write_mean_difference(char output[],
                                 size_t output_size,
                                 estimate_t value,
                                 mean_comparison_t comparison,
                                 double sample_mean) {
        int len = 0;
        if (value.sample == sample_mean) {
            const char eq_mean[] = " (=mean)";
            ensure_lt(strlen(eq_mean), output_size);
            strcpy(output, eq_mean);
            return strlen(eq_mean);
        }
        const double ratio = value.sample / sample_mean;
        switch (comparison) {
        case DELTA:
            const double rel_delta = (value.sample - sample_mean) / fabs(sample_mean);
            if (isfinite(rel_delta) && fabs(rel_delta) < 1.0) {
                len = snprintf(output, output_size,
                               " (mean%+.2g%%)",
                               rel_delta * 100.0);
            } else if (fabs(rel_delta) > 1.0) {
                return write_mean_difference(output,
                                             output_size,
                                             value,
                                             RATIO,
                                             sample_mean);
            }
            break;
        case FRACTION:
            if (isfinite(ratio) && fabs(ratio) < 1.0) {
                len = snprintf(output, output_size,
                               " (%.2g%% of mean)",
                               ratio * 100.0);
            } else if (fabs(ratio) >= 1.0) {
                return write_mean_difference(output,
                                             output_size,
                                             value,
                                             RATIO,
                                             sample_mean);
            }
            break;
        case RATIO:
            if (isfinite(ratio)) {
                len = snprintf(output, output_size,
                               " (%.1fx mean)",
                               ratio);
            }
            break;
        };
        ensure_ge(len, 0);
        ensure_lt((size_t)len, output_size);
        if (!len) output[0] = '\0';
        return (size_t)len;
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
    void set_result_confidence(analyzer_t* analyzer,
                               statistic_id_t stat,
                               estimate_t* estimate) {
        trace("Sorting bootstrap statistics...");
        qsort(analyzer->statistics[stat],
              NUM_RESAMPLES,
              sizeof(double),
              compare_f64);

        trace("Deducing confidence interval...");
        const size_t last_idx = NUM_RESAMPLES - 1;
        const size_t low_idx = round((1.0 - CONFIDENCE) / 2.0 * last_idx);
        const size_t high_idx = round((1.0 + CONFIDENCE) / 2.0 * last_idx);
        estimate->low = analyzer->statistics[stat][low_idx];
        estimate->high = analyzer->statistics[stat][high_idx];
    }

#endif   // UDIPE_BUILD_BENCHMARKS