#pragma once

//! \file
//! \brief Bitmap
//!
//! This is a simple container that is typically used for tracking the usage of
//! each element of an array of resources.
//!
//! Its implementation is more efficient when the number of ressources that is
//! tracked is a multiple of \ref BITS_PER_WORD, which is why...
//!
//! - You are encouraged to enforce this granularity by e.g. allocating a bitmap
//!   that is larger than you need and using sentinel values at the end such
//!   that the extra elements are never considered in bit searches.
//!   * While \ref BITS_PER_WORD is CPU architecture specific, a bitmap size
//!     that is a multiple of 64 will work fine on all hardware at the time of
//!     writing.
//! - You are encouraged to use bitmaps with a size that is known at compile
//!   time. Failing that, you can get some performance back by storing your
//!   dynamic side as a multiple of the \ref BITS_PER_WORD and make sure that
//!   the compiler's optimizer can see the multiplication.
//! - All bitmap operations are inline functions, allowing the compiler to
//!   exploit the granularity when it is present.

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/// \name Implementation details
/// \{

/// Divide `num` and `denom`, rounding upwards
///
/// \param num must be side-effects-free integer expression
/// \param denom must be a side-effects-free integer value that does not
///              evaluate to zero
#define DIV_CEIL(num, denom) ((num / denom) + ((num % denom) != 0))

/// Unsigned machine word type used for bit storage
///
/// At the C language level, a bitmap is just an array of \ref word_t.
typedef size_t word_t;

/// Number of bits within a \ref word_t
///
/// This links the amount of \ref word_t that a bitmap is composed of the the
/// amount of bits that it can hold internally.
///
/// Bitmap operations perform best on bitmaps whose capacity is a multiple of
/// this quantity.
#define BITS_PER_WORD (sizeof(word_t) * 8)

/// Amount of \ref word_t inside a bitmap of specified capacity
///
/// \param capacity must be an integer expression which can safely be evaluated
///                 multiple times.
#define BITMAP_WORDS(capacity)  DIV_CEIL((capacity), BITS_PER_WORD)

/// Maximum value of \ref word_t
///
/// From a bitmap perspective, this is a \ref word_t where all bits are set.
#define WORD_MAX SIZE_MAX

/// Broadcast a boolean value to all lanes of a bitmap word
///
/// Returns a \ref word_t where all bits are set to the given `value`.
static inline word_t bit_broadcast(bool value) {
    return value ? WORD_MAX : 0;
}

/// Count the number of trailing zeros in a word of the bitmap
///
/// \param x must not be zero
static inline size_t count_trailing_zeros(word_t x) {
    return __builtin_ctzll(x);
}

/// \}


/// \name Bitmap declaration
/// \{

/// Declare a bitmap on the stack or as a struct member.
///
/// This macro generates a declaration for a bitmap variable called `name`,
/// capable of holding `capacity` bits, which is allocated inline (i.e. not
/// on a separate heap allocation).
///
/// You can use this macro for the purpose of declaring either local bitmap
/// variables or struct bitmap members.
///
/// \param name must be a valid variable identifier, that is not already used
///             in the scope where this variable is called.
/// \param capacity dictates how many bits the bitmap is
///                 capable of holding. It should be a compile-time constant,
///                 otherwise VLAs will be used, which will have a negative
///                 effect on compiler optimizations.
#define INLINE_BITMAP(name, capacity)  word_t name[BITMAP_WORDS(capacity)]

/// \}


/// \name Bit indexing
/// \{

/// Bit location within a bitmap
///
/// This designates either the `offset`-th bit within the `word`-th word of a
/// particular bitmap, or an invalid location (used for failed searches).
typedef struct bit_pos_s {
    size_t word; ///< Target word, or SIZE_MAX for an invalid position
    size_t offset; ///< Target bit within the target word
} bit_pos_t;

/// First bit location inside of a bitmap
///
/// This plays the same role as index 0 in regular arrays.
#define FIRST_BIT_POS ((bit_pos_t) { .word = 0, .offset = 0 })

/// Invalid bit location within a bitmap
///
/// Used as a return value in failed bitmap searches
#define NO_BIT_POS ((bit_pos_t) { .word = SIZE_MAX, .offset = SIZE_MAX })

/// Convert a bitmap location to a linear index
///
/// \param bit must be a valid bitmap index
static inline size_t bit_pos_to_index(bit_pos_t bit) {
    assert(bit.word != SIZE_MAX);
    return bit.word * BITS_PER_WORD + bit.offset;
}

/// Convert a linear index to a bitmap location
///
/// \param index must be a valid bitmap index
static inline bit_pos_t index_to_bit_pos(size_t index) {
    assert(index != SIZE_MAX);
    return (bit_pos_t) {
        .word = index / BITS_PER_WORD,
        .offset = index % BITS_PER_WORD
    };
}

/// \}


/// \name Bitmap operations
/// \{

/// Get the value of the Nth bit of a bitmap
///
/// This tells whether a particular bit of a bitmap is set.
///
/// \param bitmap must be a valid bitmap of capacity `capacity`
/// \param capacity must be the bit storage capacity of `bitmap`
/// \param bit must be a valid bit position inside of `bitmap`
static inline bool bitmap_get(word_t bitmap[],
                              size_t capacity,
                              bit_pos_t bit) {
    assert(bit_pos_to_index(bit) < capacity);
    return (bitmap[bit.word] & ((word_t)1 << bit.offset)) != 0;
}


/// Set the value of the Nth bit of a bitmap
///
/// This lets you adjust the value of a particular bit of a bitmap.
///
/// \param bitmap must be a valid bitmap of capacity `capacity`
/// \param capacity must be the bit storage capacity of `bitmap`
/// \param bit must be a valid bit position inside of `bitmap`
/// \param value is the value to which this bit will be set
static inline void bitmap_set(word_t bitmap[],
                              size_t capacity,
                              bit_pos_t bit,
                              bool value) {
    assert(bit_pos_to_index(bit) < capacity);
    if (value) {
        bitmap[bit.word] |= ((word_t)1 << bit.offset);
    } else {
        bitmap[bit.word] &= ~((word_t)1 << bit.offset);
    }
}

// TODO: Allow fill() and all() to operate on a subset of the bitmap

/// Truth that a bitmap is filled with a uniform bit pattern
///
/// Check if all in-bounds entries within `bitmap` are equal to `value`.
///
/// \param bitmap must be a valid bitmap of capacity `capacity`
/// \param capacity must be the bit storage capacity of `bitmap`
/// \param value is the bit value that is expected everywhere in the bitmap
static inline bool bitmap_all(const word_t bitmap[],
                              size_t capacity,
                              bool value) {
    const size_t num_full_words = capacity / BITS_PER_WORD;
    const size_t remaining_bits = capacity % BITS_PER_WORD;

    const word_t full_word = bit_broadcast(value);
    for (size_t w = 0; w < num_full_words; ++w) {
        if (bitmap[w] != full_word) {
            return false;
        }
    }

    if (remaining_bits == 0) return true;
    const word_t last_word = bitmap[num_full_words];
    const word_t remainder_mask = ((word_t)1 << remaining_bits) - 1;
    return ((last_word ^ full_word) & remainder_mask) == 0;
}

/// Fill a bitmap with a uniform bit patten
///
/// Fill the bitmap `bitmap` of capacity `capacity` such that all entries with
/// an in-bounds index get the value `value`.
///
/// \param bitmap must be a valid bitmap of capacity `capacity`
/// \param capacity must be the bit storage capacity of `bitmap`
/// \param value is the bit value that will be set everywhere in the bitmap
static inline void bitmap_fill(word_t bitmap[],
                               size_t capacity,
                               bool value) {
    const word_t full_word = bit_broadcast(value);
    const size_t num_words = BITMAP_WORDS(capacity);
    for (size_t w = 0; w < num_words; ++w) bitmap[w] = full_word;
}

/// Find the first bit that has a certain value within a bitmap
///
/// \param bitmap must be a valid bitmap of capacity `capacity`
/// \param capacity must be the bit storage capacity of `bitmap`
/// \param value is the bit value that will be searched within the bitmap
/// \returns The position of the first bit that has the desired value, or
///          NO_BIT_POS to indicate absence of the desired value.
static inline bit_pos_t bitmap_find_first(word_t bitmap[],
                                          size_t capacity,
                                          bool value) {
    // Quickly skip over words where the value isn't present
    const word_t empty_word = bit_broadcast(!value);
    const size_t end_word = BITMAP_WORDS(capacity);
    size_t word;
    for (word = 0; word < end_word; ++word) {
        if (bitmap[word] != empty_word) break;
    }

    // If we skipped all words, we know the value is absent from the bitmap
    if (word == end_word) return NO_BIT_POS;

    // Otherwise, check the word on which we ended up
    const size_t num_full_words = capacity / BITS_PER_WORD;
    const size_t remaining_bits = capacity % BITS_PER_WORD;
    word_t target_word = bitmap[word];

    // Normalize the problem into that of looking for the first set bit
    if (!value) target_word = ~target_word;

    // Handle false positives related to padding bits being set to the target
    // value by clearing the padding bits (since we're now looking for set bits)
    if ((word == num_full_words) && (remaining_bits != 0)) {
        target_word &= ((word_t)1 << remaining_bits) - 1;
        if (target_word == 0) return NO_BIT_POS;
    }

    // At this point, we know there is a first set bit, so locate it
    const size_t offset = count_trailing_zeros(target_word);
    return (bit_pos_t) {
        .word = word,
        .offset = offset,
    };
}

/// Find the next bit that has a certain value within a bitmap
///
/// This is meant to be used when iterating over bits that have a certain value
/// within a certain bitmap. It receives a \ref bit_pos_t that was typically
/// returned by bitmap_find_first() or a previous call to bitmap_find_next(),
/// and returns the location of the next value of interest within the bitmap.
///
/// \param bitmap must be a valid bitmap of capacity `capacity`
/// \param capacity must be the bit storage capacity of `bitmap`
/// \param value is the bit value that will be searched within the bitmap
/// \param previous must be a valid bit position inside of `bitmap`. The search
///        will begin at the last bit within the bitmap.
/// \param wraparound indicates whether the search should wrap around to the
///        start of the bitmap if no occurence of `value` is found. If the
///        search does wrap around, then it will terminate unsuccessfully when
///        `previous` is reached again.
/// \returns The position of the first bit after `previous` + possible wrap
///          around that has the desired value, or NO_BIT_POS to indicate
///          absence of the desired value.
static inline bit_pos_t bitmap_find_next(word_t bitmap[],
                                         size_t capacity,
                                         bool value,
                                         bit_pos_t previous,
                                         bool wraparound) {
    // Check safety invariant in debug build
    assert(bit_pos_to_index(previous) < capacity);

    // If we were not looking at the last bit of the previous word, then
    // continue search within this word.
    const size_t num_full_words = capacity / BITS_PER_WORD;
    const size_t remaining_bits = capacity % BITS_PER_WORD;
    const bool previous_is_incomplete = previous.word == num_full_words;
    const size_t previous_bits = previous_is_incomplete ? remaining_bits : BITS_PER_WORD;
    if (previous.offset != (previous_bits - 1)) {
        // Extract previously processed word
        word_t previous_remainder = bitmap[previous.word];

        // Normalize into the problem of finding the first bit that is set
        if (!value) previous_remainder = ~previous_remainder;

        // If this was the last word of an incomplete bitmap, mask out its
        // padding bits so they do not result in search false positives
        if (previous_is_incomplete) previous_remainder &= ((word_t)1 << remaining_bits) - 1;

        // Eliminate bits which we have previously looked at
        const size_t dropped_bits = previous.offset + 1;
        previous_remainder >>= dropped_bits;

        // If there is a next set bit, return its position
        if (previous_remainder != 0) {
            const size_t extra_offset = count_trailing_zeros(previous_remainder);
            return (bit_pos_t) {
                .word = previous.word,
                .offset = dropped_bits + extra_offset,
            };
        }
    }

    // Look inside the remaining words from the bitmap, if any
    const size_t num_words = BITMAP_WORDS(capacity);
    if (previous.word != num_words - 1) {
        const size_t word_offset = previous.word + 1;
        bit_pos_t pos = bitmap_find_first(
            bitmap + word_offset,
            capacity - word_offset * BITS_PER_WORD,
            value
        );
        if (pos.word != SIZE_MAX) {
            pos.word += word_offset;
            return pos;
        }
    }

    // In absence of wraparound, we are done
    if (!wraparound) return NO_BIT_POS;

    // Otherwise, look into the bits before and including the previous search
    // result, if any
    return bitmap_find_first(bitmap,
                             bit_pos_to_index(previous) + 1,
                             value);
}

/// \}


/// \name Unit tests
/// \{

#ifdef UDIPE_BUILD_TESTS
    /// Unit tests for bitmaps
    ///
    /// This function runs all the unit tests for bitmaps. It must be called
    /// within the scope of with_logger().
    void bitmap_unit_tests();
#endif

/// \}
