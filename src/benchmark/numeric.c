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


    UDIPE_NON_NULL_ARGS
    double sum_f64(double values[], size_t length) {
        accumulator_t acc = ACCUMULATOR_ZERO;
        for (size_t i = 0; i < length; ++i) {
            accumulator_add_f64(&acc, values[i]);
        }
        return accumulator_to_f64(&acc);
    }


    // === Implementation details ===

    UDIPE_NON_NULL_ARGS
    void accumulator_subtract_with_underflow(accumulator_t* acc,
                                             unsigned_addend_t subtrahend) {
        // Check preconditions
        assert((subtrahend.words[0] | subtrahend.words[1]) != 0);
        assert(accumulator_lt_nonzero_subtrahend(acc, subtrahend));

        // As the accumulator magnitude will get subtracted from the subtrahend
        // magnitude in the underflowing case that we are dealing with, the
        // subtrahend's magnitude actually assumes the role of a minuend
        const uint64_t* minuend_words = subtrahend.words;
        const size_t minuend_low_idx = subtrahend.low_word_idx;
        assert(minuend_low_idx < NUM_ACCUMULATOR_WORDS);
        const size_t minuend_highest_idx = minuend_low_idx + (minuend_words[1] != 0);
        assert(minuend_highest_idx >= acc->highest_word_idx);

        // Subtract original accumulator magnitude from the minuend magnitude to
        // produce the new accumulator magnitude
        bool carry = false;
        size_t highest_word_idx = 0;
        for (size_t word_idx = 0; word_idx <= minuend_highest_idx; ++word_idx) {
            const uint64_t minuend_word =
                (word_idx < minuend_low_idx)
                    ? (uint64_t)0
                    : minuend_words[word_idx - minuend_low_idx];
            uint64_t difference;
            carry = subtract_with_carry_u64(carry,
                                            minuend_word,
                                            acc->words[word_idx],
                                            &difference);
            if (difference != 0) highest_word_idx = word_idx;
            acc->words[word_idx] = difference;
        }

        // There shouldn't be any carry here because the accumulator should be
        // smaller than the minuend.
        assert(!carry);

        // Finish updating accumulator state
        acc->highest_word_idx = highest_word_idx;
        acc->negative = !(acc->negative);
    }

    UDIPE_NON_NULL_ARGS
    void accumulate_decoded_f64(accumulator_t* acc,
                                uint64_t significand,
                                size_t zero_based_exponent,
                                bool negative) {
        // Check preconditions in debug builds
        assert((significand & SIGNIFICAND_MASK_F64) == significand);
        assert(zero_based_exponent < NUM_FINITE_EXPONENTS_F64);

        // Handle zero addend edge case
        if (significand == 0) return;

        // Translate the floating-point addend into a floating word addend
        const unsigned_addend_t magnitude =
            compute_unsigned_addend(significand, zero_based_exponent);

        // Handle the same-sign addition easy/common case
        if (negative == acc->negative) {
            // As the addend has the same sign, accumulator magnitude can only
            // increase and absence of underflow is guaranteed
            accumulate_without_underflow(acc,
                                         magnitude,
                                         add_inplace_return_carry,
                                         update_highest_idx_after_add);
        } else if (accumulator_lt_nonzero_subtrahend(acc, magnitude)) {
            // We are dealing with an opposite-sign addition, aka a subtraction,
            // and the addend term has larger magnitude than the accumulator. In
            // this case the result's magnitude is given by subtracting the
            // accumulator's magnitude from the addend's magnitude, and the end
            // result will have the sign of the addend.
            accumulator_subtract_with_underflow(acc,
                                                magnitude);
        } else {
            // The accumulator and addend have an opposite sign but the addend
            // has been checked to have a smaller magnitude, so we can subtract
            // the addend from the accumulator without underflow.
            accumulate_without_underflow(acc,
                                         magnitude,
                                         sub_inplace_return_carry,
                                         update_highest_idx_after_sub);
        }
    }

    UDIPE_NON_NULL_ARGS
    double accumulator_to_f64(const accumulator_t* acc) {
        // We start by turning the high-order word of the fixed point
        // representation into a double.
        //
        // This number has a number of significant digits which depends on its
        // number of leading zeros. We could count those, but that could get
        // expensive if the CPU doesn't have a dedicated instruction for it...
        const size_t high_order_idx = acc->highest_word_idx;
        assert(high_order_idx < NUM_ACCUMULATOR_WORDS);
        double result = acc->words[high_order_idx];

        // ...so instead we let floating-point conversions figure it out and
        // always add a contribution from the next-order word if there is one.
        //
        // The result would have 65 to 128 significant bits depending on the
        // number of leading zeros of the high-order word, which is always more
        // bits than the FRACTION_BITS that a floating point number can store,
        // so in round-to-nearest mode we are guaranteed to get the best
        // double-precision approximation of the result.
        if (high_order_idx > 0) {
            const size_t low_order_idx = high_order_idx - 1;
            result += scalbn(acc->words[low_order_idx],
                             -(int)BITS_PER_ACC_WORD);
        }
        assert(BITS_PER_ACC_WORD > SIGNIFICAND_BITS_F64);

        // At this point the result has the right mantissa, but the wrong
        // exponent. We address this by applying the appropriate exponent
        // offset. Here's how it is computed:
        //
        // - Let's consider a subnormal number with high_order_idx == 0.
        //     * The above computation generates <fraction bits> x 2^0, where it
        //       should generate 0.<fraction bits> x
        //       2^-SUBNORMAL_EXPONENT_BIAS_F64.
        //     * To get from <fraction bits> to 0.<fraction bits>, we shift the
        //       exponent by -FRACTION_BITS.
        //     * To get from 2^0 to 2^-SUBNORMAL_EXPONENT_BIAS_F64, we shift the
        //       exponent by -SUBNORMAL_EXPONENT_BIAS_F64.
        // - The above rule also works for normal numbers and numbers of other
        //   magnitudes if we add an exponent shift based on high_order_idx.
        result = scalbn(
            result,
            (int)(high_order_idx * BITS_PER_ACC_WORD)
                - (int)FRACTION_BITS_F64
                - (int)SUBNORMAL_EXPONENT_BIAS_F64
        );

        // ...and all that is left is to propagate the sign bit to the result
        const double sign = (acc->negative) ? -1.0 : 1.0;
        return sign * result;
    }


    #ifdef UDIPE_BUILD_TESTS

        // TODO: Decide what to do with previous test code

        /* /// Number of random trials per tests
        ///
        static const size_t NUM_TRIALS = 1024;

        /// Size of the random dataset used in each trial
        ///
        #define TRIAL_LENGTH ((size_t)1000)

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
        //       checking by how many ULPs we deviate from this exact result. */

        // TODO === wip new tests ===

        /// Test the number of set bits within an accumulator's inner words
        ///
        static size_t accumulator_popcount(const accumulator_t* acc) {
            ensure_ge(BITS_PER_WORD, (size_t)64);
            size_t popcount = 0;
            for (size_t word_idx = 0; word_idx < NUM_ACCUMULATOR_WORDS; ++word_idx) {
                popcount += population_count(acc->words[word_idx]);
            }
            return popcount;
        }

        /// Test basic numbers
        static void test_basic_numbers() {
            // Check that the zero accumulator is indeed zero
            ensure_eq(accumulator_to_f64(&ACCUMULATOR_ZERO), 0.0);

            // Check effect of adding basic powers of two in both directions
            //
            // Popcount test allow us to detect the presence of improperly set
            // low-order significant bits below the precision threshold of the
            // conversion back to f64.
            accumulator_t acc = ACCUMULATOR_ZERO;
            accumulator_add_f64(&acc, 1.0);
            ensure_eq(accumulator_popcount(&acc), (size_t)1);
            ensure_eq(accumulator_to_f64(&acc), 1.0);
            //
            accumulator_add_f64(&acc, -1.0);
            ensure_eq(accumulator_popcount(&acc), (size_t)0);
            ensure_eq(accumulator_to_f64(&acc), 0.0);
            //
            accumulator_add_f64(&acc, -0.5);
            ensure_eq(accumulator_popcount(&acc), (size_t)1);
            ensure_eq(accumulator_to_f64(&acc), -0.5);
            //
            accumulator_add_f64(&acc, 2.0);
            ensure_eq(accumulator_popcount(&acc), (size_t)2);
            ensure_eq(accumulator_to_f64(&acc), 1.5);
            //
            accumulator_add_f64(&acc, 0.5);
            ensure_eq(accumulator_popcount(&acc), (size_t)1);
            ensure_eq(accumulator_to_f64(&acc), 2.0);
            //
            accumulator_add_f64(&acc, -2.0);
            ensure_eq(accumulator_popcount(&acc), (size_t)0);
            ensure_eq(accumulator_to_f64(&acc), 0.0);
        }

        void numeric_unit_tests() {
            info("Testing numerical operations...");
            configure_rand();

            test_basic_numbers();

            // TODO: Use generate_entropy() and extract_entropy() to generate
            //       a bunch of floats with a bias towards high and low
            //       exponents (1/5 subnormals, 1/5 max-normal, 3/5 rest) and
            //       check round-trip conversion.

            // TODO: Missing tests
            //
            //       - Round-trip conversion from float to accumulator and back,
            //         with some minimal checks of the full accumulator repr
            //         like the above popcount test.
            //       - Sum of pairs of floats, compared to the IEEE-754 result
            //         (which for pairs should be as good as accumulator_t).
            //       - Sum of long enough (at least 2 * NUM_FINITE_EXPONENT)
            //         sequences of positive and negative powers of two,
            //         compared to result with a simplified implementation based
            //         on bitarrays. Check both accumulator state and final
            //         result.

            // TODO decide what to do with old test code


            /* debug("Exercizing comparison...");
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

            // TODO: Test all other operations */
        }

    #endif  // UDIPE_BUILD_TESTS

#endif  // UDIPE_BUILD_BENCHMARKS