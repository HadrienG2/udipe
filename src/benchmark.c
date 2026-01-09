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
    #define NUM_MEDIAN_SAMPLES ((size_t)5)
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
    #define WARMUP_OFFSET_OS (1000*UDIPE_MILLISECOND)

    /// Number of benchmark runs used for OS clock offset calibration
    ///
    /// Tune this up if clock offset calibration is unstable, as evidenced by
    /// the fact that short loops get a nonzero median duration.
    //
    // TODO: Tune on more systems
    #define NUM_RUNS_OFFSET_OS ((size_t)64*1024)

    /// Warmup duration used for shortest loop calibration
    //
    // TODO: Tune on more systems
    #define WARMUP_SHORTEST_LOOP (3000*UDIPE_MILLISECOND)

    /// Number of benchmark runs used for shortest loop calibration
    ///
    /// Tune this up if the shortest loop calibration is unstable and does not
    /// converge to a constant loop size.
    //
    // TODO: Tune on more systems
    #define NUM_RUNS_SHORTEST_LOOP ((size_t)64*1024)

    /// Warmup duration used for best loop calibration
    //
    // TODO: Tune on more systems
    #define WARMUP_BEST_LOOP (3000*UDIPE_MILLISECOND)

    /// Number of benchmark run used for optimal loop calibration, when using
    /// the system clock to perform said calibration
    ///
    /// Tune this up if the optimal loop calibration is unstable and does not
    /// converge to sufficiently reproducible statistics.
    ///
    /// Tune this down if you observe multimodal timing laws, which indicates
    /// that the CPU switches performance states during the measurement, and
    /// this state instability is not fixed by using a longer warmup or
    /// adjusting the system's power management configuration.
    //
    // TODO: Tune on more systems
    #define NUM_RUNS_BEST_LOOP_OS ((size_t)64*1024)

    #ifdef X86_64

        /// Number of benchmark runs used when measuring the duration of the
        /// optimal loop using the x86 TimeStamp Counter
        ///
        /// Tune this up if the optimal loop calibration does not yield
        /// reproducible results.
        //
        // TODO: Tune on more systems
        #define NUM_RUNS_BEST_LOOP_X86 ((size_t)8*1024)

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
        #define NUM_RUNS_OFFSET_X86 ((size_t)16*1024)

    #endif  // X86_64


    /// Comparison function for applying qsort() to int64_t[]
    static inline int compare_i64(const void* v1, const void* v2) {
        const int64_t* const d1 = (const int64_t*)v1;
        const int64_t* const d2 = (const int64_t*)v2;
        if (*d1 < *d2) return -1;
        if (*d1 > *d2) return 1;
        return 0;
    }


    temporal_filter_t
    temporal_filter_initialize(const int64_t initial_window[TEMPORAL_WINDOW]) {
        trace("Setting up a temporal outlier filter...");
        temporal_filter_t result = {
            .next_idx = 0
        };
        for (size_t i = 0; i < TEMPORAL_WINDOW; ++i) {
            result.window[i] = initial_window[i];
        }
        temporal_filter_set_min(&result);
        temporal_filter_init_maxima(&result);
        return result;
    }

    UDIPE_NON_NULL_ARGS
    void temporal_filter_set_min(temporal_filter_t* filter) {
        trace("Figuring out minimal input...");
        filter->min = INT64_MAX;
        filter->min_count = 0;
        for (size_t i = 0; i < TEMPORAL_WINDOW; ++i) {
            const int64_t value = filter->window[i];
            tracef("- Integrating value[%zu] = %zd...", i, value);
            if (value < filter->min) {
                trace("  => New minimum reached.");
                filter->min = value;
                filter->min_count = 1;
            } else if (value == filter->min) {
                trace("  => New occurence of the current minimum.");
                ++(filter->min_count);
            }
        }
        assert(filter->min_count >= 1);
        tracef("Minimal input is %zd (%zu occurences).",
               filter->min, (size_t)filter->min_count);
    }

    UDIPE_NON_NULL_ARGS
    void temporal_filter_init_maxima(temporal_filter_t* filter) {
        // min can't be an outlier because all values are >= min, the window has
        // at least 2 values, and we operate under a single-outlier hypothesis
        tracef("Initializing max_normal to min = %zd...", filter->min);
        filter->max_normal = filter->min;
        filter->max_normal_count = (filter->window[0] == filter->min);
        assert(TEMPORAL_WINDOW >= 2);

        // First value is by definition the largest value seen so far
        filter->max = filter->window[0];
        uint16_t first_max_idx = 0;
        tracef("After integrating window[0] = %zd, "
               "max is %zd and max_normal_count is %zu...",
               filter->window[0], filter->max, (size_t)filter->max_normal_count);

        // Integrate other values. At this point, we don't yet know the
        // window-wide max_normal and upper_tolerance, so we can't tell if an
        // isolated max is an outlier. We pessimistically assume that it is,
        // which keeps max_normal conservatively set to the next-to-max value,
        // that we will later use to check if max truly is an outlier or not.
        trace("Integrating other window values...");
        for (size_t i = 1; i < TEMPORAL_WINDOW; ++i) {
            const int64_t value = filter->window[i];
            tracef("- Integrating value[%zu] = %zd...", i, value);
            if (value > filter->max) {
                tracef("  => %zd is the new max, could be an outlier...",
                       value);
                if (filter->max > filter->max_normal) {
                    trace("  => ...but former max > max_normal cannot "
                          "be an outlier too, make it the new max_normal.");
                    filter->max_normal = filter->max;
                    filter->max_normal_count = 1;
                } else {
                    trace("  => ...so we stick with the former max_normal/max.");
                }
                filter->max = value;
                first_max_idx = i;
            } else if (value == filter->max_normal) {
                tracef("  => Encountered one more occurence of max_normal %zd.",
                       value);
                ++(filter->max_normal_count);
            } else if (value == filter->max) {
                assert(filter->max > filter->max_normal);
                tracef("  => Encountered a second occurence of max %zd. "
                       "It is thus not an outlier and becomes max_normal.",
                       value);
                filter->max_normal = filter->max;
                filter->max_normal_count = 2;
            } else if (value > filter->max_normal) {
                assert(value < filter->max);
                tracef("  => %zd is the new max_normal. "
                       "It cannot be an outlier because max is higher.",
                       value);
                filter->max_normal = value;
                filter->max_normal_count = 1;
            }
        }
        assert(filter->max >= filter->max_normal);
        assert(filter->max_normal_count >= 1);

        // The result may be incorrect if max is isolated: in this case we may
        // have misclassified it as an outlier.
        if (filter->max > filter->max_normal) {
            // When this happens, max_normal is next-to-max, use it to compute
            // upper_tolerance and figure out if max is indeed an outlier.
            tracef("Found isolated maximum %zd at index %zu. "
                   "Use next-to-max %zd to compute upper_tolerance "
                   "and deduce if max is an outlier...",
                   filter->max, (size_t)first_max_idx, filter->max_normal);
            temporal_filter_update_tolerance(filter);
            if (filter->max <= filter->upper_tolerance) {
                trace("max is actually in tolerance, "
                      "will become single-occurence max_normal.");
                filter->max_normal = filter->max;
                filter->max_normal_count = 1;
                temporal_filter_update_tolerance(filter);
                filter->outlier_idx = TEMPORAL_WINDOW;
            } else {
                tracef("max is indeed an outlier, "
                       "max_normal is thus %zd (%zu occurences).",
                       filter->max_normal, (size_t)filter->max_normal_count);
                filter->outlier_idx = first_max_idx;
            }
        } else {
            assert(filter->max == filter->max_normal);
            tracef("Found non-isolated max %zd (%zu occurences), "
                   "which can't be an outlier and is thus max_normal.",
                   filter->max_normal, (size_t)filter->max_normal_count);
            temporal_filter_update_tolerance(filter);
            filter->outlier_idx = TEMPORAL_WINDOW;
        }
    }

    UDIPE_NON_NULL_ARGS
    void temporal_filter_update_tolerance(temporal_filter_t* filter) {
        filter->upper_tolerance = ceil(
            filter->max_normal
            + (filter->max_normal - filter->min) * TEMPORAL_TOLERANCE
        );
        tracef("Updated outlier filter upper_tolerance to %zd.",
               filter->upper_tolerance);
    }

    UDIPE_NON_NULL_ARGS
    void temporal_filter_make_max_normal(temporal_filter_t* filter,
                                        temporal_filter_result_t* result,
                                        const char* reason) {
        assert(filter->max > filter->max_normal);
        tracef("Reclassified max %zd as non-outlier: %s.",
               filter->max, reason);
        result->previous_not_outlier = true;
        result->previous_input = filter->max;
        filter->max_normal = filter->max;
        filter->max_normal_count = 1;
        filter->outlier_idx = TEMPORAL_WINDOW;
    }

    UDIPE_NON_NULL_ARGS
    bool temporal_filter_decrease_min(temporal_filter_t* filter,
                                     temporal_filter_result_t* result,
                                     int64_t new_min) {
        assert(new_min < filter->min);
        filter->min = new_min;
        filter->min_count = 1;
        temporal_filter_update_tolerance(filter);
        if (filter->max > filter->max_normal
            && filter->max <= filter->upper_tolerance)
        {
            temporal_filter_make_max_normal(
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
    bool temporal_filter_increase_max(temporal_filter_t* filter,
                                     temporal_filter_result_t* result,
                                     int64_t new_max) {
        assert(new_max > filter->max);
        if (filter->max > filter->max_normal) {
            temporal_filter_make_max_normal(
                filter,
                result,
                "encountered a larger input and there can only be one outlier"
            );
            temporal_filter_update_tolerance(filter);
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
    bool temporal_filter_increase_max_normal(temporal_filter_t* filter,
                                            temporal_filter_result_t* result,
                                            int64_t new_max_normal) {
        assert(new_max_normal > filter->max_normal);
        assert(new_max_normal < filter->max);
        filter->max_normal = new_max_normal;
        filter->max_normal_count = 1;
        temporal_filter_update_tolerance(filter);
        if (filter->max <= filter->upper_tolerance) {
            temporal_filter_make_max_normal(
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
    void temporal_filter_reset_maxima(temporal_filter_t* filter) {
        trace("Leveraging knowledge of outlier_idx to ease max_normal search...");
        const size_t first_normal_idx = (size_t)(filter->outlier_idx == 0);
        filter->max_normal = filter->window[first_normal_idx];
        filter->max_normal_count = 1;
        for (size_t i = first_normal_idx + 1; i < TEMPORAL_WINDOW; ++i) {
            if (i == filter->outlier_idx) continue;
            const int64_t normal_value = filter->window[i];
            if (normal_value > filter->max_normal) {
                filter->max_normal = normal_value;
                filter->max_normal_count = 1;
            } else if (normal_value == filter->max_normal) {
                ++(filter->max_normal_count);
            }
        }
        if (filter->outlier_idx < TEMPORAL_WINDOW) {
            assert(filter->max == filter->window[filter->outlier_idx]);
            assert(filter->max > filter->max_normal);
        } else {
            filter->max = filter->max_normal;
        }
        temporal_filter_update_tolerance(filter);
    }

    UDIPE_NON_NULL_ARGS
    void temporal_filter_finalize(temporal_filter_t* filter) {
        for (size_t i = 0; i < TEMPORAL_WINDOW; ++i) {
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

    distribution_t distribution_allocate(size_t capacity) {
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
    void distribution_log(distribution_builder_t* builder,
                          udipe_log_level_t level,
                          const char* header) {
        if (log_enabled(level)) {
            const distribution_t* dist = &builder->inner;
            const distribution_layout_t layout = distribution_layout(dist);
            udipe_logf(level, "%s: {", header);
            for (size_t bin = 0; bin < dist->num_bins; ++bin) {
                udipe_logf(level,
                           "  %zd: %zu,",
                           layout.sorted_values[bin], layout.counts[bin]);
            }
            udipe_log(level, "}");
        }
    }

    UDIPE_NON_NULL_ARGS
    distribution_t distribution_build(distribution_builder_t* builder) {
        trace("Extracting the distribution from the builder...");
        distribution_t dist = builder->inner;
        distribution_poison(&builder->inner);

        trace("Ensuring the distribution can be sampled...");
        ensure_ge(dist.num_bins, (size_t)1);

        trace("Turning value counts into end indices...");
        distribution_layout_t layout = distribution_layout(&dist);
        size_t end_idx = 0;
        for (size_t bin = 0; bin < dist.num_bins; ++bin) {
            end_idx += layout.counts[bin];
            layout.end_indices[bin] = end_idx;
        }
        return dist;
    }

    UDIPE_NON_NULL_ARGS
    void distribution_discard(distribution_builder_t* builder) {
        distribution_finalize(&builder->inner);
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


    UDIPE_NON_NULL_ARGS
    void empty_loop(void* context) {
        size_t num_iters = *((const size_t*)context);
        // Ensures that all loop lengths get the same codegen
        UDIPE_ASSUME_ACCESSED(num_iters);
        for (size_t iter = 0; iter < num_iters; ++iter) {
            // This is ASSUME_ACCESSED and not ASSUME_READ because with
            // ASSUME_READ the compiler can unroll the loop and this will reduce
            // timing reproducibility with respect to the pure dependency chain
            // of a non-unrolled loop.
            UDIPE_ASSUME_ACCESSED(iter);
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
            int udipe_uncertainty_decimals = ceil(-log10(udipe_uncertainty)) + 1;  \
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
    distribution_t compute_duration_distribution(
        int64_t (*compute_duration)(void* /* context */,
                                    size_t /* run */),
        void* context,
        size_t num_runs,
        distribution_builder_t* result_builder
    ) {
        ensure_ge(num_runs, TEMPORAL_WINDOW);

        trace("Setting up statistics...");
        size_t num_normal_runs = 0;
        size_t num_initially_rejected = 0;
        size_t num_reclassified = 0;
        distribution_builder_t reject_builder;
        distribution_builder_t reclassify_builder;
        if (log_enabled(UDIPE_DEBUG)) {
            reject_builder = distribution_initialize();
            reclassify_builder = distribution_initialize();
        }

        trace("Seeding temporal outlier filter...");
        int64_t initial_window[TEMPORAL_WINDOW];
        for (size_t run = 0; run < TEMPORAL_WINDOW; ++run) {
            initial_window[run] = compute_duration(context, run);
        }
        temporal_filter_t filter = temporal_filter_initialize(initial_window);

        trace("Collecting temporally filtered durations...");
        TEMPORAL_FILTER_FOREACH_NORMAL(&filter, duration, {
            distribution_insert(result_builder, duration);
            ++num_normal_runs;
        });
        // There can be at most one outlier per input window
        ensure_le(TEMPORAL_WINDOW - num_normal_runs, (uint16_t)1);
        //
        for (size_t run = TEMPORAL_WINDOW; run < num_runs; ++run) {
            const int64_t duration = compute_duration(context, run);
            const temporal_filter_result_t result =
                temporal_filter_apply(&filter, duration);
            if (result.previous_not_outlier) {
                tracef("- Reclassified previous outlier duration %zd as non-outlier",
                       result.previous_input);
                for (size_t pos = 0; pos < TEMPORAL_WINDOW; ++pos) {
                    const size_t idx = (filter.next_idx + pos) % TEMPORAL_WINDOW;
                    const size_t age = TEMPORAL_WINDOW - 1 - pos;
                    tracef("  * duration[%zu aka -%zu] is %zd",
                           run - age,
                           age,
                           filter.window[idx]);
                }
                distribution_insert(result_builder, result.previous_input);
                ++num_normal_runs;
                if (log_enabled(UDIPE_DEBUG)) {
                    distribution_insert(&reclassify_builder, result.previous_input);
                    ++num_reclassified;
                }
            }
            if (!result.current_is_outlier) {
                distribution_insert(result_builder, duration);
                ++num_normal_runs;
            } else if (log_enabled(UDIPE_DEBUG)) {
                distribution_insert(&reject_builder, duration);
                ++num_initially_rejected;
            }
            ensure_le(num_normal_runs, run + 1);
        }

        trace("Reporting results...");
        if (log_enabled(UDIPE_DEBUG)) {
            if (num_initially_rejected > 0) {
                distribution_log(&reject_builder,
                                 UDIPE_DEBUG,
                                 "Durations initially rejected as outliers");
            }
            distribution_discard(&reject_builder);
            if (num_reclassified > 0) {
                distribution_log(&reclassify_builder,
                                 UDIPE_DEBUG,
                                 "Durations later reclassified to non-outlier");
                debugf("Reclassified %zu/%zu durations from outlier to normal.",
                       num_reclassified, num_runs);
            }
            distribution_discard(&reclassify_builder);
            if (num_normal_runs < num_runs) {
                const size_t num_outliers = num_runs - num_normal_runs;
                debugf("Eventually rejected %zu/%zu durations.",
                       num_outliers, num_runs);
            }
            distribution_log(result_builder,
                             UDIPE_DEBUG,
                             "Accepted durations");
        }
        distribution_t result = distribution_build(result_builder);
        assert(distribution_len(&result) == num_runs);
        return result;
    }


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
        size_t num_iters = 0;
        distribution_t tmp_offsets = os_clock_measure(
            &clock,
            empty_loop,
            &num_iters,
            WARMUP_OFFSET_OS,
            NUM_RUNS_OFFSET_OS,
            &clock.builder
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
        num_iters = 1;
        do {
            debugf("- Trying loop with %zu iteration(s)...", num_iters);
            loop_durations = os_clock_measure(
                &clock,
                empty_loop,
                &num_iters,
                WARMUP_SHORTEST_LOOP,
                NUM_RUNS_SHORTEST_LOOP,
                &clock.builder
            );
            loop_duration_stats = stats_analyze(analyzer, &loop_durations);
            log_calibration_stats(UDIPE_DEBUG,
                                  "  * Loop duration",
                                  loop_duration_stats,
                                  "ns");
            const signed_duration_ns_t loop_duration_spread =
                loop_duration_stats.high - loop_duration_stats.low;
            if (loop_duration_stats.low < 9*offset_stats.high) {
                debug("  * Clock contribution may still be >10%...");
            } else if(loop_duration_stats.low < 10*loop_duration_spread) {
                debug("  * Duration may still fluctuates by >10%...");
            } else {
                debug("  * That's long enough and stable enough.");
                clock.builder = distribution_initialize();
                break;
            }
            // If control reaches here, must still increase loop size
            num_iters *= 2;
            clock.builder = distribution_reset(&loop_durations);
        } while(true);
        infof("- Loops with >=%zu iterations have non-negligible duration.",
              num_iters);

        info("Finding optimal loop duration...");
        clock.best_empty_iters = num_iters;
        clock.best_empty_durations = loop_durations;
        distribution_poison(&loop_durations);
        clock.best_empty_stats = loop_duration_stats;
        const int64_t best_precision = loop_duration_stats.high - loop_duration_stats.low;
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
                &clock.builder
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
            // In a regime of stable run timing precision, doubling the
            // iteration count should improve iteration timing uncertainty by
            // 2x. Ignore small improvements that don't justify a 2x longer run
            // duration, and thus fewer runs per unit of execution time...
            if (uncertainty < best_uncertainty/1.1) {
                debug("  * This is our new best loop. Can we do even better?");
                best_uncertainty = uncertainty;
                clock.best_empty_iters = num_iters;
                clock.builder = distribution_reset(&clock.best_empty_durations);
                clock.best_empty_durations = loop_durations;
                distribution_poison(&loop_durations);
                clock.best_empty_stats = loop_duration_stats;
                continue;
            } else if (precision <= 3*best_precision) {
                // ...but keep trying until the uncertainty degradation becomes
                // much worse than expected in a regime of stable iteration
                // timing uncertainty, in which case loop duration fluctuates 2x
                // more when loop iteration gets 2x higher.
                debug("  * That's not much better/worse, keep trying...");
                clock.builder = distribution_reset(&loop_durations);
                continue;
            } else {
                debug("  * Absolute precision degraded by >3x: time to stop!");
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

    /// compute_duration_distribution() callback used by os_clock_measure()
    ///
    /// `context` must be a pointer to the associated \ref os_clock_t.
    static inline int64_t compute_os_duration(void* context,
                                              size_t run) {
        os_clock_t* clock = (os_clock_t*)context;
        assert(run < clock->num_durations);
        return os_duration(clock,
                           clock->timestamps[run],
                           clock->timestamps[run+1]);
    }

    UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 6)
    distribution_t os_clock_measure(
        os_clock_t* clock,
        void (*workload)(void*),
        void* context,
        udipe_duration_ns_t warmup,
        size_t num_runs,
        distribution_builder_t* result_builder
    ) {
        if (num_runs > clock->num_durations) {
            trace("Reallocating storage from %zu to %zu durations...");
            realtime_liberate(
                clock->timestamps,
                (clock->num_durations+1) * sizeof(os_timestamp_t)
            );
            clock->num_durations = num_runs;
            clock->timestamps =
                realtime_allocate((num_runs+1) * sizeof(os_timestamp_t));
        }

        trace("Warming up...");
        os_timestamp_t* timestamps = clock->timestamps;
        udipe_duration_ns_t elapsed = 0;
        os_timestamp_t start = os_now();
        do {
            workload(context);
            os_timestamp_t now = os_now();
            elapsed = os_duration(clock, start, now);
        } while(elapsed < warmup);

        tracef("Performing %zu timed runs...", num_runs);
        timestamps[0] = os_now();
        for (size_t run = 0; run < num_runs; ++run) {
            UDIPE_ASSUME_READ(timestamps);
            workload(context);
            timestamps[run+1] = os_now();
        }
        UDIPE_ASSUME_READ(timestamps);

        trace("Computing duration distribution...");
        return compute_duration_distribution(compute_os_duration,
                                             (void*)clock,
                                             num_runs,
                                             result_builder);
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
                &builder
            );
            log_calibration_stats(UDIPE_INFO,
                                  "- Offset-biased best loop",
                                  stats_analyze(analyzer, &raw_empty_ticks),
                                  "ticks");

            info("Measuring clock offset...");
            builder = distribution_initialize();
            size_t empty_loop_iters = 0;
            distribution_t tmp_offsets = x86_clock_measure(
                &clock,
                empty_loop,
                &empty_loop_iters,
                WARMUP_OFFSET_X86,
                NUM_RUNS_OFFSET_X86,
                &builder
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

        /// compute_duration_distribution() context used by x86_clock_measure()
        ///
        typedef struct x86_measure_context_s {
            x86_clock_t* clock;  ///< x86 clock used for the measurement
            size_t num_runs;  ///< Number of benchmark runs
        } x86_measure_context_t;

        /// compute_duration_distribution() callback used by x86_clock_measure()
        ///
        /// `context` must be a pointer to the associated \ref
        /// x86_measure_context_t.
        static inline int64_t compute_x86_duration(void* context,
                                                   size_t run) {
            x86_measure_context_t* measure = (x86_measure_context_t*)context;
            x86_clock_t* clock = measure->clock;
            assert(run < measure->num_runs);
            assert(measure->num_runs < clock->num_durations);
            x86_instant* starts = clock->instants;
            x86_instant* ends = starts + measure->num_runs;
            const int64_t raw_ticks = ends[run] - starts[run];
            return raw_ticks - distribution_sample(&clock->offsets);
        }

        UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 6)
        distribution_t x86_clock_measure(
            x86_clock_t* xclock,
            void (*workload)(void*),
            void* context,
            udipe_duration_ns_t warmup,
            size_t num_runs,
            distribution_builder_t* result_builder
        ) {
            if (num_runs > xclock->num_durations) {
                trace("Reallocating storage from %zu to %zu durations...");
                realtime_liberate(
                    xclock->instants,
                    2 * xclock->num_durations * sizeof(x86_instant)
                );
                xclock->num_durations = num_runs;
                xclock->instants =
                    realtime_allocate(2 * num_runs * sizeof(x86_instant));
            }

            trace("Setting up measurement...");
            x86_instant* starts = xclock->instants;
            x86_instant* ends = xclock->instants + num_runs;
            const bool strict = false;
            x86_timestamp_t timestamp = x86_timer_start(strict);
            const x86_cpu_id initial_cpu_id = timestamp.cpu_id;

            trace("Warming up...");
            udipe_duration_ns_t elapsed = 0;
            clock_t start = clock();
            do {
                timestamp = x86_timer_start(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                UDIPE_ASSUME_READ(timestamp.ticks);

                workload(context);

                timestamp = x86_timer_end(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                UDIPE_ASSUME_READ(timestamp.ticks);

                clock_t now = clock();
                elapsed = (udipe_duration_ns_t)(now - start)
                          * UDIPE_SECOND / CLOCKS_PER_SEC;
            } while(elapsed < warmup);

            tracef("Performing %zu timed runs...", num_runs);
            for (size_t run = 0; run < num_runs; ++run) {
                timestamp = x86_timer_start(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                starts[run] = timestamp.ticks;
                UDIPE_ASSUME_READ(starts);

                workload(context);

                timestamp = x86_timer_end(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                ends[run] = timestamp.ticks;
                UDIPE_ASSUME_READ(ends);
            }

            trace("Computing duration distribution...");
            x86_measure_context_t measure_context = {
                .clock = xclock,
                .num_runs = num_runs
            };
            return compute_duration_distribution(compute_x86_duration,
                                                 (void*)&measure_context,
                                                 num_runs,
                                                 result_builder);
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

        /// Number of initial temporal_filter_t states
        ///
        /// This affects the thoroughness of constructor tests and the number of
        /// states from which insertion tests will take place.
        static const size_t NUM_INITIAL_STATES = 100;

        /// Kind of temporal_filter_apply() call
        ///
        /// This is used to ensure even branch coverage in
        /// temporal_filter_apply() tests.
        typedef enum apply_kind_e {
            APPLY_BELOW_MIN = 0,
            APPLY_EQUAL_MIN,
            APPLY_BETWEEN_MIN_AND_MAX_NORMAL,
            APPLY_EQUAL_MAX_NORMAL,
            APPLY_BETWEEN_MAX_NORMAL_AND_MAX,
            APPLY_EQUAL_MAX,
            APPLY_ABOVE_MAX,
            APPLY_KIND_LEN,
        } apply_kind_t;

        /// Number of temporal_filter_apply() runs per initial state
        ///
        /// This affects the thoroughness of temporal_filter_apply() tests.
        static const size_t NUM_APPLY_CALLS = 100 * APPLY_KIND_LEN;

        /// Check two temporal outlier filters for logical state equality
        ///
        /// `next_idx` is allowed to be different as long as unwrapping the
        /// `window` ring buffer into an array from this index yields the same
        /// result for both filters.
        UDIPE_NON_NULL_ARGS
        void ensure_eq_temporal_filter(const temporal_filter_t* f1,
                                       const temporal_filter_t* f2) {
            ensure_eq(f1->min, f2->min);
            ensure_eq(f1->max_normal, f2->max_normal);
            ensure_eq(f1->upper_tolerance, f2->upper_tolerance);
            ensure_eq(f1->max, f2->max);
            ensure_eq(f1->min_count, f2->min_count);
            ensure_eq(f1->max_normal_count, f2->max_normal_count);
            for (size_t i = 0; i < TEMPORAL_WINDOW; ++i) {
                const size_t i1 = (f1->next_idx + i) % TEMPORAL_WINDOW;
                const size_t i2 = (f2->next_idx + i) % TEMPORAL_WINDOW;
                ensure_eq(f1->window[i1], f2->window[i2]);
            }
        }

        /// Perfom checks that should be true after any operation on a temporal
        /// outlier filter.
        UDIPE_NON_NULL_ARGS
        void check_any_temporal_filter(const temporal_filter_t* filter) {
            trace("Ensuring stats are internally consistent...");
            ensure_le(filter->min, filter->max_normal);
            ensure_le(filter->max_normal, filter->max);
            ensure_le(filter->max_normal, filter->upper_tolerance);
            ensure_eq(
                filter->upper_tolerance,
                ceil(
                    filter->max_normal
                        + (filter->max_normal - filter->min) * TEMPORAL_TOLERANCE
                )
            );
            ensure_lt(filter->next_idx, TEMPORAL_WINDOW);
            ensure_le(filter->min_count, TEMPORAL_WINDOW);
            ensure_le(filter->max_normal_count, TEMPORAL_WINDOW);

            trace("Ensuring stats are consistent with the input window...");
            size_t min_count = 0;
            size_t max_normal_count = 0;
            size_t max_count = 0;
            for (size_t i = 0; i < TEMPORAL_WINDOW; ++i) {
                const int64_t value = filter->window[i];
                ensure_ge(value, filter->min);
                if (value == filter->min) ++min_count;
                ensure_le(value, filter->max);
                if (value == filter->max) ++max_count;
                ensure(value <= filter->max_normal || value == filter->max);
                if (value == filter->max_normal) ++max_normal_count;
            }
            ensure_eq(min_count, filter->min_count);
            ensure_eq(max_normal_count, filter->max_normal_count);

            trace("Ensuring outliers are handled correctly...");
            if (filter->max > filter->max_normal) {
                ensure_eq(max_count, (size_t)1);
                ensure_gt(filter->max, filter->upper_tolerance);
            } else {
                ensure_eq(filter->max, filter->max_normal);
                ensure_eq(max_count, max_normal_count);
                ensure_le(filter->max, filter->upper_tolerance);
            }

            trace("Ensuring normal value iteration yields expected outputs...");
            const temporal_filter_t before = *filter;
            uint16_t expected_idx = filter->next_idx;
            TEMPORAL_FILTER_FOREACH_NORMAL(filter, normal, {
                if (filter->window[expected_idx] > filter->upper_tolerance) {
                    expected_idx = (expected_idx + 1) % TEMPORAL_WINDOW;
                }
                ensure_eq(normal, filter->window[expected_idx]);
                expected_idx = (expected_idx + 1) % TEMPORAL_WINDOW;
            });
            ensure(
                expected_idx == filter->next_idx
                || ((expected_idx + 1) % TEMPORAL_WINDOW == filter->next_idx
                    && filter->window[expected_idx] > filter->upper_tolerance)
            );

            trace("Ensuring normal iteration doesn't alter state...");
            ensure_eq_temporal_filter(filter, &before);
        }

        /// Test temporal_filter_initialize then return the initialized
        /// temporal_filter_t for use in further testing.
        static temporal_filter_t checked_temporal_filter(
            int64_t window[TEMPORAL_WINDOW]
        ) {
            temporal_filter_t filter = temporal_filter_initialize(window);

            trace("Checking initial state...");
            check_any_temporal_filter(&filter);
            ensure_eq(filter.next_idx, (uint16_t)0);
            for (size_t i = 0; i < TEMPORAL_WINDOW; ++i) {
                ensure_eq(filter.window[i], window[i]);
            }
            return filter;
        }

        /// Checks that are common to all check_apply_xyz() tests
        ///
        UDIPE_NON_NULL_ARGS
        static void check_apply_common(const temporal_filter_t* before,
                                       int64_t input,
                                       const temporal_filter_t* after,
                                       const temporal_filter_result_t* result) {
            trace("Checking input-independent apply properties...");

            trace("- Filter should end up in an internally consistent state.");
            check_any_temporal_filter(after);

            trace("- Input window should be modified in the expected way.");
            ensure_eq(after->next_idx, (before->next_idx + 1) % TEMPORAL_WINDOW);
            for (size_t i = 0; i < TEMPORAL_WINDOW; ++i) {
                ensure_eq(after->window[i],
                          i == before->next_idx ? input
                                                : before->window[i]);
            }

            trace("- Old input reclassification should be consistent with initial state.");
            if (result->previous_not_outlier) {
                ensure_eq(result->previous_input, before->max);
                ensure_gt(before->max, before->upper_tolerance);
                // Importantly, we cannot conclude anything from the state of
                // `after` because old input reclassification may happen right
                // before the old input is discarded from the input window.
            }
        }

        /// Test applying `filter` to `x` with `x < min`
        ///
        /// For at least one such `x` to exist, we need `min > INT64_MIN`.
        UDIPE_NON_NULL_ARGS
        static void check_apply_below_min(temporal_filter_t* filter) {
            assert(filter->min > INT64_MIN);
            const temporal_filter_t before = *filter;
            const int64_t discarded = before.window[before.next_idx];
            const int64_t input =
                filter->min - 1 - rand() % (filter->min - 1 - INT64_MIN);
            tracef("Applying outlier filter to sub-minimum input %zd", input);
            const temporal_filter_result_t result =
                temporal_filter_apply(filter, input);
            check_apply_common(&before, input, filter, &result);

            // Applying to a smaller value will obviously change the minimum
            ensure_eq(filter->min, input);
            ensure_eq(filter->min_count, (uint16_t)1);

            // It will only change the maximum if it replaces it in the input
            // window and there is only one occurence in the input window.
            if (filter->max != before.max) {
                ensure(before.max > before.max_normal
                       || before.max_normal_count == 1);
                ensure_eq(discarded, before.max);
            }

            // The relationship with max_normal is more subtle because reducing
            // min momentarily increases upper_tolerance, which can turn former
            // high outliers into non-outliers. We cannot read the new
            // upper_tolerance from filter for this check because it may have
            // changed again after the second stage of removing an old input.
            const int64_t tmp_upper_tolerance = ceil(
                before.max_normal
                    + (before.max_normal - input) * TEMPORAL_TOLERANCE
            );
            if (before.max > before.max_normal
                && before.max <= tmp_upper_tolerance) {
                ensure(result.previous_not_outlier);
                ensure_eq(result.previous_input, before.max);
                if (filter->max_normal != before.max) {
                    ensure_eq(discarded, before.max);
                }
            } else {
                ensure(!result.previous_not_outlier);
                if (filter->max_normal != before.max_normal) {
                    ensure_eq(discarded, before.max_normal);
                    ensure_eq(before.max_normal_count, (uint16_t)1);
                }
            }

            // Sub-minimum values have all other values above or equal to them,
            // so they cannot be our assumed single high outlier
            ensure(!result.current_is_outlier);
        }

        /// Check a scenario where the input is in `[min; max_normal[`, which
        /// means max and max_normal can only change through evictions
        static void check_max_evictions(const temporal_filter_t* before,
                                        const temporal_filter_t* after) {
            const int64_t discarded = before->window[before->next_idx];
            const bool max_normal_discarded =
                (discarded == before->max_normal
                    && before->max_normal_count == 1);
            if (after->max_normal != before->max_normal) {
                ensure(max_normal_discarded);
            }
            if (after->max != before->max) {
                if (before->max > before->max_normal) {
                    ensure_eq(discarded, before->max);
                } else {
                    ensure(max_normal_discarded);
                }
            }
        }

        /// Check that a run of temporal_filter_apply() neither classified the
        /// current input as an outlier not reclassified a former outlier input
        /// as non-outlier
        ///
        /// This is the outcome for all inputs in range `[min; max_normal]`.
        static void check_result_passthrough(const temporal_filter_result_t* result) {
            ensure(!result->current_is_outlier);
            ensure(!result->previous_not_outlier);
        }

        /// Test applying `filter` to `min`
        ///
        static void check_apply_equal_min(temporal_filter_t* filter) {
            const temporal_filter_t before = *filter;
            const int64_t discarded = before.window[before.next_idx];
            tracef("Applying outlier filter to minimum input %zd", filter->min);
            const temporal_filter_result_t result =
                temporal_filter_apply(filter, filter->min);
            check_apply_common(&before, filter->min, filter, &result);

            // This will preserve min and make its refcount go up unless another
            // occurence of min went away
            ensure_eq(filter->min, before.min);
            if (filter->min_count != before.min_count + 1) {
                ensure_eq(discarded, before.min);
                ensure_eq(filter->min_count, before.min_count);
            }

            // Max and max_normal can only change through evictions
            check_max_evictions(&before, filter);

            // An input in range [min; max_normal] will neither be rejected as
            // an outlier nor lead to the reclassification of a former outlier.
            check_result_passthrough(&result);
        }

        /// Check a scenario where the input is > min, which means min can only
        /// change through evictions
        static void check_min_evictions(const temporal_filter_t* before,
                                        const temporal_filter_t* after) {
            const int64_t discarded = before->window[before->next_idx];
            if (after->min != before->min) {
                ensure_eq(discarded, before->min);
                ensure_eq(before->min_count, (uint16_t)1);
            } else if (after->min_count != before->min_count) {
                ensure_eq(discarded, before->min);
                ensure_eq(after->min_count, before->min_count - 1);
            }
        }

        /// Test applying `filter` to an input in `]min; max_normal[`
        ///
        /// For such an input to exist, we need `max_normal - min > 1`.
        UDIPE_NON_NULL_ARGS
        static
        void check_apply_between_min_and_max_normal(temporal_filter_t* filter) {
            assert(filter->max_normal - filter->min > 1);
            const temporal_filter_t before = *filter;
            const int64_t input =
                filter->min + 1
                    + rand() % (filter->max_normal - filter->min - 1);
            tracef("Applying outlier filter to normal input %zd", input);
            const temporal_filter_result_t result =
                temporal_filter_apply(filter, input);
            check_apply_common(&before, input, filter, &result);

            // This will only change the min through evictions
            check_min_evictions(&before, filter);

            // This will only change max_normal and max through evictions
            check_max_evictions(&before, filter);

            // An input in range [min; max_normal] will neither be rejected as
            // an outlier nor lead to the reclassification of a former outlier.
            check_result_passthrough(&result);
        }

        /// Test applying `filter` to `max_normal`, which is assumed to be
        /// distinct from `min`.
        UDIPE_NON_NULL_ARGS
        static void check_apply_equal_max_normal(temporal_filter_t* filter) {
            assert(filter->max_normal > filter->min);
            const temporal_filter_t before = *filter;
            const int64_t discarded = before.window[before.next_idx];
            tracef("Applying outlier filter to max normal input %zd",
                   filter->max_normal);
            const temporal_filter_result_t result =
                temporal_filter_apply(filter, filter->max_normal);
            check_apply_common(&before, filter->max_normal, filter, &result);

            // This will only change the min through evictions
            check_min_evictions(&before, filter);

            // This will preserve max_normal and make its refcount go up unless
            // another occurence of max_normal went away
            ensure_eq(filter->max_normal, before.max_normal);
            if (filter->max_normal_count != before.max_normal_count + 1) {
                ensure_eq(discarded, before.max_normal);
                ensure_eq(filter->max_normal_count, before.max_normal_count);
            }

            // This will only change max through evictions, and only if it was
            // an outlier other than max_normal. In this case max_normal will
            // become the new maximum.
            if (filter->max != before.max) {
                ensure_eq(discarded, before.max);
                ensure_eq(filter->max, before.max_normal);
            }

            // An input in range [min; max_normal] will neither be rejected as
            // an outlier nor lead to the reclassification of a former outlier.
            check_result_passthrough(&result);
        }

        /// Test applying `filter` to an input in `]max_normal; max[`
        ///
        /// For such an input to exist, we need `max - max_normal > 1`, which
        /// implies that `max` is currently classified as an outlier.
        UDIPE_NON_NULL_ARGS
        static
        void check_apply_between_max_normal_and_max(temporal_filter_t* filter) {
            assert(filter->max - filter->max_normal > 1);
            const temporal_filter_t before = *filter;
            const int64_t discarded = before.window[before.next_idx];
            const int64_t input =
                filter->max_normal + 1
                    + rand() % (filter->max - filter->max_normal - 1);
            tracef("Applying outlier filter to above-normal input %zd", input);
            const temporal_filter_result_t result =
                temporal_filter_apply(filter, input);
            check_apply_common(&before, input, filter, &result);

            // This will only change the min through evictions
            check_min_evictions(&before, filter);

            // This will interact with max and max_normal in complex ways:
            //
            // - Upon insertion, the new input will become the new max_normal,
            //   which will increase upper_tolerance.
            // - This increase of upper_tolerance may have the effect of
            //   reclassifying the former outlier max into a non-outlier. In
            //   this case, before.max will become max_normal, and the result
            //   will be set up to notify of input reclassification.
            // - Later, at the stage where the oldest input is discarded, that
            //   oldest input may turn out to be before.max. In this case, the
            //   filter will go back to a state where the new input is
            //   max_normal. We know it is normal because it momentarily
            //   coexisted with a higher maximum, so classifying it as an
            //   outlier would violate our hypothesis that there is at most one
            //   outlier per (momentarily extended) input window.
            const int64_t upper_tolerance_after_input = ceil(
                input + (input - before.min) * TEMPORAL_TOLERANCE
            );
            if (before.max <= upper_tolerance_after_input) {
                ensure(result.previous_not_outlier);
                ensure_eq(result.previous_input, before.max);
                const int64_t final_single_max_normal =
                    (discarded == before.max) ? input : before.max;
                ensure_eq(filter->max, final_single_max_normal);
                ensure_eq(filter->max_normal, final_single_max_normal);
                ensure_eq(filter->max_normal_count, (uint16_t)1);
            } else {
                ensure(!result.previous_not_outlier);
                ensure_eq(filter->max_normal, input);
                ensure_eq(filter->max_normal_count, (uint16_t)1);
                if (filter->max != before.max) {
                    ensure_eq(discarded, before.max);
                    ensure_eq(filter->max, input);
                }
            }

            // before.max was above input so input can never be an outlier
            ensure(!result.current_is_outlier);
        }

        /// Test applying `filter` to `max`, which is assumed to be distinct
        /// from `max_normal`. This implies that `max` is currently classified
        /// as an outlier.
        UDIPE_NON_NULL_ARGS
        static void check_apply_equal_max(temporal_filter_t* filter) {
            assert(filter->max > filter->max_normal);
            const temporal_filter_t before = *filter;
            const int64_t discarded = before.window[before.next_idx];
            tracef("Applying outlier filter to max input %zd",
                   filter->max);
            const temporal_filter_result_t result =
                temporal_filter_apply(filter, filter->max);
            check_apply_common(&before, filter->max, filter, &result);

            // This will only change the min through evictions
            check_min_evictions(&before, filter);

            // By virtue of having seen two occurences of max, we know that max
            // was not an outlier after all, and since it was freshly inserted
            // it will still be max_normal in the final filter state.
            ensure_eq(filter->max, before.max);
            ensure_eq(filter->max_normal, before.max);
            if (filter->max_normal_count != 2) {
                ensure_eq(discarded, before.max);
                ensure_eq(filter->max_normal_count, 1);
            }
            ensure(!result.current_is_outlier);
            ensure(result.previous_not_outlier);
            ensure_eq(result.previous_input, before.max);
        }

        /// Test applying `filter` to `x` with `x > max`
        ///
        /// For at least one such `x` to exist, we need `max < INT64_MAX`.
        UDIPE_NON_NULL_ARGS
        static void check_apply_above_max(temporal_filter_t* filter) {
            assert(filter->max < INT64_MAX);
            const temporal_filter_t before = *filter;
            const int64_t discarded = before.window[before.next_idx];
            const int64_t input =
                filter->max + 1 + rand() % (INT64_MAX - filter->max - 1);
            tracef("Applying outlier filter to above-max input %zd", input);
            const temporal_filter_result_t result =
                temporal_filter_apply(filter, input);
            check_apply_common(&before, input, filter, &result);

            // This will only change the min through evictions
            check_min_evictions(&before, filter);

            // By definition of the maximum, this value must become max
            ensure_eq(filter->max, input);

            // The effect on max_normal and result, however, is more
            // complicated.
            //
            // First, if the former max was considered an outlier, that judgment
            // is revised (since we can't have two outliers), which makes the
            // former outlier max temporarily become the new max_normal.
            int64_t max_normal_after_input;
            uint16_t max_normal_count_after_input;
            if (before.max > before.max_normal) {
                ensure(result.previous_not_outlier);
                ensure_eq(result.previous_input, before.max);
                max_normal_after_input = before.max;
                max_normal_count_after_input = 1;
            } else {
                ensure(!result.previous_not_outlier);
                max_normal_after_input = before.max_normal;
                max_normal_count_after_input = before.max_normal_count;
            }
            // As a result, upper_tolerance gets a possibly different value...
            const int64_t upper_tolerance_after_input = ceil(
                before.max + (before.max - before.min) * TEMPORAL_TOLERANCE
            );
            // ...which may, in turn, affect the decision to classify the new
            // isolated maximal input as an outlier or not.
            ensure_eq(result.current_is_outlier,
                      input > upper_tolerance_after_input);
            if (result.current_is_outlier) {
                // If the input is classified as an outlier, then max_normal
                // will retain its former value unless the last occurence
                // disappears through evictions.
                const bool discarded_max_normal =
                    (discarded == max_normal_after_input);
                const uint16_t max_normal_count_after_discard =
                    max_normal_count_after_input - (uint16_t)discarded_max_normal;
                if (filter->max_normal == max_normal_after_input) {
                    ensure_ge(max_normal_count_after_discard, (uint16_t)1);
                } else {
                    ensure_eq(max_normal_count_after_discard, (uint16_t)0);
                }
            } else {
                // If the input is not considered an outlier, then it will
                // become max_normal and stay max_normal through evictions as a
                // newly introduced input won't be evicted.
                ensure_eq(filter->max_normal, input);
                ensure_eq(filter->max_normal_count, 1);
            }
        }

        /// Test temporal_filter_t
        ///
        static void test_temporal_filter() {
            tracef("Testing the outlier filter from %zu initial states...",
                   NUM_INITIAL_STATES);
            for (size_t state = 0; state < NUM_INITIAL_STATES; ++state) {
                trace("- Generating initial inputs...");
                int64_t window[TEMPORAL_WINDOW];
                for (size_t i = 0; i < TEMPORAL_WINDOW; ++i) {
                    // This random distribution ensures at least one repetition,
                    // some negative values, and enough spread to see rounding
                    // error in upper_tolerance computations.
                    window[i] = (rand() % (TEMPORAL_WINDOW-1) - TEMPORAL_WINDOW/3) * 10;
                    tracef("  * window[%zu] = %zd", i, window[i]);
                }

                trace("- Initializing filter...");
                temporal_filter_t filter = checked_temporal_filter(window);

                // TODO: Track last rejected value + its age to check if each
                //       rejection is valid. This includes the possible
                //       rejection within the initial input window.

                trace("- Applying filter to more inputs...");
                for (size_t i = 0; i < NUM_APPLY_CALLS; ++i) {
                    retry:
                    apply_kind_t kind = rand() % APPLY_KIND_LEN;
                    switch (kind) {
                    case APPLY_BELOW_MIN:
                        if (filter.min == INT64_MIN) goto retry;
                        check_apply_below_min(&filter);
                        break;
                    case APPLY_EQUAL_MIN:
                        check_apply_equal_min(&filter);
                        break;
                    case APPLY_BETWEEN_MIN_AND_MAX_NORMAL:
                        if (filter.max_normal - filter.min <= 1) goto retry;
                        check_apply_between_min_and_max_normal(&filter);
                        break;
                    case APPLY_EQUAL_MAX_NORMAL:
                        if (filter.max_normal == filter.min) goto retry;
                        check_apply_equal_max_normal(&filter);
                        break;
                    case APPLY_BETWEEN_MAX_NORMAL_AND_MAX:
                        if (filter.max - filter.max_normal <= 1) goto retry;
                        check_apply_between_max_normal_and_max(&filter);
                        break;
                    case APPLY_EQUAL_MAX:
                        if (filter.max == filter.max_normal) goto retry;
                        check_apply_equal_max(&filter);
                        break;
                    case APPLY_ABOVE_MAX:
                        if (filter.max == INT64_MAX) goto retry;
                        check_apply_above_max(&filter);
                        break;
                    case APPLY_KIND_LEN:
                        exit_with_error("Cannot happen by construction!");
                    }
                }
            }
        }

        /// Test distribution_builder_t and distribution_t
        ///
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

            debug("Running temporal outlier filter unit tests...");
            with_log_level(UDIPE_TRACE, {
                test_temporal_filter();
            });

            debug("Running distribution unit tests...");
            with_log_level(UDIPE_TRACE, {
                test_distibution();
            });

            // TODO: Add unit tests for stats, then clocks

            // TODO: Test other components
        }

    #endif  // UDIPE_BUILD_TESTS

#endif  // UDIPE_BUILD_BENCHMARKS