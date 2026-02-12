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
        tracef("Larger subtrahend treated as minuend with highest_idx %zu "
               "and low_idx %zu...",
               minuend_highest_idx, minuend_low_idx);

        // Subtract original accumulator magnitude from the minuend magnitude to
        // produce the new accumulator magnitude
        bool carry = false;
        size_t highest_word_idx = 0;
        for (size_t word_idx = 0; word_idx <= minuend_highest_idx; ++word_idx) {
            const uint64_t minuend_word =
                (word_idx < minuend_low_idx)
                    ? (uint64_t)0
                    : minuend_words[word_idx - minuend_low_idx];
            const uint64_t acc_word = acc->words[word_idx];
            uint64_t difference;
            carry = subtract_with_carry_u64(carry,
                                            minuend_word,
                                            acc_word,
                                            &difference);
            tracef("- At word #%zu: minuend %#018zx - acc %#018zx = %#018zx "
                   "with carry %d",
                   word_idx, minuend_word, acc_word, difference, carry);
            if (difference != 0) highest_word_idx = word_idx;
            acc->words[word_idx] = difference;
        }

        // There shouldn't be any carry here because the accumulator should be
        // smaller than the minuend.
        assert(!carry);

        // Finish updating accumulator state
        trace("Updating accumulator highest_idx and sign...");
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
        tracef("Addend has significand %#016zx, zero-based exponent %zu, sign %d.",
               significand, zero_based_exponent, negative);
        if (significand == 0) return;

        // Translate the floating-point addend into a floating word addend
        const unsigned_addend_t magnitude =
            compute_unsigned_addend(significand, zero_based_exponent);
        tracef("Addend has magnitude [%#018zx, %#018zx] with word shift %zu.",
               magnitude.words[1], magnitude.words[0], magnitude.low_word_idx);

        // Handle the same-sign addition easy/common case
        if (negative == acc->negative) {
            // As the addend has the same sign, accumulator magnitude can only
            // increase and absence of underflow is guaranteed
            trace("Addend has same sign as accumulator: will sum magnitudes.");
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
            trace("Addend has opposite sign and larger magnitude: "
                  "will subtract accumulator from addend.");
            accumulator_subtract_with_underflow(acc,
                                                magnitude);
        } else {
            // The accumulator and addend have an opposite sign but the addend
            // has been checked to have a smaller magnitude, so we can subtract
            // the addend from the accumulator without underflow.
            trace("Addend has opposite sign and lower magnitude: "
                  "will subtract addend from accumulator.");
            accumulate_without_underflow(acc,
                                         magnitude,
                                         sub_inplace_return_carry,
                                         update_highest_idx_after_sub);
        }
    }

    UDIPE_NON_NULL_ARGS
    double accumulator_to_f64(const accumulator_t* acc) {
        // Convert the accumulator into a double precision number
        //
        // This is done by iteratively summing word contributions from the
        // lowest-magnitude word to the highest-magnitude word, which should
        // yield the same rounding as one IEEE-754 sum.
        trace("Turning the accumulator into the nearest binary64 number...");
        double result = 0.0;
        int exponent = -(int)(FRACTION_BITS_F64 + SUBNORMAL_EXPONENT_BIAS_F64);
        const double sign = (acc->negative) ? -1.0 : 1.0;
        for (size_t word_idx = 0; word_idx <= acc->highest_word_idx; ++word_idx) {
            const uint64_t word = acc->words[word_idx];
            const double contribution = sign * scalbn(word, exponent);
            result += contribution;
            tracef("- Integrate acc->words[%zu] = %#018zx with exponent %d "
                   "=> Contribution %g (%a), total so far %g (%a)",
                   word_idx, word, exponent,
                   contribution, contribution, result, result);
            exponent += BITS_PER_ACC_WORD;
        }
        return result;
    }


    #ifdef UDIPE_BUILD_TESTS

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
            accumulator_add_f64(&acc, 0.0);
            ensure_eq(accumulator_popcount(&acc), (size_t)0);
            ensure_eq(accumulator_to_f64(&acc), 0.0);
            //
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

        /// Size of the datasets that we are working with
        ///
        /// Chosen to ensure coverage of most exponents
        #define TEST_SET_SIZE  ((size_t)8192)

        /// Generate a bunch of floats with a bias towards extreme numbers which
        /// are more likely to exhibit issues because they have special code
        /// paths or lie close to a logical boundary.
        static void generate_test_set(double output[TEST_SET_SIZE]) {
            uint64_t entropy[2 * TEST_SET_SIZE];
            const size_t entropy_len = sizeof(entropy)/sizeof(uint64_t);
            generate_entropy(entropy, entropy_len);
            size_t consumed_bits = 0;

            uint64_t repr_bits[TEST_SET_SIZE];
            entropy_to_bits(64,
                            repr_bits,
                            TEST_SET_SIZE,
                            &consumed_bits,
                            entropy,
                            entropy_len);

            uint64_t special_number_bias[TEST_SET_SIZE];
            entropy_to_bits(4,
                            special_number_bias,
                            TEST_SET_SIZE,
                            &consumed_bits,
                            entropy,
                            entropy_len);

            for (size_t i = 0; i < TEST_SET_SIZE; ++i) {
                uint64_t bits = repr_bits[i];

                // Bias the generator towards "special" numbers
                switch (special_number_bias[i]) {
                case 0:
                    // Positive and negative zero
                    bits &= ~(EXPONENT_MASK_F64 | FRACTION_MASK_F64);
                    break;
                case 1:
                case 2:
                    // Numbers with minimal finite exponent, mostly subnormals
                    bits &= ~EXPONENT_MASK_F64;
                    break;
                case 13:
                case 14:
                    // Numbers with maximal exponent
                    bits |= EXPONENT_MASK_F64;
                    break;
                case 15:
                    // Maximal number
                    bits |= (EXPONENT_MASK_F64 | FRACTION_MASK_F64);
                    break;
                }

                // Only generate finite numbers
                if ((bits & EXPONENT_MASK_F64) == EXPONENT_MASK_F64) {
                    bits -= (uint64_t)1 << EXPONENT_SHIFT_F64;
                }

                // Turn binary representation into a binary64 number
                output[i] = bitcast_u64_to_f64(bits);
            }
        }

        /// Test round-trip conversion between binary64 and accumulators
        ///
        static void test_round_trip(const double test_set[]) {
            for (size_t i = 0; i < TEST_SET_SIZE; ++i) {
                const double value = test_set[i];
                accumulator_t acc = ACCUMULATOR_ZERO;

                debugf("- Processing value #%zu: %g (%a)",
                       i, value, value);
                accumulator_add_f64(&acc, value);

                const uint64_t repr = bitcast_f64_to_u64(value);
                uint64_t significand = repr & FRACTION_MASK_F64;
                if (isnormal(value)) significand |= (uint64_t)1 << FRACTION_BITS_F64;
                ensure_eq(accumulator_popcount(&acc),
                          population_count(significand));

                ensure_eq(accumulator_to_f64(&acc), value);
            }
        }

        /// Test pairwise sums of f64 via accumulators
        ///
        /// This should produce the same result as the native f64 sum except for
        /// the last bit which may be rounded differently.
        static void test_pair_sum(const double test_set[]) {
            const size_t half_test_set = TEST_SET_SIZE / 2;
            for (size_t i = 0; i < half_test_set; ++i) {
                const double x = test_set[i];
                const double y = test_set[i + half_test_set];
                accumulator_t acc = ACCUMULATOR_ZERO;

                tracef("- Processing sum #%zu: %g (%a) + %g (%a)...",
                       i, x, x, y, y);
                accumulator_add_f64(&acc, x);
                accumulator_add_f64(&acc, y);

                const double expected = x + y;
                const double actual = accumulator_to_f64(&acc);
                if (actual == expected) {
                    tracef("  * Sum yielded expected result %g (%a) down to the last significant digit.",
                           expected, expected);
                } else {
                    tracef("  * Sum was rounded differently (expected %a, got %a),"
                           " which is considered acceptable.",
                           expected, actual);
                    ensure_eq(nextafter(actual, expected), expected);
                }
            }
        }

        /// Test sums of powers of two via accumulators
        ///
        /// The result is compared to what one would expect using a simplified,
        /// lower-performance implementation.
        static void test_sum_pow2(const double test_set[]) {
            accumulator_t acc = ACCUMULATOR_ZERO;
            bool expected[NUM_ACCUMULATOR_WORDS * BITS_PER_ACC_WORD] = { 0 };
            bool expected_sign = false;
            const size_t num_bits = sizeof(expected)/sizeof(bool);
            for (size_t i = 0; i < TEST_SET_SIZE; ++i) {
                // Compute an addend that is a power or two or zero
                const double value = test_set[i];
                int exp;
                double addend;
                if (value != 0.0) {
                    const double sign = copysign(1.0, value);
                    exp = ilogb(value);
                    addend = scalbn(sign, exp);
                } else {
                    addend = copysign(0.0, value);
                }
                tracef("- Adding pow2 #%zu: %a", i, addend);

                // Predict the effect of adding this addend using a highly
                // simplified/specialized implementation of the accumulator
                if (addend != 0) {
                    int zero_based_exp = exp + SUBNORMAL_EXPONENT_BIAS_F64 + FRACTION_BITS_F64;
                    ensure_ge(zero_based_exp, 0);
                    if (signbit(addend) == expected_sign) {
                        // Increase accumulator magnitude by addend, propagating
                        // carries as needed.
                        size_t addend_exp;
                        for (addend_exp = zero_based_exp; addend_exp < num_bits; ++addend_exp) {
                            if (expected[addend_exp]) {
                                expected[addend_exp] = false;
                                continue;
                            } else {
                                expected[addend_exp] = true;
                                break;
                            }
                        }
                        if (addend_exp == num_bits) {
                            warn("Accumulator overflown, this should be very unlikely with a good RNG!");
                            acc = ACCUMULATOR_ZERO;
                            continue;
                        }
                    } else {
                        // Determine how big the accumulator is
                        size_t expected_high_bit = num_bits - 1;
                        for (; expected_high_bit > 0; --expected_high_bit) {
                            if (expected[expected_high_bit]) break;
                        }

                        // Deduce who should be subtracted from whom
                        if (expected_high_bit >= (size_t)zero_based_exp) {
                            // Subtract addend from accumulator, propagating
                            // carries as needed.
                            size_t addend_exp;
                            for (addend_exp = zero_based_exp; addend_exp < num_bits; ++addend_exp) {
                                if (expected[addend_exp]) {
                                    expected[addend_exp] = false;
                                    break;
                                } else {
                                    expected[addend_exp] = true;
                                    continue;
                                }
                            }
                            ensure_lt(addend_exp, num_bits);
                        } else {
                            // Subtract accumulator from addend
                            bool carry = false;
                            for (size_t bit = 0; bit < (size_t)zero_based_exp; ++bit) {
                                const bool subtrahend = expected[bit];
                                expected[bit] = subtrahend ^ carry;
                                carry = subtrahend || carry;
                            }
                            ensure(!expected[zero_based_exp]);
                            expected[zero_based_exp] = !carry;
                            for (size_t bit = zero_based_exp + 1; bit < num_bits; ++bit) {
                                ensure(!expected[bit]);
                            }
                            expected_sign = signbit(addend);
                        }
                    }
                }

                // Add this addend into the accumulator
                accumulator_add_f64(&acc, addend);

                // Check accumulator inner words vs expected bits
                size_t highest_word_idx = 0;
                for (size_t bit = 0; bit < num_bits; ++bit) {
                    const size_t word = bit / BITS_PER_ACC_WORD;
                    const size_t offset = bit % BITS_PER_ACC_WORD;
                    const bool acc_bit = (acc.words[word] >> offset) & 1;
                    ensure_eq(acc_bit, expected[bit]);
                    if (acc_bit) highest_word_idx = word;
                }
                ensure_eq(acc.highest_word_idx, highest_word_idx);
                ensure_eq(acc.negative, expected_sign);
            }
        }

        void numeric_unit_tests() {
            info("Testing numerical operations...");
            configure_rand();

            debug("Warming up with a few basic numbers...");
            with_log_level(UDIPE_TRACE, {
                test_basic_numbers();
            });

            double test_set[TEST_SET_SIZE];
            generate_test_set(test_set);

            debug("Testing round trip conversions...");
            with_log_level(UDIPE_TRACE, {
                test_round_trip(test_set);
            });

            debug("Testing pairwise sums...");
            with_log_level(UDIPE_TRACE, {
                test_pair_sum(test_set);
            });

            debug("Testing sum of powers of 2...");
            with_log_level(UDIPE_TRACE, {
                test_sum_pow2(test_set);
            });
        }

    #endif  // UDIPE_BUILD_TESTS

#endif  // UDIPE_BUILD_BENCHMARKS