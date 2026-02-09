#ifdef UDIPE_BUILD_BENCHMARKS

    #pragma once

    //! \file
    //! \brief Numerical analysis tools
    //!
    //! There are a few basic operations in floating-point math that require
    //! some care if you don't want to experience a massive precision
    //! degradation on larger datasets. This code module provides such
    //! operations.

    #include <udipe/pointer.h>

    #include "bits.h"

    #include "../error.h"
    #include "../log.h"

    #include <stddef.h>


    /// Compute the sum of numbers in `array` of length `len` in place
    ///
    /// The summation algorithm takes precautions to minimize accumulation and
    /// cancelation error, while trying to remain reasonably fast on large
    /// arrays. It assumes finite inputs and will not work as expected if the
    /// dataset contains infinities or NaNs.
    ///
    /// To avoid overflow, the elements of `arrays` should preferably be
    /// normalized such that the maximum value is around 1.0. But since `double`
    /// has a huge exponent range (up to 2^1023), we can tolerate "reasonably"
    /// unnormalized values when length is "small enough".
    ///
    /// \param array must point to an array of at least `length` numbers, which
    ///              may be modified during the summation process.
    /// \param length indicates how many elements of `array` must be summed.
    ///
    /// \returns the sum of the elements of `array`.
    UDIPE_NON_NULL_ARGS
    double sum_f64(double array[], size_t length);


    /// \name Representation of "double precision" binary64 numbers
    /// \{

    /// Number of fraction bits stored within a binary64 number
    ///
    /// This excludes the implicit leading significand bit of normal numbers.
    #define FRACTION_BITS_F64  ((size_t)52)

    /// Mask of fraction bits within the binary64 representation
    ///
    #define FRACTION_MASK_F64  (((uint64_t)1 << FRACTION_BITS_F64) - 1)

    /// Maximal number of significant bits from a binary64 number
    ///
    /// This includes the implicit leading significand bit of normal numbers.
    /// Normal numbers have exactly this many significand bits, but subnormal
    /// numbers will have fewer significand bits, the exact number of which
    /// depends on how many leading zeroes they have.
    #define SIGNIFICAND_BITS_F64  (FRACTION_BITS_F64 + 1)

    /// Mask of significand bits in a decoded binary64 significand
    ///
    /// This mask does not directly map into the binary representation of
    /// binary64 numbers, but it can be applied to the decoded significand of a
    /// binary64 number, where the leading significand bit of numbers has been
    /// added as appropriate.
    #define SIGNIFICAND_MASK_F64  (((uint64_t)1 << SIGNIFICAND_BITS_F64) - 1)

    /// Bitshift that must be applied to binary64 exponent bits in order to
    /// position them in the right place of the 64-bit representation
    #define EXPONENT_SHIFT_F64  FRACTION_BITS_F64

    /// Number of exponent bits stored within a binary64 number
    ///
    #define EXPONENT_BITS_F64  ((size_t)11)

    /// Mask of biased exponent bits within the binary64 representation
    ///
    /// By biased we mean that after shifting back the exponent into its normal
    /// position, a bias value must be subtracted from it to get the true
    /// signed exponent of the floating-point number.
    #define EXPONENT_MASK_F64  ((((uint64_t)1 << EXPONENT_BITS_F64) - 1) << EXPONENT_SHIFT_F64)

    /// Special biased exponent bits for subnormal numbers
    ///
    /// When the biased exponent bits take this value, the number must be
    /// treated as having a 0 leading significand bit, and a minimal exponent
    /// one place higher than the rules for normal numbers would dictate.
    #define RAW_SUBNORMAL_EXPONENT_F64  ((uint64_t)0)

    /// Special biased exponent bits for non-finite numbers (+/-inf and NaNs)
    ///
    /// This module only supports finite binary64 numbers and will therefore
    /// fail upon encountering numbers with this exponent value.
    #define RAW_NONFINITE_EXPONENT_F64  EXPONENT_MASK_F64

    /// Bias to be applied when converting the biased exponent of normal
    /// binary64 to numbers to its signed counterpart
    ///
    /// Note that subnormal numbers have, in their standard `0.<fraction>`
    /// notation, an effective exponent that is one place higher than the one
    /// which would be predicted by the rule for normal numbers.
    #define NORMAL_EXPONENT_BIAS_F64  (((uint64_t)1 << (EXPONENT_BITS_F64 - 1)) - 1)

    /// Effective exponent bias of subnormal binary64 numbers
    ///
    /// As the lowest-exponent normal number and subnormal numbers have the same
    /// exponent in their standard representation (`1.<fraction>` and
    /// `0.<fraction>` respectively), it can be said that subnormal numbers
    /// effectively work with a different exponent bias.
    #define SUBNORMAL_EXPONENT_BIAS_F64  (NORMAL_EXPONENT_BIAS_F64 - 1)

    /// Number of _logically_ distinct finite exponents of binary64 numbers
    ///
    /// Starting from the total number of possible exponents, we subtract 1 as
    /// the maximal exponent is only used for infinities or NaNs and we subtract
    /// 1 again to account for the fact that the smallest normal numbers and
    /// subnormal numbers have the same exponent.
    #define NUM_FINITE_EXPONENTS_F64  (((size_t)1 << EXPONENT_BITS_F64) - 2)

    /// Sign bit of a binary64 number
    ///
    /// Finite numbers are negative when this bit is set and positive otherwise.
    #define SIGN_BIT_F64  ((uint64_t)1 << (EXPONENT_SHIFT_F64 + EXPONENT_BITS_F64))

    /// Helper for bitcasting between binary64 numbers and their representation
    ///
    typedef union u64_f64_bitcast_u {
        double f64;
        uint64_t u64;
    } u64_f64_bitcast_t;

    /// Bitcast a binary64 number into its representation
    ///
    static inline
    uint64_t bitcast_f64_to_u64(double f) {
        u64_f64_bitcast_t bitcast = { .f64 = f };
        return bitcast.u64;
    }

    /// Bitcast a binary64 representation into the matching number
    ///
    static inline
    double bitcast_u64_to_f64(uint64_t u) {
        u64_f64_bitcast_t bitcast = { .u64 = u };
        return bitcast.f64;
    }

    /// \}


    /// \name Implementation details of sum_f64()
    /// \{

    /// Size of the fixed-point representation of a binary64 number's magnitude,
    /// in bits
    ///
    /// In \ref accumulator_t, we conceptually represent the first nonzero
    /// subnormal number as a bigint of value 1, and handle exponents above the
    /// logical minimum by shifting the significant bigind left by as many bits
    /// as the exponent dictates.
    ///
    /// This means that in order to be able to handle all significant bits of a
    /// normal binary64 number in all exponent-shifted configurations, we need
    /// the following amount of bits.
    #define MIN_ACCUMULATOR_BITS  (SIGNIFICAND_BITS_F64 + NUM_FINITE_EXPONENTS_F64 - 1)

    /// Size of an accumulator word in bits
    ///
    /// We use 64-bit accumulators because...
    ///
    /// - On one side it is the largest integer data type with widespread
    ///   hardware support, so we don't want to go wider.
    /// - On the other side it is the smallest integer type that can natively
    ///   hold the 53-bit significand of binary64 without dual-word emulation
    ///   tricks, so we don't want to go narrower.
    #define BITS_PER_ACC_WORD  (sizeof(uint64_t) * 8)

    /// Size of the fixed-point representation of a binary64 number's magnitude,
    /// in 64-bit words
    ///
    /// The observant reader will notice that the rounding of the division gives
    /// us 14 more bits than we need, which means we can internally handle
    /// numbers that are up to 2^14 larger than the maximal finite binary64
    /// numbers "for free".
    ///
    /// This effectively has the effect of making the accumulator more tolerant
    /// of unnormalized input data, which is a nice property to have even though
    /// proper normalization should obviously be the rule in floating-point
    /// summation.
    #define NUM_ACCUMULATOR_WORDS  (DIV_CEIL(MIN_ACCUMULATOR_BITS, BITS_PER_ACC_WORD))

    /// Fixed-point accumulator for binary64 data
    ///
    /// This struct implements a sign-magnitude bigint large enough to hold the
    /// fixed-point representation of any finite binary64 number. It is used to
    /// compute the sum of binary64 numbers with perfect accuracy down to the
    /// last binary digit.
    ///
    /// While accuracy is guaranteed, absence of overflow is not guaranteed,
    /// however, and relies on the user either applying reasonable normalization
    /// to the input floats (multiplying them all by the inverse of the number
    /// of sum terms provides a fine worst-case bound, at the expense of of
    /// staturating subnormal values to 0) or sorting numbers in a suitable
    /// manner to ensure that positive/negative cancelations happen early enough
    /// in the summation process.
    typedef struct accumulator_s {
        /// Set of words containing fixed-point data
        ///
        /// Words are ordered by increasing weight, so the first word represents
        /// the lowest-order bits (where subnormal data goes), the second word
        /// represents bits that have a value 2^64 higher, etc.
        uint64_t words[NUM_ACCUMULATOR_WORDS];

        /// Index of the highest order word with a nonzero value, or 0 if all
        /// inner words are set to zero.
        ///
        /// Tracking this word enables O(1) decisions about how to perform
        /// subtractions in our sign-magnitude representation, and makes
        /// conversion back to binary64 easier as a bonus.
        size_t highest_word_idx;

        /// Truth that the accumulator is negative (sign bit)
        ///
        /// We use sign-magnitude representation because in the context of big
        /// integers it lets us store the sign information only once, instead of
        /// storing it once per inner word.
        bool negative;
    } accumulator_t;

    /// \ref accumulator_t value that corresponds to floating-point 0.0
    ///
    #define ACCUMULATOR_ZERO  ((accumulator_t){ 0 })

    /// Unsigned floating point addend to an \ref accumulator_t, in
    /// floating-word representation
    ///
    /// One important step of adding a floating-point number into a \ref
    /// accumulator_t is to convert its magnitude into a pair of integer words
    /// that match the accumulator's internal storage layout.
    ///
    /// Because these words are effectively a sparse representation of an \ref
    /// accumulator_t where most words are zero and `low_word_idx` effectively
    /// acts as an exponent in base 2^64, we call this a floating-word
    /// representation.
    typedef struct unsigned_addend_s {
        /// Translation of the floating-point addend into a pair of word addends
        /// to \ref accumulator_t
        ///
        /// We need a pair of word because for some exponent values, the
        /// significand will straddle a bigint word boundary. Following \ref
        /// accumulator_t's internal word layout, the first word is the
        /// low-order word and the second word is the high-order word.
        uint64_t words[2];

        /// Index of the word of the target \ref accumulator_t that `word[0]`
        /// should be added to.
        size_t low_word_idx;
    } unsigned_addend_t;

    /// Convert an unsigned floating point addend into a floating-word addend
    ///
    /// \param significand is the significand of the addend, which are the
    ///                    fraction bits of its IEEE-754 representation with the
    ///                    implicit leading bit additionally set for normal
    ///                    numbers.
    /// \param zero_based_exponent is a biased exponent which, unlike the
    ///                            standard IEEE-754 biased exponent, has a
    ///                            consistent definition for 0 (subnormal
    ///                            numbers) and 1+ (normal numbers). It always
    ///                            represents the amount of binary places by
    ///                            which the significand must be shifted before
    ///                            it is added to the bigint represented by the
    ///                            inner words.
    ///
    /// \returns a word-based representation of a floating point addend that is
    ///          destined to be added into an \ref accumulator_t
    static inline
    unsigned_addend_t compute_unsigned_addend(uint64_t significand,
                                              size_t zero_based_exponent) {
        assert((significand & SIGNIFICAND_MASK_F64) == significand);
        assert(zero_based_exponent < NUM_FINITE_EXPONENTS_F64);

        // Translate the zero_based_exponent into a (word index, bit offset)
        // coordinate within an accumulator_t
        size_t low_word_idx = zero_based_exponent / BITS_PER_ACC_WORD;
        assert(low_word_idx < NUM_ACCUMULATOR_WORDS);
        const size_t low_bit_idx = zero_based_exponent % BITS_PER_ACC_WORD;

        // Generate the word-aligned addend
        const uint64_t low_addend = significand << low_bit_idx;
        const uint64_t high_addend =
            (low_bit_idx != 0) ? significand >> (BITS_PER_ACC_WORD - low_bit_idx)
                               : 0;

        // Return the final word-based addend
        return (unsigned_addend_t){
            .words = { low_addend, high_addend },
            .low_word_idx = low_word_idx
        };
    }

    /// Add or subtract an \ref unsigned_addend_t into a \ref accumulator_t,
    /// under the assumption that if the term is subtracted, its magnitude is
    /// smaller than or equal to that of the accumulator.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param acc must be a valid \ref accumulator_t such as \ref
    ///            ACCUMULATOR_ZERO.
    /// \param magnitude is the term that must be added to or subtracted from
    ///                  the accumulator's inner magnitude words. When
    ///                  subtracting, its magnitude must be smaller than or
    ///                  equal to that of `acc`.
    /// \param accumulate_return_carry is a hook which adds or subtracts a word
    ///                                of the term from a word of the
    ///                                accumulator, and returns whether the
    ///                                addition or subtraction yielded a carry
    ///                                that must be propagated to the next
    ///                                accumulator word.
    /// \param update_highest_idx is a hook that can update the
    ///                           `highest_word_idx` of acc  knowing the
    ///                           highest-order word index where data was
    ///                           integrated or carries have been propagated. It
    ///                           must be chosen consistently with
    ///                           `accumulate_return_carry`.
    UDIPE_NON_NULL_ARGS
    static inline
    void accumulate_without_underflow(
        accumulator_t* acc,
        unsigned_addend_t magnitude,
        bool (*accumulate_return_carry)(uint64_t* /* acc */,
                                        uint64_t /* contribution */),
        void (*update_highest_idx)(accumulator_t* /* acc */,
                                   size_t /* highest_modified_idx */)
    ) {
        // Integrate the low-order word of the addend/subtrahend
        const size_t low_word_idx = magnitude.low_word_idx;
        assert(low_word_idx < NUM_ACCUMULATOR_WORDS);
        const uint64_t low_word = magnitude.words[0];
        tracef("Accumulating magnitude[0] = %#018zx into accumulator[%zu] = %#018zx...",
               low_word, low_word_idx, acc->words[low_word_idx]);
        bool carry = accumulate_return_carry(&acc->words[low_word_idx],
                                             low_word);
        tracef("...yields new accumulator[%zu] = %#018zx and carry %d.",
               low_word_idx, acc->words[low_word_idx], carry);

        // Track the highest-order accumulator word that was modified
        size_t highest_modified_idx = low_word_idx;

        // Carry propagation can't overflow the high-order word of the
        // addend/subtrahend because even in the worst case where the significand is
        // shifted by 63 binary places in the low word, there's still >= 1 unset
        // high-order bit in high_word.
        uint64_t high_word = magnitude.words[1];
        high_word += (uint64_t)carry;
        if (carry) {
            tracef("Propagated carry %d into high_word -> %#018zx.",
                   carry, high_word);
        }
        carry = false;
        assert(high_word >= magnitude.words[1]);

        // What can overflow, however, is the addition of the high word itself,
        // which will happen in the edge case where low_word_idx maps into the
        // highest order word of the accumulator and the accumulator overflows
        // as a result of carry propagation. When this happens, we have already
        // overflown the exponent range of double by a fair margin anyway...
        const size_t high_word_idx = low_word_idx + 1;
        if (high_word != 0) {
            if (high_word_idx >= NUM_ACCUMULATOR_WORDS) {
                assert(carry && high_word == 1);
                exit_with_error("Encountered an accumulator_t add overflow. "
                                "You can avoid this by normalizing inputs.");
            }
            tracef("Accumulating high_word = %#018zx into accumulator[%zu] = %#018zx...",
                   high_word, high_word_idx, acc->words[high_word_idx]);
            carry = accumulate_return_carry(&acc->words[high_word_idx],
                                            high_word);
            tracef("...yields new accumulator[%zu] = %#018zx and carry %d.",
                   high_word_idx, acc->words[high_word_idx], carry);
            highest_modified_idx = high_word_idx;
        }

        // Beyond that, we just keep propagating carries until there is no carry
        // anymore or we overflow the accumulator trying to propagate carries.
        size_t carry_idx = highest_modified_idx + 1;
        while (carry) {
            if (carry_idx >= NUM_ACCUMULATOR_WORDS) {
                exit_with_error(
                    "Encountered an accumulator_t carry propagation overflow! "
                    "You can avoid this by normalizing inputs."
                );
            }
            tracef("Propagating carry to accumulator[%zu] = %#018zx...",
                   carry_idx, acc->words[carry_idx]);
            carry = accumulate_return_carry(&acc->words[carry_idx],
                                            1);
            tracef("...yields new accumulator[%zu] = %#018zx and carry %d.",
                   carry_idx, acc->words[carry_idx], carry);
            highest_modified_idx = carry_idx++;
        }

        // Update the accumulator's highest_word_idx
        tracef("Updating highest accumulator idx knowing we modified words up to #%zu...",
               highest_modified_idx);
        update_highest_idx(acc, highest_modified_idx);
    }

    /// `accumulate_return_carry` hook for additions
    ///
    /// This is the `accumulate_return_carry` hook that can be passed to
    /// `accumulate_without_underflow` when adding a same-sign addend into an
    /// \ref accumulator_t's inner storage words.
    ///
    /// \param acc_word is the accumulator word that will be updated by this
    ///                 word summation pass.
    /// \param addend is the addend that will be added into the accumulator's
    ///               word.
    ///
    /// \returns the truth that a carry must be propagated to the next
    ///          accumulator word.
    UDIPE_NON_NULL_ARGS
    static inline
    bool add_inplace_return_carry(uint64_t* acc_word, uint64_t addend) {
        return add_with_carry_u64(false,
                                  *acc_word,
                                  addend,
                                  acc_word);
    }

    /// `accumulate_return_carry` hook for subtractions
    ///
    /// This is the `accumulate_return_carry` hook that can be passed to
    /// `accumulate_without_underflow` when subtracting an opposite-sign addend
    /// into an \ref accumulator_t's inner storage words.
    ///
    /// \param acc_word is the accumulator word that will be updated by this
    ///                 word summation pass.
    /// \param subtrahend is the subtrahend that will be subtracted from the
    ///                   accumulator's matching word.
    ///
    /// \returns the truth that a carry must be propagated to the next
    ///          accumulator word.
    UDIPE_NON_NULL_ARGS
    static inline
    bool sub_inplace_return_carry(uint64_t* acc_word, uint64_t subtrahend) {
        return subtract_with_carry_u64(false,
                                       *acc_word,
                                       subtrahend,
                                       acc_word);
    }

    /// `update_highest_idx` hook for additions
    ///
    /// This is the `update_highest_idx` hook that can be passed to
    /// `accumulate_without_underflow` when adding a same-sign addend into an
    /// \ref accumulator_t's inner storage words.
    ///
    /// \param acc is the accumulator in which a new term was previously added.
    /// \param highest_modified_idx indicates the highest-order storage word of
    ///                             `acc` that was modified (increased in this
    ///                             case) by the summation process.
    UDIPE_NON_NULL_ARGS
    static inline
    void update_highest_idx_after_add(accumulator_t* acc,
                                      size_t highest_modified_idx) {
        assert(highest_modified_idx < NUM_ACCUMULATOR_WORDS);
        if (highest_modified_idx > acc->highest_word_idx) {
            assert(acc->words[highest_modified_idx] != 0);
            acc->highest_word_idx = highest_modified_idx;
        }
    }

    /// `update_highest_idx` hook for subtractions
    ///
    /// This is the `update_highest_idx` hook that can be passed to
    /// `accumulate_without_underflow` when subtracting a same-sign addend from
    /// an \ref accumulator_t's inner storage words.
    ///
    /// \param acc is the accumulator in which a new term was previously added.
    /// \param highest_modified_idx indicates the highest-order storage word of
    ///                             `acc` that was modified (decreased in this
    ///                             case) by the subtraction process.
    UDIPE_NON_NULL_ARGS
    static inline
    void update_highest_idx_after_sub(accumulator_t* acc,
                                      size_t highest_modified_idx) {
        assert(highest_modified_idx <= acc->highest_word_idx);
        if (highest_modified_idx < acc->highest_word_idx) return;
        if (acc->words[highest_modified_idx] != 0) return;
        for (ptrdiff_t word_idx = (ptrdiff_t)highest_modified_idx - 1;
             word_idx > 0;
             --word_idx)
        {
            if (acc->words[word_idx] != 0) {
                acc->highest_word_idx = word_idx;
                return;
            }
        }
        acc->highest_word_idx = 0;
    }

    /// Truth that an accumulator's magnitude is strictly less that of a
    /// subtrahend, which is presumed to be nonzero and of opposite sign
    ///
    /// When this is true (which should not happen often on real data), we need
    /// to flip the substraction/negative addition around and subtract the
    /// accumulator from the subtrahend, which will become the new accumulator.
    ///
    /// \param acc must be a valid \ref accumulator_t such as \ref
    ///            ACCUMULATOR_ZERO.
    /// \param subtrahend is the term that will be subtracted from `acc`'s
    ///                   magnitude, which must be nonzero.
    ///
    /// \returns the truth that `acc->words` have a strictly smaller magnitude
    ///          than `subtrahend`.
    UDIPE_NON_NULL_ARGS
    static inline
    bool accumulator_lt_nonzero_subtrahend(const accumulator_t* acc,
                                           unsigned_addend_t subtrahend) {
        // Handle trivial cases where the difference in magnitude can be
        // assessed just by comparing the position of the highest-order words
        const uint64_t subtrahend_low_word = subtrahend.words[0];
        const uint64_t subtrahend_high_word = subtrahend.words[1];
        const size_t subtrahend_high_word_idx = subtrahend.low_word_idx + 1;
        if (acc->highest_word_idx > subtrahend_high_word_idx) {
            trace("acc has higher magnitude because its highest set word is higher.");
            return false;
        } else if (acc->highest_word_idx < subtrahend.low_word_idx) {
            trace("acc has lower magnitude because its highest set word is lower.");
            assert((subtrahend_low_word | subtrahend_high_word) != 0);
            return true;
        }
        assert(acc->highest_word_idx == subtrahend.low_word_idx
               || acc->highest_word_idx == subtrahend_high_word_idx);

        // Handle easy case where the subtrahend's low-order word is aligned
        // with the highest-order word of the accumulator, which means that any
        // nonzero subtrahend high-order word implies accumulator < subtrahend.
        const uint64_t acc_high_word = acc->words[acc->highest_word_idx];
        if (acc->highest_word_idx == subtrahend.low_word_idx) {
            if (subtrahend_high_word != 0) {
                trace("acc has lower magnitude because the addend high word is "
                      "nonzero and located higher than the acc high word.");
                return true;
            }
            tracef("Magnitude comparison is fully determined by comparison of "
                   "acc->words[%zu] = %#zx and subtrahend->words[0] = %#zx",
                   acc->highest_word_idx, acc_high_word, subtrahend_low_word);
            return (acc_high_word < subtrahend_low_word);
        }
        assert(acc->highest_word_idx == subtrahend_high_word_idx);
        // Must be true by definition of subtrahend_high_word_idx
        assert(acc->highest_word_idx > 0);

        // Handle full subtract-with-carry logic
        const uint64_t acc_low_word = acc->words[acc->highest_word_idx - 1];
        // This is true even in the presence of a carry from the low word
        // subtraction because the carry can reduce the high word by at most one,
        // which is enough to take it to zero but not to take it below zero
        if (acc_high_word > subtrahend_high_word) {
            trace("acc has higher magnitude because same-index "
                  "subtrahend high word is lower.");
            return false;
        }
        if (acc_high_word < subtrahend_high_word) {
            trace("acc has lower magnitude because same-index "
                  "subtrahend high word is higher.");
            return true;
        }
        assert(acc_high_word == subtrahend_high_word);
        trace("acc has the same high word as subtrahend, "
              "magnitude comparison is determined by comparison of low words.");
        return (acc_low_word < subtrahend_low_word);
    }

    /// Add an addend of opposite sign and greater magnitude into an
    /// accumulator, handling the resulting accumulator underflow.
    ///
    /// \param acc must be a valid \ref accumulator_t such as \ref
    ///            ACCUMULATOR_ZERO.
    /// \param magnitude is the magnitude of the subtrahend that will be
    ///        subtracted from `acc->words`, which is assumed to be of opposite
    ///        sign.
    UDIPE_NON_NULL_ARGS
    void accumulator_subtract_with_underflow(accumulator_t* acc,
                                             unsigned_addend_t subtrahend);

    /// Accumulate a pre-decoded binary64 number into an \ref accumulator_t
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param acc must be a valid \ref accumulator_t such as \ref
    ///            ACCUMULATOR_ZERO.
    /// \param significand is the significand of the addend, which are the
    ///                    fraction bits of its IEEE-754 representation with the
    ///                    implicit leading bit additionally set for normal
    ///                    numbers.
    /// \param zero_based_exponent is a biased exponent which, unlike the
    ///                            standard IEEE-754 biased exponent, has a
    ///                            consistent definition for 0 (subnormal
    ///                            numbers) and 1+ (normal numbers). It always
    ///                            represents the amount of binary places by
    ///                            which the significand must be conceptually
    ///                            shifted before it is added to the bigint
    ///                            represented by the inner words.
    /// \param negative is the truth that the addend is negative, i.e. it must
    ///                 be subtracted rather than added.
    UDIPE_NON_NULL_ARGS
    void accumulate_decoded_f64(accumulator_t* acc,
                                uint64_t significand,
                                size_t zero_based_exponent,
                                bool negative);

    /// Add a finite binary64 number into an \ref accumulator_t
    ///
    /// As mentioned in the type-level documentation, perfect precision is
    /// guaranteed but absence of overflow is not guaranteed, so normalizing
    /// addends to keep their magnitudes close to unity remains a prudent
    /// precaution.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param acc must be a valid \ref accumulator_t such as \ref
    ///            ACCUMULATOR_ZERO.
    /// \param value should be a finite number, i.e. any IEEE-754 number except
    ///              for infinities and NaNs.
    UDIPE_NON_NULL_ARGS
    static inline
    void accumulator_add_f64(accumulator_t* acc, double value) {
        // Decompose input value into fraction/exponent/sign
        const uint64_t value_bits = bitcast_f64_to_u64(value);
        const uint64_t fraction = value_bits & FRACTION_MASK_F64;
        const uint64_t raw_exponent = value_bits & EXPONENT_MASK_F64;
        const bool negative = (value_bits & SIGN_BIT_F64) != 0;
        tracef("Processing value %g (%a) with "
               "fraction %#015zx, biased exponent %#05zx (%zu), sign %d",
               value,
               value,
               fraction,
               raw_exponent >> EXPONENT_SHIFT_F64,
               raw_exponent >> EXPONENT_SHIFT_F64,
               negative);

        // Handle exponent special cases
        switch (raw_exponent) {
        case RAW_SUBNORMAL_EXPONENT_F64:
            // No implicit leading one for subnormal numbers
            accumulate_decoded_f64(acc,
                                   fraction,
                                   0,
                                   negative);
            break;
        case RAW_NONFINITE_EXPONENT_F64:
            // Nonfinite numbers are not welcome here
            exit_with_error("accumulator_t does not support infinities and NaNs");
        default:
            // Add the implicit leading bit of normal IEEE-754 numbers + account
            // for the implicit exponent shift that occurs as one shifts from
            // subnormal to normal numbers.
            const uint64_t significand = fraction | ((uint64_t)1 << FRACTION_BITS_F64);
            const size_t zero_based_exponent = (raw_exponent >> EXPONENT_SHIFT_F64) - 1;
            accumulate_decoded_f64(acc,
                                   significand,
                                   zero_based_exponent,
                                   negative);
        }
    }

    /// Turn an \ref accumulator_t back into a binary64 number
    ///
    /// \param acc must be a valid \ref accumulator_t such as \ref
    ///            ACCUMULATOR_ZERO.
    ///
    /// \returns the binary64 translation of the current contents of `acc`,
    ///          which should be correctly rounded down to the last digit.
    UDIPE_NON_NULL_ARGS
    double accumulator_to_f64(const accumulator_t* acc);

    /// \}


    #ifdef UDIPE_BUILD_TESTS
        /// Unit tests
        ///
        /// This function runs all the unit tests for this module. It must be called
        /// within the scope of with_logger().
        void numeric_unit_tests();
    #endif

#endif  // UDIPE_BUILD_BENCHMARKS