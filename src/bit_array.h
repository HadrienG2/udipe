#pragma once

//! \file
//! \brief Bit array (a.k.a bit map, bit set, bit string, bit vector)
//!
//! This module provides tools for declaring and manipulating bit arrays, which
//! are optimized containers for tracking arrays of boolean values. Typical uses
//! for this data structure include...
//!
//! - Tracking which element of a pool of resources are in use.
//! - Tracking which threads from a thread pool are done with some task.
//!
//! The implementation of bit array operations is more efficient when the length
//! of the bit array is known at compile time to be a multiple of \ref
//! BITS_PER_WORD, which is why...
//!
//! - You are encouraged to enforce this granularity by e.g. allocating a bit
//!   array that is larger than you need and using "neutral" padding values that
//!   will never considered be considered as valid candidates in bit searches.
//!   * While \ref BITS_PER_WORD is CPU architecture specific, a bit array
//!     length that is a multiple of 64 will work fine on all popular CPU
//!     architectures at the time of writing.
//! - You are encouraged to use bit arrays with a length that is known at
//!   compile time. Failing that, you can get some of the associated performance
//!   benefits back by storing your length as a multiple of the \ref
//!   BITS_PER_WORD and making sure that the compiler's optimizer can see the
//!   multiplication of that "length in words" by \ref BITS_PER_WORD.
//! - All bit array operations are inline functions, allowing the compiler to
//!   exploit this granularity for optimization when it is present, along with
//!   other useful compile-time information like e.g. the precise bit value that
//!   you are setting or searching.

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/// \name Implementation details
/// \{

/// Divide `num` and `denom`, rounding upwards
///
/// \param num must be a side-effects-free integer expression
/// \param denom must be a side-effects-free integer value that does not
///              evaluate to zero
#define DIV_CEIL(num, denom) ((num / denom) + ((num % denom) != 0))

/// Unsigned machine word type used for bit storage
///
/// From the C language's perspective, a bit array is an array of \ref word_t.
typedef size_t word_t;

/// Number of bits within a \ref word_t
///
/// This links the amount of \ref word_t that a bit array is composed of, to the
/// amount of boolean values that it can hold internally.
///
/// Bit array operations perform best on bit arrays whose length is known at
/// compile time to be a multiple of this quantity.
#define BITS_PER_WORD (sizeof(word_t) * 8)

/// Amount of \ref word_t inside a bit array of specified length
///
/// \param length must be a side-effects-free integer expression.
#define BIT_ARRAY_WORDS(length)  DIV_CEIL((length), BITS_PER_WORD)

/// Maximum value of \ref word_t
///
/// From a bit array perspective, this is a \ref word_t where all bits are set.
#define WORD_MAX SIZE_MAX

/// Broadcast a boolean value to all bits of a \ref word_t
///
/// Returns a \ref word_t where all bits are set to the given `value`.
static inline word_t bit_broadcast(bool value) {
    return value ? WORD_MAX : 0;
}

/// Count the number of trailing zeros in a \ref word_t
///
/// \param word must not be zero
static inline size_t count_trailing_zeros(word_t word) {
    assert(word != (size_t)0);
    #ifdef __GNUC__
        return __builtin_ctzll(word);
    #else
        for (size_t bit = 0; bit < sizeof(word_t) * 8; ++bit) {
            if (word & (size_t)1) return bit;
            word >>= 1;
        }
    #endif
}

/// Count the number of bits that are set to 1 in a \ref word_t
///
/// \returns the word's population count aka Hamming weight
static inline size_t population_count(word_t word) {
    #ifdef __GNUC__
        return __builtin_popcountll(word);
    #else
        size_t population = 0;
        for (size_t bit = 0; bit < sizeof(word_t) * 8; ++bit) {
            population += word & (size_t)1;
            word >>= 1;
        }
        return population;
    #endif
}

/// \}


/// \name Bit array declaration
/// \{

/// Declare a bit array as a stack variable or struct member
///
/// This macro generates a declaration for a bit array variable called `name`,
/// capable of holding `length` bits, whose storage is allocated inline (i.e.
/// not on a separate heap allocation).
///
/// You can use this macro for the purpose of declaring either local bit array
/// variables or struct bit array members.
///
/// \param name must be a valid variable identifier, that is not already used
///             in the scope where this variable is called.
/// \param length dictates how many bits the bit array is capable of holding.
///               It should be a compile-time constant, otherwise this macro
///               will generate a Variable Lenght Array (VLA), which will have a
///               negative effect on compiler optimizations. In any case,
///               `length` must be a side-effects-free operation.
#define INLINE_BIT_ARRAY(name, length)  word_t name[BIT_ARRAY_WORDS(length)]

/// \}


/// \name Bit indexing
/// \{

/// Bit location within a bit array
///
/// This designates either the `offset`-th bit within the `word`-th word of a
/// particular bit array, or an invalid bit position (used for failed searches).
typedef struct bit_pos_s {
    size_t word; ///< Target word, or SIZE_MAX if invalid
    size_t offset; ///< Target bit within word, or SIZE_MAX if invalid
} bit_pos_t;

/// Invalid bit location within a bit array
///
/// Used as the return value of failed bit searches.
#define NO_BIT_POS ((bit_pos_t) { .word = SIZE_MAX, .offset = SIZE_MAX })

/// Convert a bit location to a linear index
///
/// This is typically used when using the result of a bit array search to inform
/// lookup into some associated array of resources.
///
/// \param bit must be a valid bit location
static inline size_t bit_pos_to_index(bit_pos_t bit) {
    assert(bit.word != SIZE_MAX);
    assert(bit.offset < BITS_PER_WORD);
    return bit.word * BITS_PER_WORD + bit.offset;
}

/// Convert a linear index to a bit location
///
/// This is typically used when mapping an entry of an array of resource into
/// the associated entry within a bit array.
///
/// \param index must be a valid linear index
static inline bit_pos_t index_to_bit_pos(size_t index) {
    assert(index != SIZE_MAX);
    return (bit_pos_t) {
        .word = index / BITS_PER_WORD,
        .offset = index % BITS_PER_WORD
    };
}

/// First bit location inside of a bit array
///
/// This marks the start of a bit array in commands that accept a bit location
/// range like bit_array_range_alleq(), as a left-inclusive bound, much like
/// index 0 designates the start of a C array.
///
/// See also bit_array_end().
#define BIT_ARRAY_START ((bit_pos_t) { .word = 0, .offset = 0 })

/// First invalid bit location past the end of an array of `length` bits
///
/// This marks the end of a bit array in commands that accept a bit location
/// range like bit_array_range_alleq(), as a right-exclusive bound, much like
/// typical C loops over arrays are controlled by an `i < length` condition.
///
/// See also \ref BIT_ARRAY_START.
static inline bit_pos_t bit_array_end(size_t length) {
    assert(length != SIZE_MAX);
    return index_to_bit_pos(length);
}

/// \}


/// \name Bit array operations
/// \{

/// Get the value of the Nth bit of a bit array
///
/// This tells whether a particular bit of a bit array is set.
///
/// \param bit_array must be a valid array of `length` bits
/// \param length must be the number of bits within `bit_array`
/// \param bit must be a valid bit position inside of `bit_array`
static inline bool bit_array_get(const word_t bit_array[],
                                 size_t length,
                                 bit_pos_t bit) {
    assert(bit_pos_to_index(bit) < length);
    return (bit_array[bit.word] & ((word_t)1 << bit.offset)) != 0;
}

/// Set the value of the Nth bit of a bit array
///
/// This lets you adjust the value of a particular bit of a bit array.
///
/// \param bit_array must be a valid array of `length` bits
/// \param length must be the number of bits within `bit_array`
/// \param bit must be a valid bit position inside of `bit_array`
/// \param value is the value to which this bit will be set
static inline void bit_array_set(word_t bit_array[],
                                 size_t length,
                                 bit_pos_t bit,
                                 bool value) {
    assert(bit_pos_to_index(bit) < length);
    if (value) {
        bit_array[bit.word] |= ((word_t)1 << bit.offset);
    } else {
        bit_array[bit.word] &= ~((word_t)1 << bit.offset);
    }
}

/// Count the number of bits within a bit array that are set to some value
///
/// \param bit_array must be a valid array of `length` bits
/// \param length must be the number of bits within `bit_array`
/// \param value is the value whose occurences will be counted
static inline size_t bit_array_count(const word_t bit_array[],
                                     size_t length,
                                     bool value) {
    const size_t num_full_words = length / BITS_PER_WORD;
    const size_t remaining_bits = length % BITS_PER_WORD;

    // For full words, we normalize into the problem of looking for bits that
    // are set to one, then invoke the popcount intrinsic.
    size_t result = 0;
    for (size_t word = 0; word < num_full_words; ++word) {
        word_t target = bit_array[word];
        if (!value) target = ~target;
        result += population_count(target);
    }

    // If there is a trailing partial word, the logic is the same except we
    // mask out the uninitialized leading bits after normalization.
    if (remaining_bits > 0) {
        word_t target = bit_array[num_full_words];
        if (!value) target = ~target;
        target &= ((word_t)1 << remaining_bits) - 1;
        result += population_count(target);
    }
    return result;
}

/// Truth that a region of a bit array contains only a certain value
///
/// Check if all entries within `bit_array` from bit `start` (included) to bit
/// `end` (excluded) are equal to `value`.
///
/// In the common case where you want to check if the entire bit array is equal
/// to `value`, you can use the following pattern:
///
/// ```c
/// bool result = bit_array_range_alleq(bit_array,
///                                     length,
///                                     BIT_ARRAY_START,
///                                     bit_array_end(length),
///                                     value);
/// ```
///
/// \param bit_array must be a valid array of `length` bits.
/// \param length must be the number of bits within `bit_array`.
/// \param start designates the first bit to be checked, which must be in range
///              for this bit array. Use \ref BIT_ARRAY_START if you want to
///              cover every bit from the start of `bit_array`.
/// \param end designates the bit **past** the last bit to be checked. In other
///            words, if `start == end`, no bit will be checked. This bit
///            position can be in range or one bit past the end of `bit_array`.
///            Use \link #bit_array_end `bit_array_end(length)`
///            \endlink if you want to cover every bit until the end of
///            `bit_array`.
/// \param value is the bit value that is expected.
///
/// \returns the truth that all bits in range `[start; end[` are set to `value`.
static inline bool bit_array_range_alleq(const word_t bit_array[],
                                         size_t length,
                                         bit_pos_t start,
                                         bit_pos_t end,
                                         bool value) {
    assert(bit_pos_to_index(start) < length
           || (start.word == end.word && start.offset == end.offset));
    assert(bit_pos_to_index(end) <= length);

    // For each word covered by the selected range...
    for (size_t word = start.word; word <= end.word; ++word) {
        // Ignore end word (which may not exist) if it has no active bit
        if ((word == end.word) && (end.offset == 0)) break;

        // Load the word of interest
        word_t target = bit_array[word];

        // Normalize into the problem of looking for zeroed bits
        if (value) target = ~target;

        // In the last word, zero bits past the end
        if (word == end.word) target &= ((word_t)1 << end.offset) - 1;

        // In the first word, discard bits before the start
        if (word == start.word) target >>= start.offset;

        // If any of the remaining bits is set, then one bit within the selected
        // region of the original word was not equal to the user-expected value.
        if (target != 0) return false;
    }

    // If the loop above didn't exit, then all bits have the right value
    return true;
}

/// Fill a region of a bit array with a uniform bit pattern
///
/// Set all entries within `bit_array` from bit `start` (included) to bit
/// `end` (excluded) to `value`.
///
/// In the common case where you want to set the entire bit array to `value`,
/// you can use the following pattern:
///
/// ```c
/// bool result = bit_array_range_set(bit_array,
///                                   length,
///                                   BIT_ARRAY_START,
///                                   bit_array_end(length),
///                                   value);
/// ```
///
/// \param bit_array must be a valid array of `length` bits.
/// \param length must be the number of bits within `bit_array`.
/// \param start designates the first bit to be set, which must be in range
///              for `bit_array` unless `start == end`. Use \ref BIT_ARRAY_START
///              if you want to cover every bit from the start of the bit array.
/// \param end designates the bit **past** the last bit to be set. In other
///            words, if `start == end`, no bit will be set. This bit
///            position can be in range or one bit past the end of the bit array.
///            Use \link #bit_array_end `bit_array_end(length)` \endlink if you
///            want to cover every bit until the end of `bit_array`.
/// \param value is the bit value that will be set.
static inline void bit_array_range_set(word_t bit_array[],
                                       size_t length,
                                       bit_pos_t start,
                                       bit_pos_t end,
                                       bool value) {
    assert(bit_pos_to_index(start) < length
           || (start.word == end.word && start.offset == end.offset));
    assert(bit_pos_to_index(end) <= length);

    // Filling an entire bit array word means assigning this value to it
    const word_t broadcast = bit_broadcast(value);

    // We mostly do this for each covered word, except for the first and last
    // one where some masking may be necessary
    for (size_t word = start.word; word <= end.word; ++word) {
        // Fast path for words other than the start and the end word, and for
        // start words that are fully covered.
        //
        // This fast path must not be removed as it is necessary for the
        // correctness of the partial word computation below.
        if ((word > start.word || start.offset == 0) && word < end.word) {
            bit_array[word] = broadcast;
            continue;
        }

        // Ignore the end word (which may not exist) if it has no active bit
        if (word == end.word && end.offset == 0) break;

        // Load the current value of the word of interest
        const word_t current = bit_array[word];

        // Set up a mask to select which bits will be modified
        //
        // It is not obvious that the code below works without running into
        // C bit shift overflow UB, so here is a mathematical proof:
        //
        // 1. The fast path above guarantees that both of these are true:
        //    a. If control reaches this point, then either word == start.word
        //       or word == end.word is true. Both of these may be true.
        //    b. If control reaches this point and word == start.word, then
        //       start.offset != 0 must be true as well.
        // 2. By design of bit_pos_t, bit.offset is in range [0; BITS_PER_WORD[,
        //    except for invalid positions which are not valid inputs here.
        // 3. From the above, we can prove that offset_delta will always be
        //    in range [0; BITS_PER_WORD[ by enumerating all control flow
        //    possibilities for word:
        //    - (word != start.word && word != end.word): This case is forbidden
        //      by 1a and thus cannot be encountered.
        //    - (word == start.word && word != end.word): In this case
        //      offset_delta is BITS_PER_WORD - start.offset. Per points 1b and
        //      2 we know that start.offset is in range [1; BITS_PER_WORD[, so
        //      this difference must be in range [1; BITS_PER_WORD[.
        //    - (word != start.word && word == end.word): In this case
        //      offset_delta is just end.offset, which per point 2 must be in
        //      range [0; BITS_PER_WORD[.
        //    - (word == start.word && word == end.word): In this case,
        //      start.offset is in range [1; BITS_PER_WORD[ per points 1b and 2
        //      and end.offset is in range [0; BITS_PER_WORD[ per point 2.
        //      Furthermore, the conditional below ensures that
        //      start.offset <= end.offset. From all this, it follows that the
        //      difference end.offset - start.offset is positive and in range
        //      [0; BITS_PER_WORD - 1[.
        // 4. Given the proof from point 3 that offset_delta is in range
        //    [0; BITS_PER_WORD[, we know that the C expression
        //    (1 << offset_delta) does not invoke bitshift UB and is therefore a
        //    valid way to construct a word with bits [0; offset_delta[ set.
        // 5. Shifting that word left by start_offset bits, which is legal
        //    because per point 2 start_offset is in range [0; BITS_PER_WORD[,
        //    will yield a word where bits at positions
        //    [start_offset; offset_delta+start_offset[ is set.
        // 7. Simplifying the above expression tells us that we have therefore
        //    built a word were bits at positions [start_offset; end_offset[
        //    are set, which is what we were looking for.
        const size_t start_offset = (word == start.word) ? start.offset : 0;
        const size_t end_offset = (word == end.word) ? end.offset : BITS_PER_WORD;
        // Fast path for empty range AND ensures correctness, see above
        if (start_offset > end_offset) continue;
        const size_t offset_delta = end_offset - start_offset;
        const word_t fill_mask = (((word_t)1 << offset_delta) - 1) << start_offset;

        // Update bit array with the masked mixture of the current and new value
        bit_array[word] = (broadcast & fill_mask) | (current & ~fill_mask);
    }
}

/// Find the first bit that has a certain value within a bit array
///
/// \param bit_array must be a valid array of `length` bits.
/// \param length must be the number of bits within `bit_array`.
/// \param value is the bit value that will be searched within `bit_array`.
/// \returns The position of the first bit that has the desired value, or
///          \ref NO_BIT_POS to indicate absence of the desired value.
static inline bit_pos_t bit_array_find_first(const word_t bit_array[],
                                             size_t length,
                                             bool value) {
    // Quickly skip over words where the value isn't present
    const word_t empty_word = bit_broadcast(!value);
    const size_t end_word = BIT_ARRAY_WORDS(length);
    size_t word;
    for (word = 0; word < end_word; ++word) {
        if (bit_array[word] != empty_word) break;
    }

    // If we skipped all words, we know the value is absent from the bit array
    if (word == end_word) return NO_BIT_POS;

    // Otherwise, check the word on which we ended up
    const size_t num_full_words = length / BITS_PER_WORD;
    const size_t remaining_bits = length % BITS_PER_WORD;
    word_t found_word = bit_array[word];

    // Normalize into the problem of looking for set bits
    if (!value) found_word = ~found_word;

    // Handle false positives related to padding bits by clearing the padding
    // (this is valid since we're now looking for set bits only)
    if ((word == num_full_words) && (remaining_bits != 0)) {
        found_word &= ((word_t)1 << remaining_bits) - 1;
        if (found_word == 0) return NO_BIT_POS;
    }

    // If control reached this point, we know that this is not a padding issue
    // and there is a set bit, so we can let CTZ find it for us.
    const size_t offset = count_trailing_zeros(found_word);
    return (bit_pos_t) {
        .word = word,
        .offset = offset,
    };
}

/// Find the next bit that has a certain value within a bit array
///
/// This is meant to be used when iterating over bits that have a certain value
/// within a certain bit array. It receives a \ref bit_pos_t that was typically
/// returned by bit_array_find_first() or a previous call to
/// bit_array_find_next(), and returns the location of the next value of
/// interest within the bit array.
///
/// \param bit_array must be a valid array of `length` bits.
/// \param length must be the number of bits within `bit_array`.
/// \param previous must be a valid bit position inside of `bit_array`. The
///        search will begin after this bit. It will not include this bit unless
///        `wraparound` is enabled.
/// \param wraparound indicates whether the search should wrap around to the
///        start of `bit_array` if no occurence of `value` is found. If the
///        search does wrap around, then it will terminate unsuccessfully if
///        `previous` is reached again and does not contain `value`.
/// \param value is the bit value that will be searched within `bit_array`.
/// \returns The position of the first bit after `previous` (including possible
///          search wraparound) that has the desired value, or \ref NO_BIT_POS
///          to indicate absence of the desired value.
static inline bit_pos_t bit_array_find_next(const word_t bit_array[],
                                            size_t length,
                                            bit_pos_t previous,
                                            bool wraparound,
                                            bool value) {
    // Check safety invariant in debug build
    assert(bit_pos_to_index(previous) < length);

    // If we were not looking at the last bit of the previous word, then
    // continue search within this word.
    const size_t num_full_words = length / BITS_PER_WORD;
    const size_t remaining_bits = length % BITS_PER_WORD;
    const bool previous_incomplete = previous.word == num_full_words;
    const size_t previous_bits = previous_incomplete ? remaining_bits : BITS_PER_WORD;
    if (previous.offset != (previous_bits - 1)) {
        // Extract previous word
        word_t previous_word = bit_array[previous.word];

        // Normalize into the problem of looking for set bits
        if (!value) previous_word = ~previous_word;

        // If this was the last word of an incomplete bit array, mask out its
        // padding bits so they do not result in search false positives
        if (previous_incomplete) previous_word &= ((word_t)1 << remaining_bits) - 1;

        // Eliminate bits which we have previously looked at
        const size_t dropped_bits = previous.offset + 1;
        previous_word >>= dropped_bits;

        // If there is a next set bit, return its position
        if (previous_word != 0) {
            const size_t extra_offset = count_trailing_zeros(previous_word);
            return (bit_pos_t) {
                .word = previous.word,
                .offset = dropped_bits + extra_offset,
            };
        }
    }

    // Look inside the remaining words from the bit array, if any
    const size_t num_words = BIT_ARRAY_WORDS(length);
    if (previous.word != num_words - 1) {
        const size_t word_offset = previous.word + 1;
        bit_pos_t pos = bit_array_find_first(
            bit_array + word_offset,
            length - word_offset * BITS_PER_WORD,
            value
        );
        if (pos.word != SIZE_MAX) {
            pos.word += word_offset;
            return pos;
        }
    }

    // In absence of wraparound, we are done
    if (!wraparound) return NO_BIT_POS;

    // Otherwise, look into bits before and including the `previous` bit
    return bit_array_find_first(bit_array,
                                bit_pos_to_index(previous) + 1,
                                value);
}

/// \}


/// \name Unit tests
/// \{

#ifdef UDIPE_BUILD_TESTS
    /// Unit tests for bit arrays
    ///
    /// This function runs all the unit tests for bit arrays. It must be called
    /// within the scope of with_logger().
    void bit_array_unit_tests();
#endif

/// \}
