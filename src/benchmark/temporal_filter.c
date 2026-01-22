#ifdef UDIPE_BUILD_BENCHMARKS

    #include "temporal_filter.h"

    #include <udipe/pointer.h>

    #include "../error.h"
    #include "../unit_tests.h"

    #include <assert.h>
    #include <math.h>
    #include <stddef.h>
    #include <stdint.h>


    // === Implementation details ===

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
                                        const char reason[]) {
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


    // === Public API ===

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

        void temporal_filter_unit_tests() {
            infof("Running temporal_filter_t tests from %zu initial states...",
                  NUM_INITIAL_STATES);
            configure_rand();
            with_log_level(UDIPE_TRACE, {
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
                    size_t num_rejections = 0;
                    for (size_t i = 0; i < NUM_APPLY_CALLS + num_rejections; ++i) {
                        apply_kind_t kind = rand() % APPLY_KIND_LEN;
                        switch (kind) {
                        case APPLY_BELOW_MIN:
                            if (filter.min == INT64_MIN) {
                                ++num_rejections;
                                continue;
                            }
                            check_apply_below_min(&filter);
                            break;
                        case APPLY_EQUAL_MIN:
                            check_apply_equal_min(&filter);
                            break;
                        case APPLY_BETWEEN_MIN_AND_MAX_NORMAL:
                            if (filter.max_normal - filter.min <= 1) {
                                ++num_rejections;
                                continue;
                            }
                            check_apply_between_min_and_max_normal(&filter);
                            break;
                        case APPLY_EQUAL_MAX_NORMAL:
                            if (filter.max_normal == filter.min) {
                                ++num_rejections;
                                continue;
                            }
                            check_apply_equal_max_normal(&filter);
                            break;
                        case APPLY_BETWEEN_MAX_NORMAL_AND_MAX:
                            if (filter.max - filter.max_normal <= 1) {
                                ++num_rejections;
                                continue;
                            }
                            check_apply_between_max_normal_and_max(&filter);
                            break;
                        case APPLY_EQUAL_MAX:
                            if (filter.max == filter.max_normal) {
                                ++num_rejections;
                                continue;
                            }
                            check_apply_equal_max(&filter);
                            break;
                        case APPLY_ABOVE_MAX:
                            if (filter.max == INT64_MAX) {
                                ++num_rejections;
                                continue;
                            }
                            check_apply_above_max(&filter);
                            break;
                        case APPLY_KIND_LEN:
                            exit_with_error("Cannot happen by construction!");
                        }
                    }
                }
            });
        }

    #endif  // UDIPE_BUILD_TESTS

#endif  // UDIPE_BUILD_BENCHMARKS