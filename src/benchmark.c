#ifdef UDIPE_BUILD_BENCHMARKS

    #include "benchmark.h"

    #include <udipe/log.h>
    #include <udipe/pointer.h>

    #include "memory.h"
    #include "unit_tests.h"
    #include "visibility.h"

    #include <assert.h>
    #include <hwloc.h>
    #include <math.h>
    #include <stddef.h>
    #include <stdint.h>
    #include <stdlib.h>
    #include <string.h>


    /// Number of samples used for median duration computations
    ///
    /// To reduce the impact of outliers, we don't directly handle raw
    /// durations, we handle medians of a small number of duration samples. This
    /// parameter controls the number of samples that are used.
    ///
    /// Tuning this parameter has many consequences:
    ///
    /// - It can only take odd values. No pseudo-median allowed.
    /// - Tuning it higher allows you to tolerate more OS interrupts, and thus
    ///   work with benchmark run durations that are closer to the
    ///   inter-interrupt spacing. Given a fixed run timing precision, these
    ///   longer benchmark runs let you achieve lower uncertainty on the
    ///   benchmark iteration duration.
    /// - Tuning it higher makes statistics more sensitive to the difference
    ///   between the empirical duration distribution and the true duration
    ///   distribution, therefore you need to collect more benchmark run
    ///   duration data points for the statistics to converge. When combined
    ///   with the use of longer benchmark runs, this means that benchmarks will
    ///   take longer to execute before stable results are achieved.
    #define NUM_MEDIAN_SAMPLES ((size_t)7)
    static_assert(NUM_MEDIAN_SAMPLES % 2 == 1,
                  "Medians are computed over an odd number of samples");

    /// Confidence interval used for all statistics
    ///
    /// Picked because 95% is kinda the standard in statistics, so it is what
    /// the end user will most likely be used to.
    #define CONFIDENCE 95.0

    /// Desired number of measurements on either side of the confidence interval
    ///
    /// Tune this up if you observe unstable duration statistics even though the
    /// underlying duration distributions are stable.
    ///
    /// Tuning it too high will increase the overhead of the statistical
    /// analysis process for no good reason.
    //
    // TODO: Tune on more system
    #define NUM_EDGE_MEASUREMENTS ((size_t)512)

    /// Warmup duration used for OS clock offset calibration
    //
    // TODO: Tune on more systems
    #define WARMUP_OFFSET_OS (1*UDIPE_MILLISECOND)

    /// Number of benchmark runs used for OS clock offset calibration
    ///
    /// Tune this up if clock offset calibration is unstable, as evidenced by
    /// the fact that short loops get a nonzero median duration.
    //
    // TODO: Tune on more systems
    #define NUM_RUNS_OFFSET_OS ((size_t)8*1024)

    /// Warmup duration used for shortest loop calibration
    //
    // TODO: Tune on more systems
    #define WARMUP_SHORTEST_LOOP (1*UDIPE_MILLISECOND)

    /// Number of benchmark runs used for shortest loop calibration
    ///
    /// Tune this up if the shortest loop calibration is unstable and does not
    /// converge to a constant loop size.
    //
    // TODO: Tune on more systems
    #define NUM_RUNS_SHORTEST_LOOP ((size_t)1024)

    /// Warmup duration used for best loop calibration
    //
    // TODO: Tune on more systems
    #define WARMUP_BEST_LOOP (100*UDIPE_MILLISECOND)

    /// Number of benchmark run used for optimal loop calibration, when using
    /// the system clock to perform said calibration
    ///
    /// Tune this up if the optimal loop calibration is unstable and does not
    /// converge to sufficiently reproducible statistics.
    //
    // TODO: Tune on more systems
    #define NUM_RUNS_BEST_LOOP_OS ((size_t)128*1024)

    #ifdef X86_64

        /// Number of benchmark runs used when measuring the duration of the
        /// optimal loop using the x86 TimeStamp Counter
        ///
        /// Tune this up if the optimal loop calibration does not yield
        /// reproducible results.
        //
        // TODO: Tune on more systems
        #define NUM_RUNS_BEST_LOOP_X86 ((size_t)512)

        /// Warmup duration used for TSC clock offset calibration
        //
        // TODO: Tune on more systems
        #define WARMUP_OFFSET_X86 (1*UDIPE_MILLISECOND)

        /// Number of benchmark runs used for TSC clock offset calibration
        ///
        /// Tune this up if the TSC offset calibration does not yield
        /// reproducible results.
        //
        // TODO: Tune on more systems
        #define NUM_RUNS_OFFSET_X86 ((size_t)2*1024)

    #endif  // X86_64


    /// Comparison function for applying qsort() to int64_t[]
    static inline int compare_i64(const void* v1, const void* v2) {
        const int64_t* const d1 = (const int64_t*)v1;
        const int64_t* const d2 = (const int64_t*)v2;
        if (*d1 < *d2) return -1;
        if (*d1 > *d2) return 1;
        return 0;
    }


    outlier_filter_t
    outlier_filter_initialize(int64_t initial_window[OUTLIER_WINDOW]) {
        outlier_filter_t result = {
            .next_idx = 0
        };
        for (size_t i = 0; i < OUTLIER_WINDOW; ++i) {
            result.window[i] = initial_window[i];
        }
        outlier_filter_update_all(&result);
        return result;
    }

    UDIPE_NON_NULL_ARGS
    void outlier_filter_update_all(outlier_filter_t* filter) {
        filter->min = INT64_MAX;
        filter->min_count = 0;
        for (size_t i = 0; i < OUTLIER_WINDOW; ++i) {
            const int64_t value = filter->window[i];
            if (value < filter->min) {
                filter->min = value;
                filter->min_count = 1;
            } else if (value == filter->min) {
                ++(filter->min_count);
            }
        }
        assert(filter->min_count >= 1);
        debugf("Minimal input is %zd (%zu occurences).",
               filter->min, (size_t)filter->min_count);

        // Updating min invalidates max and upper_tolerance, which must be
        // recomputed as well
        if (outlier_filter_update_maxima(filter)) {
            outlier_filter_update_tolerance(filter);
        }
    }

    UDIPE_NON_NULL_ARGS
    bool outlier_filter_update_maxima(outlier_filter_t* filter) {
        // We first initialize max and max_normal by ignoring upper_tolerance:
        // an isolated maximum value in the dataset is always considered to be
        // an outlier, no matter how large or small it is.
        debug("Finding a pessimistic (max, max_normal) pair...");
        filter->max = filter->window[0];
        filter->max_normal = INT64_MIN;
        filter->max_normal_count = 0;
        tracef("Initialized max to first value %zd. "
               "max_normal will be set according to the next value.",
               filter->max);
        assert(OUTLIER_WINDOW >= 2);
        for (size_t i = 1; i < OUTLIER_WINDOW; ++i) {
            const int64_t value = filter->window[i];
            if (value > filter->max) {
                tracef("%zd is the new max, could be an outlier. "
                       "Use former max %zd as max_normal for now.",
                       value,
                       filter->max);
                filter->max_normal = filter->max;
                filter->max_normal_count = 1;
                filter->max = value;
            } else if (value == filter->max_normal) {
                tracef("Encountered one more occurence of max_normal %zd.",
                       value);
                ++(filter->max_normal_count);
            } else if (value == filter->max) {
                assert(filter->max > filter->max_normal);
                tracef("Encountered a second occurence of max %zd. "
                       "It is thus not an outlier and becomes max_normal.",
                       value);
                filter->max_normal = filter->max;
                filter->max_normal_count = 2;
            } else if (filter->max_normal_count == 0) {
                assert(value < filter->max);
                tracef("Initialized max_normal to %zd.", value);
                filter->max_normal = value;
                filter->max_normal_count = 1;
            }
        }
        assert(filter->max >= filter->max_normal);
        assert(filter->max_normal_count >= 1);
        debugf("Max input is %zd and max_normal is %zd (%zu occurences), "
               "unless we misclassified an isolated max as an outlier.",
               filter->max, filter->max_normal, (size_t)filter->max_normal_count);

        // At this point, if the max is not isolated, the result is correct...
        if (filter->max > filter->max_normal) {
            // ...but if there is an isolated max, it may have been
            // misclassified as an outlier. Compute upper_tolerance to tell.
            debugf("Reevaluating outlier status of isolated max %zd...",
                   filter->max);
            outlier_filter_update_tolerance(filter);
            if (filter->max <= filter->upper_tolerance) {
                debug("It's actually in tolerance, make it max_normal.");
                filter->max_normal = filter->max;
                filter->max_normal_count = 1;
                return true;
            } else {
                debug("It is indeed an outlier, nothing to do.");
                return false;
            }
        } else {
            return true;
        }
    }

    UDIPE_NON_NULL_ARGS
    void outlier_filter_update_tolerance(outlier_filter_t* filter) {
        filter->upper_tolerance =
            filter->max_normal
            + (filter->max_normal - filter->min) * OUTLIER_TOLERANCE;
        debugf("Updated outlier filter upper_tolerance to %zd.",
               filter->upper_tolerance);
    }

    UDIPE_NON_NULL_ARGS
    void outlier_filter_make_max_normal(outlier_filter_t* filter,
                                        outlier_filter_result_t* result,
                                        const char* reason) {
        assert(filter->max > filter->max_normal);
        debugf("Reclassified max %zd as non-outlier: %s.",
               filter->max, reason);
        result->previous_not_outlier = true;
        result->previous_input = filter->max;
        filter->max_normal = filter->max;
        filter->max_normal_count = 1;
    }

    UDIPE_NON_NULL_ARGS
    bool outlier_filter_decrease_min(outlier_filter_t* filter,
                                     outlier_filter_result_t* result,
                                     int64_t new_min) {
        const int64_t old_min = filter->min;
        assert(new_min < old_min);
        filter->min = new_min;
        filter->min_count = 1;
        outlier_filter_update_tolerance(filter);
        if (filter->max > filter->max_normal
            && filter->max <= filter->upper_tolerance)
        {
            outlier_filter_make_max_normal(
                filter,
                result,
                "tolerance window widened because min decreased"
            );
            return true;
        } else {
            return false;
        }
    }

    UDIPE_NON_NULL_ARGS
    bool outlier_filter_increase_max(outlier_filter_t* filter,
                                     outlier_filter_result_t* result,
                                     int64_t new_max) {
        assert(new_max > filter->max);
        if (filter->max > filter->max_normal) {
            outlier_filter_make_max_normal(
                filter,
                result,
                "encountered a larger input and there can only be one outlier"
            );
            outlier_filter_update_tolerance(filter);
        }
        filter->max = new_max;
        if (filter->max <= filter->upper_tolerance) {
            filter->max_normal = filter->max;
            filter->max_normal_count = 1;
            return true;
        } else {
            return false;
        }
    }

    UDIPE_NON_NULL_ARGS
    bool outlier_filter_increase_max_normal(outlier_filter_t* filter,
                                            outlier_filter_result_t* result,
                                            int64_t new_max_normal) {
        assert(new_max_normal > filter->max_normal);
        assert(new_max_normal < filter->max);
        filter->max_normal = new_max_normal;
        filter->max_normal_count = 1;
        outlier_filter_update_tolerance(filter);
        if (filter->max <= filter->upper_tolerance) {
            outlier_filter_make_max_normal(
                filter,
                result,
                "tolerance window widened because max_normal increased"
            );
            return true;
        } else {
            return false;
        }
    }

    UDIPE_NON_NULL_ARGS
    void outlier_filter_finalize(outlier_filter_t* filter) {
        for (size_t i = 0; i < OUTLIER_WINDOW; ++i) {
            filter->window[i] = INT64_MIN;
        }
        filter->min = INT64_MAX;
        filter->max_normal = 0;
        filter->max = INT64_MIN;
        filter->upper_tolerance = INT64_MIN;
        filter->next_idx = UINT16_MAX;
        filter->min_count = 0;
        filter->max_normal_count = 0;
    }


    /// Logical size of a bin from a \ref distribution_t
    ///
    /// \ref distribution_t internally uses a structure-of-array layout, so it
    /// is not literally an array of `(int64_t, size_t)` pairs but rather an
    /// array of `int64_t` followed by an array of `size_t`.
    const size_t distribution_bin_size = sizeof(int64_t) + sizeof(size_t);

    /// Allocate a distribution of duration-based values
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param capacity is the number of bins that the distribution should be
    ///                 able to hold internally before reallocating.
    ///
    /// \returns a distribution that must later be liberated using
    ///          distribution_finalize().
    static distribution_t distribution_allocate(size_t capacity) {
        void* const allocation = malloc(capacity * distribution_bin_size);
        exit_on_null(allocation, "Failed to allocate distribution storage");
        debugf("Allocated storage for %zu bins at location %p.",
               capacity, allocation);
        return (distribution_t){
            .allocation = allocation,
            .num_bins = 0,
            .capacity = capacity
        };
    }

    distribution_builder_t distribution_initialize() {
        const size_t capacity = get_page_size() / distribution_bin_size;
        return (distribution_builder_t){
            .inner = distribution_allocate(capacity)
        };
    }

    UDIPE_NON_NULL_ARGS
    void distribution_create_bin(distribution_builder_t* builder,
                                 size_t pos,
                                 int64_t value) {
        distribution_t* dist = &builder->inner;
        distribution_layout_t layout = distribution_layout(dist);
        if (dist->num_bins < dist->capacity) {
            trace("There's enough room in the allocation for this new bin.");
            const size_t end_pos = dist->num_bins;
            if (pos == end_pos) {
                trace("New bin is at the end of the histogram, can append it directly.");
                layout.sorted_values[end_pos] = value;
                layout.counts[end_pos] = 1;
                ++(dist->num_bins);
                return;
            }

            tracef("Backing up current bin at position %zu...", pos);
            int64_t next_value = layout.sorted_values[pos];
            size_t next_count = layout.counts[pos];

            trace("Inserting new value...");
            layout.sorted_values[pos] = value;
            layout.counts[pos] = 1;

            trace("Shifting previous bins up...");
            for (size_t dst = pos + 1; dst < dist->num_bins; ++dst) {
                int64_t tmp_value = layout.sorted_values[dst];
                size_t tmp_count = layout.counts[dst];
                layout.sorted_values[dst] = next_value;
                layout.counts[dst] = next_count;
                next_value = tmp_value;
                next_count = tmp_count;
            }

            trace("Restoring last bin...");
            layout.sorted_values[end_pos] = next_value;
            layout.counts[end_pos] = next_count;
            ++(dist->num_bins);
        } else {
            debug("No room for extra bins, must reallocate...");
            assert(dist->num_bins == dist->capacity);
            distribution_t new_dist = distribution_allocate(2 * dist->capacity);
            distribution_layout_t new_layout = distribution_layout(&new_dist);

            trace("Transferring old values smaller than the new one...");
            for (size_t bin = 0; bin < pos; ++bin) {
                new_layout.sorted_values[bin] = layout.sorted_values[bin];
                new_layout.counts[bin] = layout.counts[bin];
            }

            trace("Inserting new value...");
            new_layout.sorted_values[pos] = value;
            new_layout.counts[pos] = 1;

            trace("Transferring old values larger than the new one...");
            for (size_t src = pos; src < dist->num_bins; ++src) {
                const size_t dst = src + 1;
                new_layout.sorted_values[dst] = layout.sorted_values[src];
                new_layout.counts[dst] = layout.counts[src];
            }

            trace("Replacing former distribution...");
            new_dist.num_bins = dist->num_bins + 1;
            distribution_finalize(dist);
            builder->inner = new_dist;
        }
    }

    /// Mark a distribution as poisoned so it cannot be used anymore
    ///
    /// This is used when a distribution is either liberated or moved to a
    /// different variable, in order to ensure that incorrect
    /// user-after-free/move can be detected.
    static inline void distribution_poison(distribution_t* dist) {
        *dist = (distribution_t){
            .allocation = NULL,
            .num_bins = 0,
            .capacity = 0
        };
    }

    UDIPE_NON_NULL_ARGS
    distribution_t distribution_build(distribution_builder_t* builder) {
        trace("Extracting the distribution from the builder...");
        distribution_t dist = builder->inner;
        distribution_poison(&builder->inner);

        trace("Ensuring the distribution can be sampled...");
        ensure_ge(dist.num_bins, (size_t)1);

        distribution_layout_t layout = distribution_layout(&dist);
        if (log_enabled(UDIPE_DEBUG)) {
            debug("Final distribution is {");
            for (size_t bin = 0; bin < dist.num_bins; ++bin) {
                debugf("  %zd: %zu,",
                       layout.sorted_values[bin], layout.counts[bin]);
            }
            debug("}");
        }

        trace("Turning value counts into end indices...");
        size_t end_idx = 0;
        for (size_t bin = 0; bin < dist.num_bins; ++bin) {
            end_idx += layout.counts[bin];
            layout.end_indices[bin] = end_idx;
        }
        return dist;
    }

    UDIPE_NON_NULL_ARGS
    distribution_t distribution_sub(distribution_builder_t* builder,
                                    const distribution_t* left,
                                    const distribution_t* right) {
        // To avoid "amplifying" outliers by using multiple copies, we iterate
        // over the shortest distribution and sample from the longest one
        assert(builder->inner.num_bins == 0);
        const distribution_t* shorter;
        const distribution_t* longer;
        int64_t diff_sign;
        if (distribution_len(left) <= distribution_len(right)) {
            trace("Left distribution is shorter, will iterate over left and sample from right.");
            shorter = left;
            longer = right;
            diff_sign = +1;
        } else {
            trace("Right distribution is shorter, will iterate over right and sample from left.");
            shorter = right;
            longer = left;
            diff_sign = -1;
        }

        const distribution_layout_t short_layout = distribution_layout(shorter);
        const size_t short_bins = shorter->num_bins;
        tracef("Iterating over the %zu bins of the shorter distribution...",
               short_bins);
        size_t prev_short_end_idx = 0;
        for (size_t short_pos = 0; short_pos < short_bins; ++short_pos) {
            const int64_t short_value = short_layout.sorted_values[short_pos];
            const size_t short_end_idx = short_layout.end_indices[short_pos];
            const size_t short_count = short_end_idx - prev_short_end_idx;
            tracef("- Bin #%zu contains %zu occurences of value %zd.",
                   short_pos, short_count, short_value);
            for (size_t long_sample = 0; long_sample < short_count; ++long_sample) {
                const int64_t diff = short_value - distribution_sample(longer);
                tracef("  * Random short-long difference is %zd.", diff);
                const int64_t signed_diff = diff_sign * diff;
                tracef("  * Random left-right difference is %zd.", signed_diff);
                distribution_insert(builder, signed_diff);
            }
            prev_short_end_idx = short_end_idx;
        }
        return distribution_build(builder);
    }

    UDIPE_NON_NULL_ARGS
    distribution_t distribution_scaled_div(distribution_builder_t* builder,
                                           const distribution_t* num,
                                           int64_t factor,
                                           const distribution_t* denom) {
        // To avoid "amplifying" outliers by using multiple copies, we iterate
        // over the shortest distribution and sample from the longest one
        assert(builder->inner.num_bins == 0);
        if (distribution_len(num) <= distribution_len(num)) {
            trace("Numerator distribution is shorter, will iterate over num and sample from denom.");
            const distribution_layout_t num_layout = distribution_layout(num);
            const size_t num_bins = num->num_bins;
            tracef("Iterating over the %zu bins of the numerator distribution...",
                   num_bins);
            size_t prev_end_idx = 0;
            for (size_t num_pos = 0; num_pos < num_bins; ++num_pos) {
                const int64_t num_value = num_layout.sorted_values[num_pos];
                const size_t curr_end_idx = num_layout.end_indices[num_pos];
                const size_t num_count = curr_end_idx - prev_end_idx;
                tracef("- Numerator bin #%zu contains %zu occurences of value %zd.",
                       num_pos, num_count, num_value);
                for (size_t denom_sample = 0; denom_sample < num_count; ++denom_sample) {
                    const int64_t denom_value = distribution_sample(denom);
                    tracef("  * Sampled random denominator value %zd.", denom_value);
                    const int64_t scaled_ratio = num_value * factor / denom_value;
                    tracef("  * Scaled ratio sample is %zd.", scaled_ratio);
                    distribution_insert(builder, scaled_ratio);
                }
                prev_end_idx = curr_end_idx;
            }
            return distribution_build(builder);
        } else {
            trace("Denominator distribution is shorter, will iterate over denom and sample from num.");
            const distribution_layout_t denom_layout = distribution_layout(denom);
            const size_t denom_bins = denom->num_bins;
            tracef("Iterating over the %zu bins of the denominator distribution...",
                   denom_bins);
            size_t prev_end_idx = 0;
            for (size_t denom_pos = 0; denom_pos < denom_bins; ++denom_pos) {
                const int64_t denom_value = denom_layout.sorted_values[denom_pos];
                const size_t curr_end_idx = denom_layout.end_indices[denom_pos];
                const size_t denom_count = curr_end_idx - prev_end_idx;
                tracef("- Denominator bin #%zu contains %zu occurences of value %zd.",
                       denom_pos, denom_count, denom_value);
                for (size_t num_sample = 0; num_sample < denom_count; ++num_sample) {
                    const int64_t num_value = distribution_sample(num);
                    tracef("  * Sampled random numerator value %zd.", num_value);
                    const int64_t scaled_ratio = num_value * factor / denom_value;
                    tracef("  * Scaled ratio sample is %zd.", scaled_ratio);
                    distribution_insert(builder, scaled_ratio);
                }
                prev_end_idx = curr_end_idx;
            }
            return distribution_build(builder);
        }
    }

    UDIPE_NON_NULL_ARGS
    distribution_builder_t distribution_reset(distribution_t* dist) {
        tracef("Resetting storage at location %p...", dist->allocation);
        distribution_builder_t result = (distribution_builder_t){
            .inner = (distribution_t){
                .allocation = dist->allocation,
                .num_bins = 0,
                .capacity = dist->capacity
            }
        };

        trace("Poisoning distribution state to detect invalid usage...");
        distribution_poison(dist);
        return result;
    }

    UDIPE_NON_NULL_ARGS
    void distribution_finalize(distribution_t* dist) {
        debugf("Liberating storage at location %p...", dist->allocation);
        free(dist->allocation);

        trace("Poisoning distribution state to detect invalid usage...");
        distribution_poison(dist);
    }

    stats_analyzer_t stats_analyzer_initialize(float confidence_f) {
        debug("Checking analysis parameters...");
        double confidence = (double)confidence_f;
        ensure_gt(confidence, 0.0);
        ensure_lt(confidence, 100.0);

        debug("Determining storage needs...");
        size_t num_medians = ceil((double)(2*NUM_EDGE_MEASUREMENTS)
                                  / (1.0-0.01*confidence));
        if ((num_medians % 2) == 0) num_medians += 1;

        debug("Allocating storage...");
        size_t medians_size = num_medians * sizeof(int64_t);
        int64_t* medians = realtime_allocate(medians_size);

        debug("Finishing setup...");
        double low_quantile = (1.0 - 0.01*confidence) / 2.0;
        double high_quantile = 1.0 - low_quantile;
        return (stats_analyzer_t){
            .medians = medians,
            .num_medians = num_medians,
            .low_idx = (size_t)(low_quantile * num_medians),
            .center_idx = num_medians / 2,
            .high_idx = (size_t)(high_quantile * num_medians)
        };
    }

    UDIPE_NON_NULL_ARGS
    stats_t stats_analyze(stats_analyzer_t* analyzer,
                          const distribution_t* dist) {
        trace("Computing medians...");
        int64_t median_samples[NUM_MEDIAN_SAMPLES];
        for (size_t median = 0; median < analyzer->num_medians; ++median) {
            tracef("- Computing medians[%zu]...", median);
            for (size_t sample = 0; sample < NUM_MEDIAN_SAMPLES; ++sample) {
                const int64_t value = distribution_sample(dist);
                tracef("  * Inserting sample %zd...", value);
                ptrdiff_t prev;
                for (prev = sample - 1; prev >= 0; --prev) {
                    int64_t pivot = median_samples[prev];
                    tracef("    - Checking median_samples[%zd] = %zd...",
                           prev, pivot);
                    if (pivot > value) {
                        trace("    - Too high, shift that up to make room.");
                        median_samples[prev + 1] = median_samples[prev];
                        continue;
                    } else {
                        trace("    - Small enough, value goes after that.");
                        break;
                    }
                }
                tracef("  * Sample inserted at median_samples[%zd].", prev + 1);
                median_samples[prev + 1] = value;
            }
            analyzer->medians[median] = median_samples[NUM_MEDIAN_SAMPLES / 2];
            tracef("  * medians[%zu] is therefore %zd.",
                   median, analyzer->medians[median]);
        }

        trace("Computing result...");
        qsort(analyzer->medians,
              analyzer->num_medians,
              sizeof(int64_t),
              compare_i64);
        return (stats_t){
            .center = analyzer->medians[analyzer->center_idx],
            .low = analyzer->medians[analyzer->low_idx],
            .high = analyzer->medians[analyzer->high_idx]
        };
    }

    UDIPE_NON_NULL_ARGS
    void stats_analyzer_finalize(stats_analyzer_t* analyzer) {
        debug("Liberating storage...");
        realtime_liberate(analyzer->medians,
                          analyzer->num_medians * sizeof(int64_t));

        debug("Poisoining analyzer state...");
        analyzer->medians = NULL;
        analyzer->num_medians = 0;
        analyzer->center_idx = SIZE_MAX;
        analyzer->low_idx = SIZE_MAX;
        analyzer->high_idx = SIZE_MAX;
    }


    /// Empty benchmark workload
    ///
    /// Used to measure the offset of the operating system clock.
    static inline void nothing(void* /* context */) {}

    /// Empty-loop benchmark workload
    ///
    /// Used to measure the maximal precision of a clock and the maximal
    /// benchmark duration before OS interrupts start hurting clock precision.
    ///
    /// \param context must be a `const size_t*` indicating the desired amount
    ///                of loop iterations.
    UDIPE_NON_NULL_ARGS
    static inline void empty_loop(void* context) {
        size_t num_iters = *((const size_t*)context);
        // Ensures that all loop lengths get the same codegen
        UDIPE_ASSUME_ACCESSED(num_iters);
        for (size_t iter = 0; iter < num_iters; ++iter) {
            UDIPE_ASSUME_READ(iter);
        }
    }

    /// Log statistics from the calibration process
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param level is the verbosity level at which this log will be emitted
    /// \param header is a string that will be prepended to the log
    /// \param stats is \ref stats_t from the calibration process that will
    ///              be printed out
    /// \param unit is a string that spells out the measurement unit of
    ///             `stats`
    #define log_calibration_stats(level, header, stats, unit)  \
        do {  \
            stats_t udipe_duration = (stats);  \
            udipe_logf((level),  \
                       "%s: %zd %s with %g%% CI [%zd; %zd].",  \
                       (header),  \
                       udipe_duration.center,  \
                       (unit),  \
                       CONFIDENCE,  \
                       udipe_duration.low,  \
                       udipe_duration.high);  \
        } while(false)

    /// Compute the relative uncertainty from some \ref stats_t
    ///
    /// \param stats is \ref stats_t that directly or indirectly derive from
    ///              some measurements.
    /// \returns the associated statistical uncertainty in percentage points
    static inline double relative_uncertainty(stats_t stats) {
        return (double)(stats.high - stats.low) / stats.center * 100.0;
    }

    /// Log per-iteration statistics from the calibration process
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param level is the verbosity level at which this log will be emitted
    /// \param stats is \ref stats_t from the calibration process that will
    ///              be printed out
    /// \param unit is a string that spells out the measurement unit of
    ///             `stats`
    #define log_iteration_stats(level, bullet, stats, num_iters, unit)  \
        do {  \
            const stats_t udipe_stats = (stats);  \
            const size_t udipe_num_iters = (num_iters);  \
            const double udipe_center = (double)udipe_stats.center / udipe_num_iters;  \
            const double udipe_low = (double)udipe_stats.low / udipe_num_iters;  \
            const double udipe_high = (double)udipe_stats.high / udipe_num_iters;  \
            const double udipe_spread = udipe_high - udipe_low;  \
            const int udipe_stats_decimals = ceil(-log10(udipe_spread));  \
            const double udipe_uncertainty = relative_uncertainty(udipe_stats);  \
            int udipe_uncertainty_decimals = ceil(-log10(udipe_uncertainty));  \
            if (udipe_uncertainty_decimals < 0) udipe_uncertainty_decimals = 0;  \
            udipe_logf((level),  \
                       "%s That's %.*f %s/iter with %g%% CI [%.*f; %.*f] (%.*f%% uncertainty).",  \
                       (bullet),  \
                       udipe_stats_decimals,  \
                       udipe_center,  \
                       (unit),  \
                       CONFIDENCE,  \
                       udipe_stats_decimals,  \
                       udipe_low,  \
                       udipe_stats_decimals,  \
                       udipe_high,  \
                       udipe_uncertainty_decimals,  \
                       udipe_uncertainty);  \
        } while(false)

    UDIPE_NON_NULL_ARGS
    os_clock_t os_clock_initialize(stats_analyzer_t* analyzer) {
        // Zero out all clock fields initially
        //
        // This is a valid (if incorrect) value for some fields but not all of
        // them. We will take care of the missing fields later on.
        os_clock_t clock = { 0 };

        #ifdef _WIN32
            debug("Obtaining Windows performance counter frequency...");
            clock.win32_frequency = QueryPerformanceFrequency().QuadPart;
        #endif

        debug("Allocating timestamp buffer and duration distribution...");
        size_t max_runs = NUM_RUNS_OFFSET_OS;
        if (max_runs < NUM_RUNS_SHORTEST_LOOP) max_runs = NUM_RUNS_SHORTEST_LOOP;
        if (max_runs < NUM_RUNS_BEST_LOOP_OS) max_runs = NUM_RUNS_BEST_LOOP_OS;
        const size_t timestamps_size = (max_runs+1) * sizeof(os_timestamp_t);
        clock.timestamps = realtime_allocate(timestamps_size);
        clock.num_durations = max_runs;
        clock.builder = distribution_initialize();

        info("Bootstrapping clock offset to 0 ns...");
        distribution_insert(&clock.builder, 0);
        clock.offsets = distribution_build(&clock.builder);
        clock.builder = distribution_initialize();

        info("Measuring actual clock offset...");
        distribution_t tmp_offsets = os_clock_measure(
            &clock,
            nothing,
            NULL,
            WARMUP_OFFSET_OS,
            NUM_RUNS_OFFSET_OS,
            &clock.builder,
            analyzer
        );
        clock.builder = distribution_reset(&clock.offsets);
        clock.offsets = tmp_offsets;
        distribution_poison(&tmp_offsets);
        const stats_t offset_stats = stats_analyze(analyzer, &clock.offsets);
        log_calibration_stats(UDIPE_INFO, "- Clock offset", offset_stats, "ns");

        info("Deducing clock baseline...");
        distribution_t tmp_zeros = distribution_sub(&clock.builder,
                                                    &clock.offsets,
                                                    &clock.offsets);
        const stats_t zero_stats = stats_analyze(analyzer, &tmp_zeros);
        clock.builder = distribution_reset(&tmp_zeros);
        log_calibration_stats(UDIPE_INFO,
                              "- Baseline",
                              zero_stats,
                              "ns");

        info("Finding minimal measurable loop...");
        distribution_t loop_durations;
        stats_t loop_duration_stats;
        size_t num_iters = 1;
        do {
            debugf("- Trying loop with %zu iteration(s)...", num_iters);
            loop_durations = os_clock_measure(
                &clock,
                empty_loop,
                &num_iters,
                WARMUP_SHORTEST_LOOP,
                NUM_RUNS_SHORTEST_LOOP,
                &clock.builder,
                analyzer
            );
            loop_duration_stats = stats_analyze(analyzer, &loop_durations);
            log_calibration_stats(UDIPE_DEBUG,
                                  "  * Loop duration",
                                  loop_duration_stats,
                                  "ns");
            if (loop_duration_stats.low > 5*offset_stats.high) {
                debug("  * Loop finally contributes much more than clock offset!");
                clock.builder = distribution_initialize();
                break;
            } else {
                debug("  * Measured duration is close to clock offset, try a longer loop...");
                num_iters *= 2;
                clock.builder = distribution_reset(&loop_durations);
                continue;
            }
        } while(true);
        infof("- Loops with >=%zu iterations have non-negligible duration.",
              num_iters);

        info("Finding optimal loop duration...");
        clock.best_empty_iters = num_iters;
        clock.best_empty_durations = loop_durations;
        distribution_poison(&loop_durations);
        clock.best_empty_stats = loop_duration_stats;
        const int64_t best_precision = zero_stats.high - zero_stats.low;
        double best_uncertainty = relative_uncertainty(loop_duration_stats);
        do {
            num_iters *= 2;
            debugf("- Trying loop with %zu iterations...", num_iters);
            loop_durations = os_clock_measure(
                &clock,
                empty_loop,
                &num_iters,
                WARMUP_BEST_LOOP,
                NUM_RUNS_BEST_LOOP_OS,
                &clock.builder,
                analyzer
            );
            loop_duration_stats = stats_analyze(analyzer, &loop_durations);
            log_calibration_stats(UDIPE_DEBUG,
                                  "  * Loop duration",
                                  loop_duration_stats,
                                  "ns");
            log_iteration_stats(UDIPE_DEBUG,
                                "  *",
                                loop_duration_stats,
                                num_iters,
                                "ns");
            const double uncertainty = relative_uncertainty(loop_duration_stats);
            const int64_t precision = loop_duration_stats.high - loop_duration_stats.low;
            if (uncertainty < best_uncertainty) {
                debug("  * This is our new best loop. Can we do even better?");
                best_uncertainty = uncertainty;
                clock.best_empty_iters = num_iters;
                clock.builder = distribution_reset(&clock.best_empty_durations);
                clock.best_empty_durations = loop_durations;
                distribution_poison(&loop_durations);
                clock.best_empty_stats = loop_duration_stats;
                continue;
            } else if (precision <= 2*best_precision) {
                debug("  * That's not much worse. Keep trying...");
                clock.builder = distribution_reset(&loop_durations);
                continue;
            } else {
                debug("  * Timing precision degraded by >2x. Time to stop!");
                clock.builder = distribution_reset(&loop_durations);
                break;
            }
        } while(true);
        infof("- Achieved optimal precision at %zu loop iterations.",
              clock.best_empty_iters);
        log_calibration_stats(UDIPE_INFO,
                              "- Best loop duration",
                              clock.best_empty_stats,
                              "ns");
        log_iteration_stats(UDIPE_INFO,
                            "-",
                            clock.best_empty_stats,
                            clock.best_empty_iters,
                            "ns");
        return clock;
    }

    UDIPE_NON_NULL_ARGS
    void os_clock_finalize(os_clock_t* clock) {
        debug("Liberating and poisoning timestamp storage...");
        realtime_liberate(clock->timestamps,
                          (clock->num_durations+1) * sizeof(os_timestamp_t));
        clock->timestamps = NULL;
        clock->num_durations = 0;

        debug("Destroying duration distributions...");
        distribution_finalize(&clock->offsets);
        distribution_finalize(&clock->best_empty_durations);
        distribution_finalize(&clock->builder.inner);

        debug("Poisoning the rest of the OS clock...");
        #ifdef _WIN32
            clock->win32_frequency = 0;
        #endif
        clock->best_empty_iters = SIZE_MAX;
        clock->best_empty_stats = (stats_t){
            .low = INT64_MIN,
            .center = INT64_MIN,
            .high = INT64_MIN
        };
    }


    #ifdef X86_64

        UDIPE_NON_NULL_ARGS
        x86_clock_t
        x86_clock_initialize(os_clock_t* os,
                             stats_analyzer_t* analyzer) {
            // Zero out all clock fields initially
            //
            // This is a valid (if incorrect) value for some fields but not all
            // of them. We will take care of the missing fields later on.
            x86_clock_t clock = { 0 };

            debug("Allocating timestamp and duration distribution...");
            size_t max_runs = NUM_RUNS_BEST_LOOP_X86;
            if (max_runs < NUM_RUNS_OFFSET_X86) max_runs = NUM_RUNS_OFFSET_X86;
            const size_t instants_size = 2 * max_runs * sizeof(x86_instant);
            clock.instants = realtime_allocate(instants_size);
            clock.num_durations = max_runs;
            distribution_builder_t builder = distribution_initialize();

            info("Bootstrapping clock offset to 0 ticks...");
            distribution_insert(&builder, 0);
            clock.offsets = distribution_build(&builder);
            builder = distribution_initialize();

            // This should happen as soon as possible to reduce the risk of CPU
            // clock frequency changes, which would degrade the quality of our
            // TSC frequency calibration
            //
            // TODO: Investigate paired benchmarking techniques as a more robust
            //       alternative to reducing the delay between these two
            //       measurements. The general idea is to alternatively measure
            //       durations with the OS and TSC clocks, use pairs of raw
            //       duration data points from each clock to compute frequency
            //       samples, and compute statistics over these frequency
            //       samples. This way we are using data that was acquired in
            //       similar system configurations, so even if the system
            //       configuration changes over time, the results remain stable.
            info("Measuring optimal loop again with the TSC...");
            size_t best_empty_iters = os->best_empty_iters;
            distribution_t raw_empty_ticks = x86_clock_measure(
                &clock,
                empty_loop,
                &best_empty_iters,
                WARMUP_BEST_LOOP,
                NUM_RUNS_BEST_LOOP_X86,
                &builder,
                analyzer
            );
            log_calibration_stats(UDIPE_INFO,
                                  "- Offset-biased best loop",
                                  stats_analyze(analyzer, &raw_empty_ticks),
                                  "ticks");

            info("Measuring clock offset...");
            builder = distribution_initialize();
            distribution_t tmp_offsets = x86_clock_measure(
                &clock,
                nothing,
                NULL,
                WARMUP_OFFSET_X86,
                NUM_RUNS_OFFSET_X86,
                &builder,
                analyzer
            );
            builder = distribution_reset(&clock.offsets);
            clock.offsets = tmp_offsets;
            distribution_poison(&tmp_offsets);
            log_calibration_stats(UDIPE_INFO,
                                  "- Clock offset",
                                  stats_analyze(analyzer, &clock.offsets),
                                  "ticks");

            info("Deducing clock baseline...");
            distribution_t tmp_zeros = distribution_sub(&builder,
                                                        &clock.offsets,
                                                        &clock.offsets);
            const stats_t zero_stats = stats_analyze(analyzer, &tmp_zeros);
            builder = distribution_reset(&tmp_zeros);
            log_calibration_stats(UDIPE_INFO,
                                  "- Baseline",
                                  zero_stats,
                                  "ticks");

            debug("Applying offset correction to best loop duration...");
            distribution_t corrected_empty_ticks = distribution_sub(
                &builder,
                &raw_empty_ticks,
                &clock.offsets
            );
            builder = distribution_reset(&raw_empty_ticks);
            clock.best_empty_stats = stats_analyze(analyzer,
                                                   &corrected_empty_ticks);
            log_calibration_stats(UDIPE_DEBUG,
                                  "- Offset-corrected best loop",
                                  clock.best_empty_stats,
                                  "ticks");
            log_iteration_stats(UDIPE_DEBUG,
                                "-",
                                clock.best_empty_stats,
                                os->best_empty_iters,
                                "ticks");

            info("Deducing TSC tick frequency...");
            clock.frequencies = distribution_scaled_div(
                &builder,
                &corrected_empty_ticks,
                UDIPE_SECOND,
                &os->best_empty_durations
            );
            // `builder` cannot be used after this point
            log_calibration_stats(UDIPE_INFO,
                                  "- TSC frequency",
                                  stats_analyze(analyzer, &clock.frequencies),
                                  "ticks/sec");

            debug("Deducing best loop duration...");
            const stats_t best_empty_duration = x86_duration(
                &clock,
                &os->builder,
                &corrected_empty_ticks,
                analyzer
            );
            log_calibration_stats(UDIPE_DEBUG,
                                  "- Best loop duration",
                                  best_empty_duration,
                                  "ns");
            log_iteration_stats(UDIPE_DEBUG,
                                "-",
                                best_empty_duration,
                                os->best_empty_iters,
                                "ns");
            return clock;
        }

        UDIPE_NON_NULL_ARGS
        stats_t x86_duration(x86_clock_t* clock,
                             distribution_builder_t* tmp_builder,
                             const distribution_t* ticks,
                             stats_analyzer_t* analyzer) {
            distribution_t tmp_durations =
                distribution_scaled_div(tmp_builder,
                                        ticks,
                                        UDIPE_SECOND,
                                        &clock->frequencies);
            const stats_t result = stats_analyze(analyzer, &tmp_durations);
            *tmp_builder = distribution_reset(&tmp_durations);
            return result;
        }

        UDIPE_NON_NULL_ARGS
        void x86_clock_finalize(x86_clock_t* clock) {
            debug("Liberating and poisoning timestamp storage...");
            realtime_liberate(clock->instants,
                              2 * clock->num_durations * sizeof(x86_instant));
            clock->instants = NULL;
            clock->num_durations = 0;

            debug("Destroying offset and frequency distributions...");
            distribution_finalize(&clock->offsets);
            distribution_finalize(&clock->frequencies);

            debug("Poisoning the now-invalid TSC clock...");
            clock->best_empty_stats = (stats_t){
                .low = INT64_MIN,
                .center = INT64_MIN,
                .high = INT64_MIN
            };
        }

    #endif  // X86_64


    benchmark_clock_t benchmark_clock_initialize() {
        // Zero out all clock fields initially
        //
        // This is a valid (if incorrect) value for some fields but not all of
        // them. We will take care of the missing fields later on.
        benchmark_clock_t clock = { 0 };

        debug("Setting up statistical analysis...");
        clock.analyzer = stats_analyzer_initialize(CONFIDENCE);

        info("Setting up the OS clock...");
        clock.os = os_clock_initialize(&clock.analyzer);

        #ifdef X86_64
            info("Setting up the TSC clock...");
            clock.x86 = x86_clock_initialize(&clock.os, &clock.analyzer);
        #endif
        return clock;
    }

    UDIPE_NON_NULL_ARGS
    void benchmark_clock_recalibrate(benchmark_clock_t* clock) {
        // TODO: Check if clock calibration still seems correct, recalibrate if
        //       needed.
        // TODO: This should probably be implemented by implementing recalibrate
        //       for the x86 and OS clocks, then calling these here.
        error("Not implemented yet!");
        exit(EXIT_FAILURE);
    }

    UDIPE_NON_NULL_ARGS
    void benchmark_clock_finalize(benchmark_clock_t* clock) {
        debug("Liberating the statistical analyzer...");
        stats_analyzer_finalize(&clock->analyzer);

        #ifdef X86_64
            debug("Liberating the TSC clock...");
            x86_clock_finalize(&clock->x86);
        #endif

        debug("Liberating the OS clock...");
        os_clock_finalize(&clock->os);
    }


    DEFINE_PUBLIC
    UDIPE_NON_NULL_ARGS
    UDIPE_NON_NULL_RESULT
    udipe_benchmark_t* udipe_benchmark_initialize(int argc, char *argv[]) {
        // Set up logging
        logger_t logger = logger_initialize((udipe_log_config_t){ 0 });
        udipe_benchmark_t* benchmark;
        with_logger(&logger, {
            debug("Setting up benchmark harness...");
            benchmark =
                (udipe_benchmark_t*)realtime_allocate(sizeof(udipe_benchmark_t));
            memset(benchmark, 0, sizeof(udipe_benchmark_t));
            benchmark->logger = logger;

            // Warn about bad build/runtime configurations
            #ifndef NDEBUG
                warn("You are running micro-benchmarks on a Debug build. "
                     "This will bias measurements!");
            #else
                if (benchmark->logger.min_level <= UDIPE_DEBUG) {
                    warn("You are running micro-benchmarks with DEBUG/TRACE "
                         "logging enabled. This will bias measurements!");
                }
            #endif

            debug("Setting up benchmark name filter...");
            ensure_le(argc, 2);
            const char* filter_key = (argc == 2) ? argv[1] : "";
            benchmark->filter = name_filter_initialize(filter_key);

            debug("Setting up the hwloc topology...");
            exit_on_negative(hwloc_topology_init(&benchmark->topology),
                             "Failed to allocate the hwloc hopology!");
            exit_on_negative(hwloc_topology_load(benchmark->topology),
                             "Failed to build the hwloc hopology!");

            debug("Pinning the benchmark timing thread...");
            benchmark->timing_cpuset = hwloc_bitmap_alloc();
            exit_on_null(benchmark->timing_cpuset,
                         "Failed to allocate timing thread cpuset");
            exit_on_negative(
                hwloc_get_last_cpu_location(benchmark->topology,
                                            benchmark->timing_cpuset,
                                            HWLOC_CPUBIND_THREAD),
                "Failed to query timing thread cpuset"
            );
            exit_on_negative(hwloc_set_cpubind(benchmark->topology,
                                               benchmark->timing_cpuset,
                                               HWLOC_CPUBIND_THREAD
                                               | HWLOC_CPUBIND_STRICT),
                             "Failed to pin the timing thread");

            // Set up the benchmark clock
            benchmark->clock = benchmark_clock_initialize();
        });
        return benchmark;
    }

    DEFINE_PUBLIC
    UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 3)
    bool udipe_benchmark_run(udipe_benchmark_t* benchmark,
                             const char* name,
                             udipe_benchmark_runnable_t runnable,
                             void* context) {
        bool name_matches;
        with_logger(&benchmark->logger, {
            name_matches = name_filter_matches(benchmark->filter, name);
            if (name_matches) {
                trace("Pinning the benchmark timing thread...");
                exit_on_negative(hwloc_set_cpubind(benchmark->topology,
                                                   benchmark->timing_cpuset,
                                                   HWLOC_CPUBIND_THREAD
                                                   | HWLOC_CPUBIND_STRICT),
                                 "Failed to pin benchmark timing thread");

                tracef("Running benchmark \"%s\"...", name);
                runnable(context, benchmark);

                trace("Recalibrating benchmark clock...");
                benchmark_clock_recalibrate(&benchmark->clock);
            }
        });
        return name_matches;
    }

    DEFINE_PUBLIC
    UDIPE_NON_NULL_ARGS
    void udipe_benchmark_finalize(udipe_benchmark_t** benchmark) {
        logger_t logger = (*benchmark)->logger;
        with_logger(&logger, {
            info("All benchmarks executed successfully!");

            debug("Finalizing the benchmark clock...");
            benchmark_clock_finalize(&(*benchmark)->clock);

            debug("Freeing and poisoning the timing thread cpuset...");
            hwloc_bitmap_free((*benchmark)->timing_cpuset);
            (*benchmark)->timing_cpuset = NULL;

            debug("Destroying and poisoning the hwloc topology...");
            hwloc_topology_destroy((*benchmark)->topology);
            (*benchmark)->topology = NULL;

            debug("Finalizing the benchmark name filter...");
            name_filter_finalize(&(*benchmark)->filter);

            debug("Liberating and poisoning the benchmark...");
            realtime_liberate(*benchmark, sizeof(udipe_benchmark_t));
            *benchmark = NULL;

            debug("Finalizing the logger...");
        });
        logger_finalize(&logger);
    }


    DEFINE_PUBLIC void udipe_micro_benchmarks(udipe_benchmark_t* benchmark) {
        // Microbenchmarks are ordered such that a piece of code is
        // benchmarked before other pieces of code that may depend on it
        // TODO: UDIPE_BENCHMARK(benchmark, xyz_micro_benchmarks, NULL);
    }


    #ifdef UDIPE_BUILD_TESTS

        static void test_distibution() {
            trace("Setting up a distribution...");
            distribution_builder_t builder = distribution_initialize();
            const void* const initial_allocation = builder.inner.allocation;
            const size_t initial_capacity = builder.inner.capacity;
            ensure_ne(initial_allocation, NULL);
            ensure_eq(builder.inner.num_bins, (size_t)0);
            ensure_ge(initial_capacity, (size_t)5);

            trace("Checking initial layout");
            const distribution_layout_t initial_layout =
                distribution_layout(&builder.inner);
            ensure_ne((void*)initial_layout.sorted_values, NULL);
            ensure_ne((void*)initial_layout.counts, NULL);
            const size_t values_size =
                (char*)initial_layout.counts
                    - (char*)initial_layout.sorted_values;
            ensure_eq(values_size, initial_capacity * sizeof(int64_t));

            assert(RAND_MAX <= INT64_MAX);
            const int64_t value3 = rand() - RAND_MAX / 2;
            tracef("Inserting value3 = %zd for the first time...", value3);
            distribution_insert(&builder, value3);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)1);
            ensure_eq(builder.inner.capacity, initial_capacity);
            distribution_layout_t layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value3);
            ensure_eq(layout.counts[0], (size_t)1);

            trace("Inserting value3 again...");
            distribution_insert(&builder, value3);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)1);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value3);
            ensure_eq(layout.counts[0], (size_t)2);

            const int64_t value5 = value3 + 2 + rand() % (INT64_MAX - value3 - 1);
            tracef("Inserting value5 = %zd for the first time...", value5);
            distribution_insert(&builder, value5);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)2);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value3);
            ensure_eq(layout.counts[0], (size_t)2);
            ensure_eq(layout.sorted_values[1], value5);
            ensure_eq(layout.counts[1], (size_t)1);

            trace("Inserting value5 again two times...");
            distribution_insert(&builder, value5);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)2);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value3);
            ensure_eq(layout.counts[0], (size_t)2);
            ensure_eq(layout.sorted_values[1], value5);
            ensure_eq(layout.counts[1], (size_t)2);
            //
            distribution_insert(&builder, value5);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)2);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value3);
            ensure_eq(layout.counts[0], (size_t)2);
            ensure_eq(layout.sorted_values[1], value5);
            ensure_eq(layout.counts[1], (size_t)3);

            const int64_t value1 = value3 - 2 - rand() % (value3 - 1 - INT64_MIN);
            tracef("Inserting value1 = %zd for the first time...", value1);
            distribution_insert(&builder, value1);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)3);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)1);
            ensure_eq(layout.sorted_values[1], value3);
            ensure_eq(layout.counts[1], (size_t)2);
            ensure_eq(layout.sorted_values[2], value5);
            ensure_eq(layout.counts[2], (size_t)3);

            trace("Inserting value1 again three times...");
            distribution_insert(&builder, value1);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)3);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)2);
            ensure_eq(layout.sorted_values[1], value3);
            ensure_eq(layout.counts[1], (size_t)2);
            ensure_eq(layout.sorted_values[2], value5);
            ensure_eq(layout.counts[2], (size_t)3);
            //
            distribution_insert(&builder, value1);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)3);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)3);
            ensure_eq(layout.sorted_values[1], value3);
            ensure_eq(layout.counts[1], (size_t)2);
            ensure_eq(layout.sorted_values[2], value5);
            ensure_eq(layout.counts[2], (size_t)3);
            //
            distribution_insert(&builder, value1);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)3);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value3);
            ensure_eq(layout.counts[1], (size_t)2);
            ensure_eq(layout.sorted_values[2], value5);
            ensure_eq(layout.counts[2], (size_t)3);

            const int64_t value2 = value1 + 1 + rand() % (value3 - value1 - 1);
            tracef("Inserting value2 = %zd for the first time...", value2);
            distribution_insert(&builder, value2);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)4);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts,
                      (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value2);
            ensure_eq(layout.counts[1], (size_t)1);
            ensure_eq(layout.sorted_values[2], value3);
            ensure_eq(layout.counts[2], (size_t)2);
            ensure_eq(layout.sorted_values[3], value5);
            ensure_eq(layout.counts[3], (size_t)3);

            trace("Inserting value2 again two times...");
            distribution_insert(&builder, value2);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)4);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts,
                      (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value2);
            ensure_eq(layout.counts[1], (size_t)2);
            ensure_eq(layout.sorted_values[2], value3);
            ensure_eq(layout.counts[2], (size_t)2);
            ensure_eq(layout.sorted_values[3], value5);
            ensure_eq(layout.counts[3], (size_t)3);
            //
            distribution_insert(&builder, value2);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)4);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value2);
            ensure_eq(layout.counts[1], (size_t)3);
            ensure_eq(layout.sorted_values[2], value3);
            ensure_eq(layout.counts[2], (size_t)2);
            ensure_eq(layout.sorted_values[3], value5);
            ensure_eq(layout.counts[3], (size_t)3);

            const int64_t value4 = value3 + 1 + rand() % (value5 - value3 - 1);
            tracef("Inserting value4 = %zd for the first time...", value4);
            distribution_insert(&builder, value4);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)5);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value2);
            ensure_eq(layout.counts[1], (size_t)3);
            ensure_eq(layout.sorted_values[2], value3);
            ensure_eq(layout.counts[2], (size_t)2);
            ensure_eq(layout.sorted_values[3], value4);
            ensure_eq(layout.counts[3], (size_t)1);
            ensure_eq(layout.sorted_values[4], value5);
            ensure_eq(layout.counts[4], (size_t)3);

            trace("Inserting value4 again three times...");
            distribution_insert(&builder, value4);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)5);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value2);
            ensure_eq(layout.counts[1], (size_t)3);
            ensure_eq(layout.sorted_values[2], value3);
            ensure_eq(layout.counts[2], (size_t)2);
            ensure_eq(layout.sorted_values[3], value4);
            ensure_eq(layout.counts[3], (size_t)2);
            ensure_eq(layout.sorted_values[4], value5);
            ensure_eq(layout.counts[4], (size_t)3);
            //
            distribution_insert(&builder, value4);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)5);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value2);
            ensure_eq(layout.counts[1], (size_t)3);
            ensure_eq(layout.sorted_values[2], value3);
            ensure_eq(layout.counts[2], (size_t)2);
            ensure_eq(layout.sorted_values[3], value4);
            ensure_eq(layout.counts[3], (size_t)3);
            ensure_eq(layout.sorted_values[4], value5);
            ensure_eq(layout.counts[4], (size_t)3);
            //
            distribution_insert(&builder, value4);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)5);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value2);
            ensure_eq(layout.counts[1], (size_t)3);
            ensure_eq(layout.sorted_values[2], value3);
            ensure_eq(layout.counts[2], (size_t)2);
            ensure_eq(layout.sorted_values[3], value4);
            ensure_eq(layout.counts[3], (size_t)4);
            ensure_eq(layout.sorted_values[4], value5);
            ensure_eq(layout.counts[4], (size_t)3);

            trace("Setting up an allocation backup...");
            size_t allocation_size =
                builder.inner.capacity * distribution_bin_size;
            void* prev_data = malloc(allocation_size);
            int64_t* prev_values = (int64_t*)prev_data;
            size_t* prev_counts = (size_t*)(
                (char*)prev_data + builder.inner.capacity * sizeof(int64_t)
            );

            trace("Inserting new values until reallocation...");
            while (builder.inner.num_bins < builder.inner.capacity) {
                trace("- No reallocation expected here. Backing up state...");
                memcpy(prev_data, builder.inner.allocation, allocation_size);
                const size_t prev_bins = builder.inner.num_bins;

                const int64_t value = rand() - RAND_MAX / 2;
                tracef("- Inserting value %zd...", value);
                distribution_insert(&builder, value);

                trace("- Checking global metadata which shouldn't change...");
                ensure_eq(builder.inner.allocation, initial_allocation);
                ensure_eq(builder.inner.capacity, initial_capacity);

                trace("- Checking bin contents...");
                size_t after_offset;
                size_t insert_pos = SIZE_MAX;
                for (size_t src_pos = 0; src_pos < prev_bins; ++src_pos) {
                    tracef("  * Checking former bin #%zu...", src_pos);
                    if (prev_values[src_pos] < value) {
                        ensure_eq(layout.sorted_values[src_pos],
                                  prev_values[src_pos]);
                        ensure_eq(layout.counts[src_pos], prev_counts[src_pos]);
                    } else if (prev_values[src_pos] > value) {
                        if (insert_pos == SIZE_MAX) {
                            insert_pos = src_pos;
                            after_offset = 1;
                        }
                        const size_t dst_pos = src_pos + after_offset;
                        ensure_eq(layout.sorted_values[dst_pos],
                                  prev_values[src_pos]);
                        ensure_eq(layout.counts[dst_pos], prev_counts[src_pos]);
                    } else {
                        assert(prev_values[src_pos] == value);
                        insert_pos = src_pos;
                        after_offset = 0;
                        ensure_eq(layout.sorted_values[src_pos], value);
                        ensure_eq(layout.counts[src_pos],
                                  prev_counts[src_pos] + 1);
                    }
                }

                const size_t prev_end = prev_bins;
                const size_t prev_last = prev_end - 1;
                if (insert_pos == SIZE_MAX) {
                    trace("- Checking past-the-end insertion...");
                    ensure_eq(builder.inner.num_bins, prev_end + 1);
                    ensure_eq(layout.sorted_values[prev_end], value);
                    ensure_eq(layout.counts[prev_end], (size_t)1);
                } else {
                    trace("- Checking internal insertion...");
                    ensure_eq(builder.inner.num_bins, prev_bins + after_offset);
                }
            }

            trace("Testing reallocation...");
            retry:
                const int64_t value = rand() - RAND_MAX / 2;
                tracef("- Checking candidate value %zd...", value);
                size_t insert_pos = SIZE_MAX;
                for (size_t pos = 0; pos < builder.inner.num_bins; ++pos) {
                    if (layout.sorted_values[pos] > value) {
                        insert_pos = pos;
                        break;
                    } else if (layout.sorted_values[pos] == value) {
                        tracef("  * Value already present in bin #%zu, try again...",
                               pos);
                        goto retry;
                    }
                }
                if (insert_pos == SIZE_MAX) insert_pos = builder.inner.num_bins;
                tracef("  * Value will be inserted as bin #%zu", insert_pos);
            //
            trace("- Backing up state...");
            memcpy(prev_data, builder.inner.allocation, allocation_size);
            const void* const prev_allocation = builder.inner.allocation;
            const size_t prev_bins = builder.inner.num_bins;
            const size_t prev_capacity = builder.inner.capacity;
            //
            trace("- Performing insertion which should reallocate...");
            distribution_insert(&builder, value);
            //
            trace("- Checking that reallocation occured...");
            ensure_ne(builder.inner.allocation, prev_allocation);
            ensure_eq(builder.inner.num_bins, prev_bins + 1);
            ensure_gt(builder.inner.capacity, prev_capacity);
            //
            trace("- Checking that reallocation changes the layout...");
            const distribution_layout_t new_layout =
                distribution_layout(&builder.inner);
            ensure_ne((void*)new_layout.sorted_values,
                      (void*)layout.sorted_values);
            ensure_ne((void*)new_layout.counts, (void*)layout.counts);
            layout = new_layout;
            //
            trace("- Checking bin contents...");
            ensure_eq(layout.sorted_values[insert_pos], value);
            ensure_eq(layout.counts[insert_pos], (size_t)1);
            for (size_t src_pos = 0; src_pos < prev_bins; ++src_pos) {
                tracef("  * Checking former bin #%zu...", src_pos);
                const size_t dst_pos = src_pos + (size_t)(src_pos >= insert_pos);
                ensure_eq(layout.sorted_values[dst_pos],
                          prev_values[src_pos]);
                ensure_eq(layout.counts[dst_pos],
                          prev_counts[src_pos]);
            }

            trace("Reallocating backup storage to match new capacity...");
            free(prev_data);
            allocation_size = builder.inner.capacity * distribution_bin_size;
            prev_data = malloc(allocation_size);
            prev_values = (int64_t*)prev_data;
            prev_counts = (size_t*)(
                (char*)prev_data + builder.inner.capacity * sizeof(int64_t)
            );

            trace("Backing up the final distribution builder...");
            memcpy(prev_data, builder.inner.allocation, allocation_size);

            trace("Building the distribution...");
            distribution_t prev_dist = builder.inner;
            distribution_t dist = distribution_build(&builder);
            ensure_eq(builder.inner.allocation, NULL);
            ensure_eq(builder.inner.num_bins, (size_t)0);
            ensure_eq(builder.inner.capacity, (size_t)0);
            ensure_eq(dist.allocation, prev_dist.allocation);
            ensure_eq(dist.num_bins, prev_dist.num_bins);
            ensure_eq(dist.capacity, prev_dist.capacity);

            trace("Checking the final distribution's bins...");
            size_t expected_end_idx = 0;
            size_t* prev_end_indices = prev_counts;
            for (size_t bin = 0; bin < dist.num_bins; ++bin) {
                ensure_eq(layout.sorted_values[bin], prev_values[bin]);
                expected_end_idx += prev_counts[bin];
                ensure_eq(layout.end_indices[bin], expected_end_idx);
                prev_end_indices[bin] = expected_end_idx;
            }
            prev_counts = NULL;
            ensure_eq(distribution_len(&dist), expected_end_idx);

            trace("Testing distribution sampling...");
            const size_t num_samples = 10 * dist.num_bins;
            for (size_t i = 0; i < num_samples; ++i) {
                trace("- Grabbing one sample...");
                const int64_t sample = distribution_sample(&dist);

                trace("- Checking const correctness and locating sampled bin...");
                size_t sampled_bin = SIZE_MAX;
                for (size_t bin = 0; bin < dist.num_bins; ++bin) {
                    if (layout.sorted_values[bin] == sample) sampled_bin = bin;
                    ensure_eq(layout.sorted_values[bin], prev_values[bin]);
                    ensure_eq(layout.end_indices[bin], prev_end_indices[bin]);
                }
                ensure_ne(sampled_bin, SIZE_MAX);
            }

            trace("Deallocating backup storage...");
            free(prev_data);
            prev_data = NULL;
            prev_values = NULL;
            prev_end_indices = NULL;

            trace("Resetting the distribution...");
            prev_dist = dist;
            builder = distribution_reset(&dist);
            ensure_eq(dist.allocation, NULL);
            ensure_eq(dist.num_bins, (size_t)0);
            ensure_eq(dist.capacity, (size_t)0);
            ensure_eq(builder.inner.allocation, prev_dist.allocation);
            ensure_eq(builder.inner.num_bins, (size_t)0);
            ensure_eq(builder.inner.capacity, prev_dist.capacity);

            trace("Destroying the distribution...");
            distribution_finalize(&builder.inner);
            ensure_eq(builder.inner.allocation, NULL);
            ensure_eq(builder.inner.num_bins, (size_t)0);
            ensure_eq(builder.inner.capacity, (size_t)0);
        }

        void benchmark_unit_tests() {
            info("Running benchmark harness unit tests...");
            configure_rand();

            debug("Running distribution unit tests...");
            with_log_level(UDIPE_TRACE, {
                test_distibution();
            });

            // TODO: Add unit tests for stats, then clocks

            // TODO: Test other components
        }

    #endif  // UDIPE_BUILD_TESTS

#endif  // UDIPE_BUILD_BENCHMARKS