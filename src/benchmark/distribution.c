#ifdef UDIPE_BUILD_BENCHMARKS

    #include "distribution.h"

    #include <udipe/pointer.h>

    #include "../error.h"
    #include "../log.h"
    #include "../memory.h"
    #include "../unit_tests.h"

    #include <assert.h>
    #include <stdalign.h>
    #include <stddef.h>
    #include <stdint.h>
    #include <string.h>


    /// Logical size of a bin from a \ref distribution_t
    ///
    /// \ref distribution_t internally uses a structure-of-array layout, so it
    /// is not literally an array of `(int64_t, size_t)` pairs but rather an
    /// array of `int64_t` followed by an array of `size_t`.
    static const size_t DISTRIBUTION_BIN_SIZE = sizeof(int64_t) + sizeof(size_t);
    static_assert(alignof(int64_t) >= alignof(size_t));


    // === Implementation details ===

    distribution_t distribution_allocate(size_t capacity) {
        void* const allocation = malloc(capacity * DISTRIBUTION_BIN_SIZE);
        exit_on_null(allocation, "Failed to allocate distribution storage");
        debugf("Allocated storage for %zu bins at location %p.",
               capacity, allocation);
        return (distribution_t){
            .allocation = allocation,
            .num_bins = 0,
            .capacity = capacity
        };
    }

    UDIPE_NON_NULL_ARGS
    void distribution_create_bin(distribution_builder_t* builder,
                                 size_t pos,
                                 int64_t value) {
        distribution_t* dist = &builder->inner;
        assert(pos <= dist->num_bins);
        distribution_layout_t layout = distribution_layout(dist);
        if (pos > 0) assert(layout.sorted_values[pos - 1] < value);
        if (pos < dist->num_bins) assert(layout.sorted_values[pos] > value);

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


    // === Building distributions from a stream of values ===

    distribution_builder_t distribution_initialize() {
        const size_t capacity = get_page_size() / DISTRIBUTION_BIN_SIZE;
        return (distribution_builder_t){
            .inner = distribution_allocate(capacity)
        };
    }

    UDIPE_NON_NULL_ARGS
    distribution_t distribution_build(distribution_builder_t* builder) {
        trace("Extracting the distribution from the builder...");
        distribution_t dist = builder->inner;
        distribution_poison(&builder->inner);

        trace("Ensuring the distribution is not empty...");
        ensure_ge(dist.num_bins, (size_t)1);

        trace("Turning value counts into end ranks...");
        distribution_layout_t layout = distribution_layout(&dist);
        size_t end_rank = 0;
        for (size_t bin = 0; bin < dist.num_bins; ++bin) {
            end_rank += layout.counts[bin];
            layout.end_ranks[bin] = end_rank;
        }
        return dist;
    }

    UDIPE_NON_NULL_ARGS
    void distribution_discard(distribution_builder_t* builder) {
        distribution_finalize(&builder->inner);
    }


    // === Building distributions from other distributions ===

    UDIPE_NON_NULL_ARGS
    distribution_t distribution_scale(distribution_builder_t* builder,
                                      int64_t factor,
                                      const distribution_t* dist) {
        ensure_eq(builder->inner.num_bins, (size_t)0);

        if (factor == 0) {
            trace("Handling zero factor special case...");
            ensure_ge(builder->inner.capacity, (size_t)1);
            distribution_layout_t builder_layout = distribution_layout(&builder->inner);
            builder_layout.sorted_values[0] = 0;
            builder_layout.counts[0] = distribution_len(dist);
            builder->inner.num_bins = 1;
            return distribution_build(builder);
        }

        const size_t num_bins = dist->num_bins;
        if (builder->inner.capacity < num_bins) {
            tracef("Enlarging builder to match input capacity %zu...",
                   dist->capacity);
            distribution_finalize(&builder->inner);
            builder->inner = distribution_allocate(dist->capacity);
        }

        ensure_ne(factor, (int64_t)0);
        trace("Handling nonzero factor, flipping bin order if negative...");
        const distribution_layout_t dist_layout = distribution_layout(dist);
        distribution_layout_t builder_layout = distribution_layout(&builder->inner);
        size_t prev_end_rank = 0;
        for (size_t dist_pos = 0; dist_pos < num_bins; ++dist_pos) {
            const int64_t dist_value = dist_layout.sorted_values[dist_pos];
            const int64_t scaled_value = factor * dist_value;

            const size_t dist_end_rank = dist_layout.end_ranks[dist_pos];
            assert(dist_end_rank > prev_end_rank);  // Nonzero positive count
            const size_t count = dist_end_rank - prev_end_rank;
            prev_end_rank = dist_end_rank;

            const size_t builder_pos = (factor > 0) ? dist_pos
                                                    : num_bins - dist_pos - 1;

            tracef("- Input bin #%zu with %zu occurences of %zd becomes "
                   "output bin #%zu with as many occurences of scaled %zd.",
                   dist_pos, count, dist_value,
                   builder_pos, scaled_value);
            builder_layout.sorted_values[builder_pos] = scaled_value;
            builder_layout.counts[builder_pos] = count;
        }
        builder->inner.num_bins = dist->num_bins;
        return distribution_build(builder);
    }

    UDIPE_NON_NULL_ARGS
    distribution_t distribution_sub(distribution_builder_t* builder,
                                    const distribution_t* left,
                                    const distribution_t* right) {
        ensure_eq(builder->inner.num_bins, (size_t)0);

        // To avoid "amplifying" outliers by using multiple copies, we iterate
        // over the shortest distribution and sample from the longest one
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
        size_t prev_short_end_rank = 0;
        for (size_t short_pos = 0; short_pos < short_bins; ++short_pos) {
            const int64_t short_value = short_layout.sorted_values[short_pos];
            const size_t short_end_rank = short_layout.end_ranks[short_pos];
            const size_t short_count = short_end_rank - prev_short_end_rank;
            tracef("- Bin #%zu contains %zu occurences of value %zd.",
                   short_pos, short_count, short_value);
            for (size_t long_sample = 0; long_sample < short_count; ++long_sample) {
                const int64_t diff = short_value - distribution_choose(longer);
                tracef("  * Random short-long difference is %zd.", diff);
                const int64_t signed_diff = diff_sign * diff;
                tracef("  * Random left-right difference is %zd.", signed_diff);
                distribution_insert(builder, signed_diff);
            }
            prev_short_end_rank = short_end_rank;
        }
        return distribution_build(builder);
    }

    UDIPE_NON_NULL_ARGS
    distribution_t distribution_scaled_div(distribution_builder_t* builder,
                                           const distribution_t* num,
                                           int64_t factor,
                                           const distribution_t* denom) {
        ensure_eq(builder->inner.num_bins, (size_t)0);

        // To avoid "amplifying" outliers by using multiple copies, we iterate
        // over the shortest distribution and sample from the longest one
        if (distribution_len(num) <= distribution_len(num)) {
            trace("Numerator distribution is shorter, will iterate over num and sample from denom.");
            const distribution_layout_t num_layout = distribution_layout(num);
            const size_t num_bins = num->num_bins;
            tracef("Iterating over the %zu bins of the numerator distribution...",
                   num_bins);
            size_t prev_end_rank = 0;
            for (size_t num_pos = 0; num_pos < num_bins; ++num_pos) {
                const int64_t num_value = num_layout.sorted_values[num_pos];
                const size_t curr_end_rank = num_layout.end_ranks[num_pos];
                const size_t num_count = curr_end_rank - prev_end_rank;
                tracef("- Numerator bin #%zu contains %zu occurences of value %zd.",
                       num_pos, num_count, num_value);
                for (size_t denom_sample = 0; denom_sample < num_count; ++denom_sample) {
                    const int64_t denom_value = distribution_choose(denom);
                    tracef("  * Sampled random denominator value %zd.", denom_value);
                    const int64_t scaled_ratio = num_value * factor / denom_value;
                    tracef("  * Scaled ratio sample is %zd.", scaled_ratio);
                    distribution_insert(builder, scaled_ratio);
                }
                prev_end_rank = curr_end_rank;
            }
            return distribution_build(builder);
        } else {
            trace("Denominator distribution is shorter, will iterate over denom and sample from num.");
            const distribution_layout_t denom_layout = distribution_layout(denom);
            const size_t denom_bins = denom->num_bins;
            tracef("Iterating over the %zu bins of the denominator distribution...",
                   denom_bins);
            size_t prev_end_rank = 0;
            for (size_t denom_pos = 0; denom_pos < denom_bins; ++denom_pos) {
                const int64_t denom_value = denom_layout.sorted_values[denom_pos];
                const size_t curr_end_rank = denom_layout.end_ranks[denom_pos];
                const size_t denom_count = curr_end_rank - prev_end_rank;
                tracef("- Denominator bin #%zu contains %zu occurences of value %zd.",
                       denom_pos, denom_count, denom_value);
                for (size_t num_sample = 0; num_sample < denom_count; ++num_sample) {
                    const int64_t num_value = distribution_choose(num);
                    tracef("  * Sampled random numerator value %zd.", num_value);
                    const int64_t scaled_ratio = num_value * factor / denom_value;
                    tracef("  * Scaled ratio sample is %zd.", scaled_ratio);
                    distribution_insert(builder, scaled_ratio);
                }
                prev_end_rank = curr_end_rank;
            }
            return distribution_build(builder);
        }
    }


    // === Querying distributions ===

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


    #ifdef UDIPE_BUILD_TESTS

        /// Test distribution_builder_t and distribution_t
        ///
        static void test_distribution() {
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

            ensure_le((uint64_t)RAND_MAX, (uint64_t)INT64_MAX);
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
                builder.inner.capacity * DISTRIBUTION_BIN_SIZE;
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
            int64_t value;
            size_t insert_pos;
            bool rejected;
            do {
                value = rand() - RAND_MAX / 2;
                tracef("- Checking candidate value %zd...", value);
                insert_pos = SIZE_MAX;
                rejected = false;
                for (size_t pos = 0; pos < builder.inner.num_bins; ++pos) {
                    if (layout.sorted_values[pos] < value) {
                        continue;
                    } else if (layout.sorted_values[pos] > value) {
                        insert_pos = pos;
                    } else {
                        assert(layout.sorted_values[pos] == value);
                        tracef("  * Value exists in bin #%zu, try again...",
                               pos);
                        rejected = true;
                    }
                    break;
                }
                if (insert_pos == SIZE_MAX) insert_pos = builder.inner.num_bins;
            } while(rejected);
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
            allocation_size = builder.inner.capacity * DISTRIBUTION_BIN_SIZE;
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
            size_t* prev_end_ranks = prev_counts;
            for (size_t bin = 0; bin < dist.num_bins; ++bin) {
                ensure_eq(layout.sorted_values[bin], prev_values[bin]);
                expected_end_idx += prev_counts[bin];
                ensure_eq(layout.end_ranks[bin], expected_end_idx);
                prev_end_ranks[bin] = expected_end_idx;
            }
            prev_counts = NULL;
            ensure_eq(distribution_len(&dist), expected_end_idx);

            trace("Testing distribution sampling...");
            const size_t num_samples = 10 * dist.num_bins;
            for (size_t i = 0; i < num_samples; ++i) {
                trace("- Grabbing one sample...");
                const int64_t sample = distribution_choose(&dist);

                trace("- Checking const correctness and locating sampled bin...");
                size_t sampled_bin = SIZE_MAX;
                for (size_t bin = 0; bin < dist.num_bins; ++bin) {
                    if (layout.sorted_values[bin] == sample) sampled_bin = bin;
                    ensure_eq(layout.sorted_values[bin], prev_values[bin]);
                    ensure_eq(layout.end_ranks[bin], prev_end_ranks[bin]);
                }
                ensure_ne(sampled_bin, SIZE_MAX);
            }

            trace("Deallocating backup storage...");
            free(prev_data);
            prev_data = NULL;
            prev_values = NULL;
            prev_end_ranks = NULL;

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

            // TODO: Break up in smaller functions + test newer functionality
            //       like scale, sub, scaled_div...
        }

        void distribution_unit_tests() {
            info("Testing distributions of duration-based values...");
            configure_rand();
            with_log_level(UDIPE_TRACE, {
                test_distribution();
            });
        }

    #endif  // UDIPE_BUILD_TESTS

#endif  // UDIPE_BUILD_BENCHMARKS