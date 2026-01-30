#ifdef UDIPE_BUILD_BENCHMARKS

    #include "numeric.h"

    #include "../bits.h"
    #include "../error.h"
    #include "../unit_tests.h"

    #include <assert.h>
    #include <math.h>
    #include <stddef.h>
    #include <stdint.h>
    #include <stdlib.h>
    #include <string.h>


    /// Comparison function used by sort_f64_exp_sign_mantissa()
    ///
    /// It assumes the input dataset is free of NaNs and sorts it...
    ///
    /// - First by buckets of increasing exponent
    /// - Then, within each exponent bucket, by sign (negative then positive)
    /// - Then, within each exponent/sign bucket, by increasing mantissa
    ///
    /// \param v1 is a `const double*` to a number that is not NaN.
    /// \param v2 has the same properties as `v1`.
    ///
    /// \returns -1 if `v1` goes before `v2`, +1 if `v1` goes after `v2`, and 0
    ///          if they are considered equal to each other.
    UDIPE_NON_NULL_ARGS
    static inline int compare_f64_exp_sign_mantissa(const void* v1,
                                                    const void* v2) {
        const double d1 = *((const double*)v1);
        const double d2 = *((const double*)v2);
        assert(!isunordered(d1, d2));

        // Split exponent/fraction
        int exp1, exp2;
        const double frac1 = frexp(d1, &exp1);
        const double frac2 = frexp(d2, &exp2);

        // Low exponent before high exponent
        if (exp1 < exp2) return -1;
        if (exp1 > exp2) return 1;

        // Negative before positive
        if (signbit(frac1) > signbit(frac2)) return -1;
        if (signbit(frac1) < signbit(frac2)) return 1;

        // Small mantissa before large mantissa
        if (fabs(frac1) < fabs(frac2)) return -1;
        if (fabs(frac1) > fabs(frac2)) return 1;
        return 0;
    }

    /// Sort numbers in place by exponent, sign and mantissa
    ///
    /// This sorts `array` using the comparison operator defined by
    /// compare_f64_exp_sign_mant().
    ///
    /// \param array must point to an array of at least `length` numbers, which
    ///              will be reordered. It should not contain NaNs.
    /// \param length indicates how many elements of `array` must be summed
    UDIPE_NON_NULL_ARGS
    static void sort_f64_exp_sign_mantissa(double array[], size_t length) {
        qsort(array,
              length,
              sizeof(double),
              compare_f64_exp_sign_mantissa);
    }

    /// Perform one pass of pairwise summation on an array of numbers which are
    /// assumed to have the same exponent and sign, packing the results on the
    /// right hand side of the array
    ///
    /// \param homogeneous must point to an array of at least `length` numbers,
    ///                    assumed to initially have the same exponent and sign,
    ///                    without NaNs. It will be modified during the
    ///                    summation process.
    /// \param length indicates how many elements are present in `homogeneous`,
    ///               which must be at least two elements (otherwise this
    ///               operation makes no sense).
    ///
    /// \returns the number of elements that have been integrated through
    ///          pairwise summation, i.e. the shift that should next be applied
    ///          upwards to the array pointer and downwards to its length in a
    ///          recursive pairwise sum procedure.
    UDIPE_NON_NULL_ARGS
    static size_t sum_same_exp_sign_pass(double homogeneous[], size_t length) {
        // Check preconditions
        assert(length >= 2);

        // Split the dataset in two equal sized halves, and possibly a
        // remainder element if length is odd.
        const size_t half_length = length / 2;
        const size_t second_half_start = length - half_length;
        const size_t first_half_start = second_half_start - half_length;
        const size_t remainder_pos = 0;

        // Sum the upper half into the lower half
        for (size_t i = 0; i < half_length; ++i) {
            const double left = homogeneous[first_half_start + i];
            double* const right_ptr = &homogeneous[second_half_start + i];
            assert(!isunordered(left, *right_ptr)
                   && signbit(left) == signbit(*right_ptr));
            *right_ptr += left;
        }

        // If there is a remainder element, sum it with a random other element.
        //
        // This randomization ensures that there is not one single array
        // element which always serves an accumulator for all remainders,
        // leading its magnitude grows out of control with respect to other
        // array elements, as in a recursive sum this would degrade precision.
        if (remainder_pos < first_half_start) {
            const double remainder = homogeneous[remainder_pos];
            const size_t last_idx = length - 1;
            const size_t target_from_last = rand() % half_length;
            double* const target_ptr = &homogeneous[last_idx - target_from_last];
            assert(!isunordered(remainder, *target_ptr)
                   && signbit(remainder) == signbit(*target_ptr));
            *target_ptr += remainder;
        }
        return length - half_length;
    }

    /// Sum a bunch of floating-point numbers in place under the assumption that
    /// they all have the same exponent and sign
    ///
    /// \param homogeneous must point to an array of at least `length` numbers,
    ///                    assumed to initially have the same exponent and sign,
    ///                    without NaNs. It will be modified during the
    ///                    summation process.
    /// \param length indicates how many elements of `homogeneous` must be summed.
    ///
    /// \returns the sum of the elements of `homogeneous`.
    UDIPE_NON_NULL_ARGS
    static double sum_same_exp_sign(double homogeneous[], size_t length) {
        // Handle special cases
        if (length == 0) return 0.0;

        // Accumulate values through pairwise sum
        while (length >= 2) {
            const size_t num_integrated = sum_same_exp_sign_pass(homogeneous,
                                                                 length);
            homogeneous += num_integrated;
            length -= num_integrated;
        }
        ensure_eq(length, (size_t)1);
        return homogeneous[0];
    }

    /// Within an array of numbers that is partitioned by sign with negative
    /// numbers (if any) at the start and positive numbers (if any) at the end,
    /// find the index of the first positive number (if any)
    ///
    /// \param posneg must point to an array of at least `length` numbers,
    ///                    partitioned by sign such that negative numbers are
    ///                    packed at the start and positive numbers are packed
    ///                    at the end. It should not contain NaNs.
    /// \param length indicates how many elements of `posneg` must be included
    ///               in the search.
    ///
    /// \returns the index of the first positive number, if any, or `SIZE_MAX`
    ///          if there is no positive number.
    UDIPE_NON_NULL_ARGS
    static size_t find_first_positive(const double posneg[], size_t length) {
        // Handle special cases
        if (length == 0) return SIZE_MAX;
        if (length == 1) {
            assert(!isnan(*posneg));
            return signbit(*posneg) ? SIZE_MAX : 0;
        }

        // Check if there is at least one positive number
        const size_t last_idx = length - 1;
        const double last = posneg[last_idx];
        assert(!isnan(last));
        if (signbit(last)) return SIZE_MAX;

        // Check if there is at least one negative number
        const double first = posneg[0];
        assert(!isnan(first));
        if (!signbit(first)) return 0;

        // There is a partition point, find it via binary search
        size_t last_negative_idx = 0;
        size_t first_positive_idx = last_idx;
        while (first_positive_idx - last_negative_idx > 1) {
            const size_t mid_idx = last_negative_idx
                                 + (first_positive_idx - last_negative_idx) / 2;
            const double mid_value = posneg[mid_idx];
            assert(!isnan(mid_value));
            if (signbit(mid_value)) {
                last_negative_idx = mid_idx;
            } else {
                first_positive_idx = mid_idx;
            }
        }
        assert(first_positive_idx == last_negative_idx + 1);
        return first_positive_idx;
    }

    /// Perform canceling sums between numbers of identical exponent and
    /// opposite sign
    ///
    /// Within an array of numbers of identical exponent that are partitioned by
    /// sign (negative before positive), featuring at least 1 negative number
    /// and 1 positive number, this method performs a pairwise summation pass
    /// between numbers of opposite sign, ensuring that at the end...
    ///
    /// - All remaining numbers (cancelation results then untouched numbers) end
    ///   up lying on the right-hand side of the array.
    /// - End results are sorted by exponent, then by sign, then by mantissa.
    ///
    /// \param posneg is the input array, which should have at least `length`
    ///               elements of identical exponent, excluding NaNs.
    /// \param length is the length of `posneg` that will be considered.
    /// \param first_positive_idx is the index of the first positive number
    ///                           inside of `posneg`. All elements of `posneg`
    ///                           with `index < first_positive_idx` should be
    ///                           negative and all elements with `index >=
    ///                           first_positive_idx` should be positive.
    ///
    /// \returns the number of sums that have been computed, which is the amount
    ///          by which the array pointer should be shifted up and the length
    ///          counter should be shifted down.
    ///
    /// \internal
    ///
    /// For some background on what we are doing here, canceling floating-point
    /// sums are the trickiest part of sum_f64() because they are subjected to
    /// two competing constraints:
    ///
    /// - On one hand, like all floating point sums, canceling sums are most
    ///   accurate when summing numbers of identical magnitude. Otherwise if we
    ///   denote N the exponent of the first number and M the exponent of the
    ///   second number and assume N <= M, M - N bits of mantissa from the
    ///   smallest number are lost to rounding error. If this happens over many
    ///   sums, it can cause unbounded accumulation error.
    /// - On the other hand, for numbers of equal magnitude and opposite sign,
    ///   if we denote M the total number of mantissa bits and L the number of
    ///   identical leading digits in the mantissas of the positive and negative
    ///   numbers, the result of a canceling sum only has M - L - 1 correct
    ///   leading mantissa bits with respect the the sum of exact results, with
    ///   the remaining mantissa bits being zero when they wouldn't be if
    ///   computations were carried with infinite precision. This means that
    ///   inaccurate low-order bits in the mantissa of the numbers being summed
    ///   "bubble up" the mantissa, and that in turn leads to a desire to sum
    ///   numbers of opposite sign as early as possible so that those trailing
    ///   mantissa bits are as close to the true result as possible.
    ///
    /// The way we currently approach this compromise is that we sum same-sign
    /// numbers of low magnitude until they reach the same magnitude as a number
    /// of opposite sign to minimize rounding error, then perform the
    /// cancelation as early as possible to maximize the number of correct bits
    /// after cancelation.
    UDIPE_NON_NULL_ARGS
    static size_t cancel_and_shift(size_t first_positive_idx,
                                   double posneg[],
                                   size_t length) {
        // Determine the amount of positive and negative numbers
        assert(first_positive_idx < length);
        const size_t num_negatives = first_positive_idx;
        assert(num_negatives >= 1);
        const size_t num_positives = length - first_positive_idx;
        assert(num_positives >= 1);

        // Compute all of the canceling positive/negative sums
        size_t num_canceling_sums;
        if (num_positives >= num_negatives) {
            // Accumulate the smaller lower negative half into the larger upper
            // positive half.
            num_canceling_sums = num_negatives;
            for (size_t n = 0; n < num_negatives; ++n) {
                double* const positive_ptr = &posneg[first_positive_idx + n];
                assert(*positive_ptr >= 0.0);
                const double negative = posneg[n];
                assert(!isunordered(*positive_ptr, negative) && negative < 0.0);
                *positive_ptr += negative;
            }
        } else {
            // Accumulate the smaller upper positive half into the larger lower
            // negative half.
            assert(num_positives < num_negatives);
            num_canceling_sums = num_positives;
            for (size_t p = 0; p < num_positives; ++p) {
                double* const negative_ptr = &posneg[p];
                const double positive = posneg[first_positive_idx + p];
                assert(!isunordered(*negative_ptr, positive));
                *negative_ptr += positive;
            }

            // Shift all formerly negative data right to get the desired final
            // layout where data is packed on the right
            //
            // This could get expensive but shouldn't happen often because in
            // timing statistics we are interested in mostly positive numbers
            // which means that the branch where num_positives < num_negatives
            // should be rare.
            const size_t last_idx = length - 1;
            const size_t last_negative_idx = first_positive_idx - 1;
            for (size_t n = 0; n < num_negatives; ++n) {
                posneg[last_idx - n] = posneg[last_negative_idx - n];
            }
        }

        // Restore expected data ordering for the next processing pass
        //
        // The output of a canceling sum has a strictly lower exponent than its
        // inputs as the implicit leading set bit of the fraction is always
        // canceled and conceptually replaced by the leading mantissa bit that
        // differs between the two addends.
        //
        // Therefore only canceled outputs need to be re-sorted to restore the
        // expected ordering, as the numbers that weren't summed are guaranteed
        // to fall into a different (higher) exponent bucket.
        sort_f64_exp_sign_mantissa(posneg + num_canceling_sums,
                                   num_canceling_sums);
        return num_canceling_sums;
    }

    /// Find the start of the second exponent bucket, if any
    ///
    /// Within an array of numbers that are sorted by exponent and doesn't
    /// include NaNs, this function locates the first number (if any) whose
    /// exponent differs from that of the first array element (if any).
    ///
    /// \param sorted must point to an array of at least `length` numbers,
    ///                    sorted by exponent. It should not contain NaNs.
    /// \param length indicates how many elements of `posneg` must be included
    ///               in the search.
    ///
    /// \returns the index of the first number whose exponent differs from that
    ///          of `sorted[0]`, if any, or `SIZE_MAX` if there is no number
    ///          matching this criterion.
    UDIPE_NON_NULL_ARGS
    static size_t find_second_exponent(const double sorted[], size_t length) {
        // Handle special cases
        if (length == 0) return SIZE_MAX;

        // Check if there is at least one element with another exponent
        const double first = sorted[0];
        assert(!isnan(first));
        const int first_exp = ilogb(first);
        const size_t last_idx = length - 1;
        const double last = sorted[last_idx];
        assert(!isnan(last));
        const int last_exp = ilogb(last);
        if (last_exp == first_exp) return SIZE_MAX;

        // Narrow down the first exponent boundary through binary search
        size_t last_same_exp_idx = 0;
        size_t first_other_exp_idx = last_idx;
        while (first_other_exp_idx - last_same_exp_idx > 1) {
            assert(first_other_exp_idx > last_same_exp_idx);
            const size_t mid_idx =
                last_same_exp_idx
                    + (first_other_exp_idx - last_same_exp_idx) / 2;
            const double mid_value = sorted[mid_idx];
            const int mid_exp = ilogb(mid_value);
            if (mid_exp > first_exp) {
                first_other_exp_idx = mid_idx;
            } else {
                assert(mid_exp == first_exp);
                last_same_exp_idx = mid_idx;
            }
        }
        assert(first_other_exp_idx == last_same_exp_idx + 1);
        return first_other_exp_idx;
    }

    /// Result of find_first_exponent_layout()
    ///
    /// In the context of an array of floating-point numbers that are sorted by
    /// exponent, then sign, then mantissa, this tells you the layout of the
    /// first exponent bin. See the docs of first_exponent_bin_layout() for more
    /// information.
    typedef struct first_exponent_layout_s {
        /// Index of the first positive number of same exponent (if any)
        ///
        /// If all numbers in the first exponent bucket are negative, then this
        /// will take the `USIZE_MAX` sentinel value.
        size_t first_positive_idx;

        /// Total amount of numbers with the same exponent, i.e. index of the
        /// first number with a different exponent.
        ///
        /// By design, find_first_exponent_layout() can only be called on an
        /// array with two exponent bins, so this length should always be defined.
        size_t length;
    } first_exponent_layout_t;
    //
    /// Determine the layout of the first exponent bin in a sorted array
    ///
    /// In an array that is sorted by exponent, then sign, then mantissa, this
    /// utility function is called in circumstances where no subset of the
    /// layout of the first exponent bin can be trivially figured out. The
    /// latter implies that the first element of `sorted` is negative and at
    /// least two exponent bins are present.
    ///
    /// The job of this function is then to figure out the layout of the first
    /// exponent bin, namely the position of the first positive value (if any)
    /// and of the end of the bin (which is the index of the first value with a
    /// different exponent).
    ///
    /// \param sorted should point to an array of at least `length` values that
    ///               are sorted by exponent, excluding NaNs, and where it has
    ///               previously been checked that the first value is negative
    ///               (otherwise the more specialized find_second_exponent()
    ///               search function should be used) and the last value has a
    ///               different exponent (otherwise the more specialized
    ///               same-exponent functions should be used).
    /// \param length is the number of elements of `sorted` that will be
    ///               included in the search
    ///
    /// \returns the position of the first positive value with identical
    ///          exponent (if any) and the first value with a different exponent.
    UDIPE_NON_NULL_ARGS
    static first_exponent_layout_t find_first_exponent_layout(
        const double sorted[],
        size_t length
    ) {
        // Check preconditions
        assert(length >= 2);
        const double first_value = sorted[0];
        assert(!isnan(first_value));
        const int first_exp = ilogb(first_value);
        const int first_sign = signbit(first_value);
        assert(first_sign);
        const size_t last_idx = length - 1;
        const double last_value = sorted[last_idx];
        assert(!isnan(last_value) && ilogb(last_value) != first_exp);

        // Look for the exponent boundary while simultaneously collecting data
        // for the subsequent search for the sign boundary.
        size_t same_exp_same_sign_idx = 0;
        size_t same_exp_other_sign_idx = SIZE_MAX;
        size_t same_exp_any_sign_idx = 0;
        size_t other_exp_idx = last_idx;
        while (other_exp_idx - same_exp_any_sign_idx > 1) {
            const size_t mid_idx = same_exp_any_sign_idx
                                 + (other_exp_idx - same_exp_any_sign_idx) / 2;
            const double mid_value = sorted[mid_idx];
            assert(!isnan(mid_value));

            const int mid_exp = ilogb(mid_value);
            if (mid_exp != first_exp) {
                other_exp_idx = mid_idx;
                continue;
            }
            assert(mid_exp == first_exp);
            same_exp_any_sign_idx = mid_idx;

            if (signbit(mid_value) == first_sign) {
                same_exp_same_sign_idx = mid_idx;
            } else {
                same_exp_other_sign_idx = mid_idx;
            }
        }
        assert(other_exp_idx == same_exp_any_sign_idx + 1);
        const size_t first_exp_length = other_exp_idx;

        // If the last value of identical exponent has the same sign as the
        // first value of the array, there is no sign boundary and we are done.
        if (same_exp_same_sign_idx == same_exp_any_sign_idx) {
            return (first_exponent_layout_t){
                .first_positive_idx = SIZE_MAX,
                .length = first_exp_length
            };
        }

        // Otherwise, we know that there is a sign boundary and just need a few
        // more iterations of classic binary search to narrow it down
        assert(same_exp_other_sign_idx <= same_exp_any_sign_idx
               && same_exp_other_sign_idx > same_exp_same_sign_idx);
        size_t same_sign_idx = same_exp_same_sign_idx;
        size_t other_sign_idx = same_exp_other_sign_idx;
        while (other_sign_idx - same_sign_idx > 1) {
            const size_t mid_idx = same_sign_idx
                                 + (other_sign_idx - same_sign_idx) / 2;
            const double mid_value = sorted[mid_idx];
            assert(!isnan(mid_value));
            if (signbit(mid_value) == first_sign) {
                same_sign_idx = mid_idx;
            } else {
                other_sign_idx = mid_idx;
            }
        }
        assert(other_sign_idx == same_sign_idx + 1
               && !signbit(sorted[other_sign_idx]));
        return (first_exponent_layout_t){
            .first_positive_idx = other_sign_idx,
            .length = first_exp_length
        };
    }

    UDIPE_NON_NULL_ARGS
    double sum_f64(double array[], size_t length) {
        // Handle special cases
        if (length == 0) return 0.0;
        if (length == 1) return array[0];

        // Sort the inputs by exponent, then sign, then mantissa
        sort_f64_exp_sign_mantissa(array, length);

        // As long as there are numbers to be summed...
        while (length >= 2) {
            // Check if all numbers have the same exponent
            const double first = array[0];
            const int first_exp = ilogb(first);
            const size_t last_idx = length - 1;
            const double last = array[last_idx];
            const int last_exp = ilogb(first);
            if (first_exp == last_exp) {
                // If they also have the same sign, we're in the easiest
                // scenario covered by the sum_same_exp_sign() function.
                if (signbit(first) == signbit(last)) {
                    return sum_same_exp_sign(array, length);
                }

                // Otherwise, we have a mixture of positive and negative
                // numbers, which per the data ordering invariant of the outer
                // loop should be partitioned with negative values packed at the
                // beginning and positive values packed at the end
                assert(signbit(first) && !signbit(last));

                // Locate the partition point, which should exist
                const size_t first_positive_idx = find_first_positive(array,
                                                                      length);
                assert(first_positive_idx < length);

                // Perform the canceling sums, reorder the results as expected,
                // then repeat the toplevel logic over the remaining numbers
                // (results of canceling sums are not guaranteed to all have the
                // same exponent so we cannot just stay in this branch).
                const size_t num_canceling_sums =
                    cancel_and_shift(first_positive_idx, array, length);
                array += num_canceling_sums;
                length -= num_canceling_sums;
                continue;
            }

            // If control reached this point, we have numbers of at least two
            // different exponents.  In this case we need to locate the next
            // positive/negative boundary (if any) and the next exponent
            // boundary, and from this decide what we do next.
            assert(first_exp < last_exp);

            // First of all, handle a common edge case where we only have
            // positive numbers with exponent first_exp. Because exponent
            // buckets are partitioned by negative/positive, this can be done by
            // checking if the first value is positive.
            if (!signbit(first)) {
                // In this case, we just find the next exponent boundary...
                const size_t next_exp_idx = find_second_exponent(array, length);
                assert(next_exp_idx < length);

                // ...perform one pairwise sum pass in the first exponent bucket
                const size_t num_same_exp = next_exp_idx;
                const size_t num_integrated =
                    sum_same_exp_sign_pass(array, num_same_exp);
                array += num_integrated;
                length -= num_integrated;

                // ...and bring the array back to the expected ordering...
                //
                // If this global sort becomes a performance bottleneck, it's
                // probably possible to analytically prove that sorting a
                // smaller subset of the array is sufficient, which may improve
                // performance if binary search followed by a small sort is
                // faster than sorting everything (which also remains to be
                // empirically proven through benchmarking).
                //
                // I suspect sorting the region that goes until exponent
                // first_exp + 3 is sufficient (+1 because we're summing pairs
                // of numbers, +1 because there may be a trailing element that
                // gets summed too, and +1 to account for rounding shenanigans),
                // but I need to do the formal proof work to cross-check it.
                sort_f64_exp_sign_mantissa(array, length);
                continue;
            }

            // In the most general case, we must analyze the full layout of the
            // first exponent bin...
            const first_exponent_layout_t first_exp_layout =
                find_first_exponent_layout(array, length);

            // ...use the same same-sign logic as above if there are no
            // positive numbers...
            if (first_exp_layout.first_positive_idx == SIZE_MAX) {
                const size_t num_integrated =
                    sum_same_exp_sign_pass(array,
                                           first_exp_layout.length);
                array += num_integrated;
                length -= num_integrated;
                sort_f64_exp_sign_mantissa(array, length);
                continue;
            }

            // ...and otherwise handle the cancelations as done above
            const size_t num_canceling_sums =
                cancel_and_shift(first_exp_layout.first_positive_idx,
                                 array,
                                 first_exp_layout.length);
            array += num_canceling_sums;
            length -= num_canceling_sums;
        }
        ensure_eq(length, (size_t)1);
        return array[0];
    }


    #ifdef UDIPE_BUILD_TESTS

        /// Number of random trials per tests
        ///
        static const size_t NUM_TRIALS = 1024;

        /// Size of the random dataset used in each trial
        ///
        #define TRIAL_LENGTH ((size_t)1000)

        /// Generate a bunch of random bits
        ///
        /// \param words is the array where the output bits will be stored
        /// \param length is the number of words in the `words` array
        static void generate_entropy(uint64_t words[], size_t length) {
            const size_t bits_per_rand = population_count(RAND_MAX);
            const size_t bits_per_word = 64;
            memset(words, 0, length * sizeof(uint64_t));
            size_t bits_so_far = 0;
            while (bits_so_far < length * bits_per_word) {
                const size_t word = bits_so_far / bits_per_word;
                const size_t offset = bits_so_far % bits_per_word;
                tracef("- At global bit #%zu (word #%zu, local bit #%zu)",
                       bits_so_far, word, offset);
                const size_t local_bits = bits_per_word - offset;
                const uint64_t entropy = rand();
                words[word] |= entropy << offset;
                if (local_bits < bits_per_rand && word + 1 < length) {
                    const size_t remaining_bits = bits_per_rand - local_bits;
                    words[word + 1] = entropy >> local_bits;
                }
                bits_so_far += bits_per_rand;
            }
        }

        // Double precision number information
        #define NUM_FRACTION_BITS ((size_t)52)
        #define NUM_EXP_BITS ((size_t)11)
        static const uint64_t FRACTION_MASK =
            ((uint64_t)1 << NUM_FRACTION_BITS) - 1;
        static const uint64_t EXP_MASK =
            (((uint64_t)1 << NUM_EXP_BITS) - 1) << NUM_FRACTION_BITS;
        //
        #define EXP_BIAS ((1 << (NUM_EXP_BITS - 1)) - 1)
        #define MIN_FINITE_EXP (-EXP_BIAS)
        static int MIN_NORMAL_EXP = MIN_FINITE_EXP + 1;
        static int MAX_FINITE_EXP = EXP_BIAS;
        static int INFINITE_EXP = EXP_BIAS + 1;

        /// Bitcast a 64-bit integer into a double-precision float
        ///
        static inline double bitcast_u64_to_f64(uint64_t bits) {
            union {
                uint64_t u64;
                double f64;
            } bitcast = { .u64 = bits };
            return bitcast.f64;
        }

        /// Generate a finite double-precision number
        ///
        /// Finite numbers include normal numbers and subnormal numbers,
        /// including positive and negative zero.
        static inline void generate_finite(double target[TRIAL_LENGTH]) {
            // First we generate a bunch of entropy
            debugf("Generating entropy for all %zu numbers...", TRIAL_LENGTH);
            uint64_t entropy[TRIAL_LENGTH];
            generate_entropy(entropy, TRIAL_LENGTH);

            // Then, for each generated word...
            for (size_t i = 0; i < TRIAL_LENGTH; ++i) {
                uint64_t entropy_word = entropy[i];
                do {
                    tracef("- Generating number #%zu from entropy word %#zx...",
                           i, entropy_word);

                    // Finite numbers are those for which the exponent does not
                    // take an all-ones bit pattern. If we encounter one of
                    // those, we just reroll, as the 2^-11 rejection sampling
                    // probability is low enough that this should never be a
                    // performance problem.
                    const uint64_t exp_bits = entropy_word & EXP_MASK;
                    tracef("  * Entropy word has exponent bits %#zx (exponent mask being %#zx).",
                           exp_bits, EXP_MASK);
                    if (exp_bits == EXP_MASK) {
                        trace("  * That's not finite, regenerate entropy word...");
                        generate_entropy(&entropy_word, 1);
                        continue;
                    }

                    // If our number is the valid representation of a double, we
                    // cast it to double and we're done.
                    target[i] = bitcast_u64_to_f64(entropy_word);
                    tracef("  * End result is %g.", target[i]);
                    assert(isfinite(target[i]));
                    break;
                } while(true);
            }
        }

        /// Strongly typed version of compare_f64_exp_sign_mantissa()
        static int compare_f64_typed(double d1, double d2) {
            return compare_f64_exp_sign_mantissa(&d1, &d2);
        }

        /// Test comparison of floating-point numbers
        ///
        static void test_comparison() {
            double finite[TRIAL_LENGTH];
            for (size_t t = 0; t < NUM_TRIALS; ++t) {
                generate_finite(finite);
                for (size_t i = 0; i < TRIAL_LENGTH / 2; ++i) {
                    const double d1 = finite[2*i];
                    const double d2 = finite[2*i + 1];
                    tracef("- Comparing %g with %g...", d1, d2);
                    ensure(!isunordered(d1, d2));

                    const int result = compare_f64_typed(d1, d2);

                    if (ilogb(d1) < ilogb(d2)) {
                        ensure_eq(result, -1);
                        continue;
                    }
                    if (ilogb(d1) > ilogb(d2)) {
                        ensure_eq(result, 1);
                        continue;
                    }
                    tracef("  * Exponents %d and %d are the same...",
                           ilogb(d1), ilogb(d2));

                    if (signbit(d1) > signbit(d2)) {
                        ensure_eq(result, -1);
                        continue;
                    }
                    if (signbit(d1) < signbit(d2)) {
                        ensure_eq(result, 1);
                        continue;
                    }
                    tracef("  * Sign bits %d and %d are the same...",
                           signbit(d1), signbit(d2));

                    if (fabs(d1) < fabs(d2)) {
                        ensure_eq(result, -1);
                        continue;
                    }
                    if (fabs(d1) > fabs(d2)) {
                        ensure_eq(result, 1);
                        continue;
                    }
                    tracef("  * Fractions %g and %g are the same...",
                           fabs(d1), fabs(d2));
                    ensure_eq(d1, d2);
                    ensure_eq(result, 0);
                }
            }
        }

        /// Test sorting of floating-point number
        ///
        static void test_sort() {
            double finite[TRIAL_LENGTH];
            for (size_t t = 0; t < NUM_TRIALS; ++t) {
                generate_finite(finite);
                sort_f64_exp_sign_mantissa(finite, TRIAL_LENGTH);
                for (size_t i = 0; i < TRIAL_LENGTH - 1; ++i) {
                    ensure_le(compare_f64_typed(finite[i], finite[i+1]), 0);
                }
            }
        }

        /// Generate normal numbers with identical exponent and sign
        ///
        /// We do not support generating subnormal numbers here because 1/we
        /// would end up on this edge case too rarely, it would benefit from a
        /// more dedicated generation strategy and 2/it makes the generation
        /// algorithm more complicated since we can't just use exponent bits to
        /// enforce the common magnitude.
        ///
        /// What we do support, however, is capping the max exponent as sums of
        /// numbers with large exponents are going to overflow and we don't need
        /// to test this case as we already warn the user to normalize their
        /// data...
        ///
        /// \param target is the buffer that will receive the output number
        /// \param max_normal_exp specifies the maximal binary exponent that is
        ///                       acceptable in the output data. This is used to
        ///                       simulate input data being more or less well
        ///                       normalized.
        static inline void generate_same_exp_sign(double target[TRIAL_LENGTH],
                                                  int max_normal_exp) {
            // Generate enough entropy
            const size_t num_exp_sign_bits = NUM_EXP_BITS + 1;
            const size_t num_entropy_bits =
                num_exp_sign_bits + TRIAL_LENGTH * NUM_FRACTION_BITS;
            const size_t bits_per_word = 64;
            const size_t num_entropy_words =
                num_entropy_bits / bits_per_word +
                    (num_entropy_bits % bits_per_word != 0);
            assert(num_entropy_words < TRIAL_LENGTH);
            uint64_t entropy[TRIAL_LENGTH];
            generate_entropy(entropy, num_entropy_words);

            // Translate exponent constraint into an IEEE-754 like
            // representation, except we put the exponent in the low order bits
            ensure_ge(max_normal_exp, MIN_FINITE_EXP);
            ensure_le(max_normal_exp, MAX_FINITE_EXP);
            const uint64_t min_biased_exp = MIN_NORMAL_EXP + EXP_BIAS;
            const uint64_t max_biased_exp = max_normal_exp + EXP_BIAS;
            const uint64_t shifted_exp_mask = (1 << NUM_EXP_BITS) - 1;
            assert(max_biased_exp <= shifted_exp_mask);

            // Reroll first entropy word until exponent is in desired range,
            // then extract exponent + mask from the low-order bits
            do {
                const uint64_t biased_exp = (entropy[0] & shifted_exp_mask);
                if (biased_exp >= min_biased_exp
                    && biased_exp <= max_biased_exp) break;
                generate_entropy(&entropy[0], 1);
            } while(true);
            const size_t shifted_exp_sign_mask = (1 << num_exp_sign_bits) - 1;
            const uint64_t exp_sign_bits =
                (entropy[0] & shifted_exp_sign_mask) << NUM_FRACTION_BITS;
            size_t current_bit = num_exp_sign_bits;

            // Iterate over remaining entropy bits to generate fractions
            for (size_t i = 0; i < TRIAL_LENGTH; ++i) {
                assert(current_bit < num_entropy_bits);
                const size_t word = current_bit / bits_per_word;
                assert(word < num_entropy_words);
                const size_t offset = current_bit % bits_per_word;
                const size_t local_bits = bits_per_word - offset;

                uint64_t buffer = entropy[word] >> offset;
                if (local_bits < NUM_FRACTION_BITS) {
                    assert(word + 1 < num_entropy_words);
                    buffer |= entropy[word + 1] << local_bits;
                }
                buffer = exp_sign_bits | (buffer & FRACTION_MASK);

                target[i] = bitcast_u64_to_f64(buffer);
                current_bit += NUM_FRACTION_BITS;
            }
        }

        /// Test one pass of summing numbers of same exponent/sign
        ///
        void test_sum_same_exp_sign_pass() {
            double homogeneous[TRIAL_LENGTH], backup[TRIAL_LENGTH];
            for (size_t t = 0; t < NUM_TRIALS; ++t) {
                generate_same_exp_sign(homogeneous, MAX_FINITE_EXP - 2);
                size_t used_length = 2 + rand() % (TRIAL_LENGTH - 1);
                for (size_t i = 0; i < used_length; ++i) backup[i] = homogeneous[i];

                const size_t num_summed = sum_same_exp_sign_pass(homogeneous,
                                                                 used_length);

                const size_t half_length = used_length / 2;
                const bool has_remainder = (used_length % 2 != 0);
                const size_t expected_num_summed = half_length + has_remainder;
                ensure_eq(num_summed, expected_num_summed);
                const double* const summed = homogeneous + num_summed;
                const size_t num_remaining = used_length - num_summed;

                // Check that the order of magnitude is right. We can't check
                // more without assuming stuff about the implementation
                const int typical_exp = ilogb(backup[0]) + 1;
                bool remainder_expected = has_remainder;
                for (size_t i = 0; i < num_remaining; ++i) {
                    ensure(isfinite(summed[i]));
                    const int summed_exp = ilogb(summed[i]);
                    if (summed_exp != typical_exp) {
                        ensure(has_remainder);
                        ensure(remainder_expected);
                        ensure_eq(summed_exp, typical_exp + 1);
                        remainder_expected = false;
                    }
                }
            }
        }

        // TODO: Test full sum_same_exp_sign() by computing the
        //       infinite-precision sum using a bigint fixed-point
        //       representation (initial exponent provides the base exponent,
        //       log2 of the number of sums rounded up provides the required
        //       number of fixed-point bits on top of the initial mantissa
        //       bits), then casting/truncating back to f64 at the end and
        //       checking by how many ULPs we deviate from this exact result.

        void numeric_unit_tests() {
            info("Testing numerical operations...");
            configure_rand();

            debug("Exercizing comparison...");
            with_log_level(UDIPE_TRACE, {
                test_comparison();
            });

            debug("Exercizing sort...");
            with_log_level(UDIPE_TRACE, {
                test_sort();
            });

            debug("Exercizing same-sign sum pass...");
            with_log_level(UDIPE_TRACE, {
                test_sum_same_exp_sign_pass();
            });

            // TODO: Test all other operations
        }

    #endif  // UDIPE_BUILD_TESTS

#endif  // UDIPE_BUILD_BENCHMARKS