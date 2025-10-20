#pragma once

//! \file
//! \brief Bitmap
//!
//! `libudipe` internally uses bitmaps in various circumstances.

#include <assert.h>
#include <stddef.h>


/// Divide `num` and `denom`, rounding upwards
///
/// \param num must be an integer value
/// \param denom must be an integer value other than zero
#define div_ceil(num, denom)  \
    ({  \
        typeof(num) udipe_num = (num);  \
        typeof(denom) udipe_denom = (denom);  \
        (udipe_num / udipe_denom) + ((udipe_num % udipe_denom) != 0);  \
    })


/// Unsigned machine word type used for bitmap storage
///
/// A bitmap is simply an array of such words.
typedef word_t = size_t;


/// Maximum value of \ref word_t
///
/// From a bitmap perspective, this is a \ref word_t where all bits are set
#define WORD_MAX SIZE_MAX


/// Number of bits within a \ref word_t
///
/// This links the amount of storage words to the amount of storage bits
#define BITS_PER_WORD (sizeof(word_t) * 8)


/// Number of \ref word_t inside a bitmap of specified capacity
///
/// \param capacity must be a positive integer expression
#define bitmap_len(capacity)  div_ceil((capacity), BITS_PER_WORD)


/// Declare a bitmap whose size is known at compile time
///
/// This macro generates a declaration for a bitmap variable called `name` which
/// is capable of holding `capacity` bits, which is allocated inline (i.e. not
/// on a separate heap allocation).
///
/// You can use this macro for the purpose of declaring either local bitmap
/// variables or struct bitmap members.
///
/// \param name must be a valid variable identifier, that is not already used
///             in the scope where this variable is called.
/// \param capacity should be a compile-time constant, and dictates how many
///                 bits the bitmap is capable of holding. Attempting to access
///                 a higher-indexed bit will result in undefined behavior.
#define INLINE_BITMAP(name, capacity)  word_t name[bitmap_len(capacity)]



/// Broadcast a boolean value to all lanes of a bitmap word
///
/// Returns a \ref word_t where all bits are set to the given `value`.
static inline word_t broadcast_bit(bool value) {
    return value ? WORD_MAX : 0;
}


/// Fill a bitmap with a uniform bit patten
///
/// Fill the bitmap `bitmap` of capacity `capacity` such that all entries with
/// an in-bounds index get the value `value`.
static inline void bitmap_fill(word_t bitmap[],
                               size_t capacity,
                               bool value) {
    const word_t full_word = broadcast_bit(value);
    const size_t num_words = bitmap_len(capacity);
    for (size_t w = 0; w < num_words; ++w) bitmap[w] = full_word;
}


/// Truth that a bitmap is filled with a uniform bit pattern
///
/// Check if all in-bounds entries within the bitmap `bitmap` of capacity
/// `capacity` have value `value`.
static inline bool bitmap_all(const word_t bitmap[],
                              size_t capacity,
                              bool value) {
    const size_t num_full_words = capacity / BITS_PER_WORD;
    const size_t remaining_bits = capacity % BITS_PER_WORD;

    const word_t full_word = broadcast_bit(value);
    for (size_t w = 0; w < num_full_words; ++i) {
        if (bitmap[w] != full_word) return false;
    }

    if (remaining_bits == 0) return true;
    const word_t last_word = bitmap[num_full_words];
    const word_t remainder_mask = (1 << remaining_bits) - 1;
    return ((last_word ^ full_word) & remainder_mask) == 0;
}


/// Location within a bitmap
///
/// This designates the `offset`-th bit within the `word`-th word of a
/// particular bitmap.
typedef struct bit_s {
    size_t word;
    size_t offset;
} bit_t;


/// Convert a bitmap location to a linear index
///
/// This is used when converting the result of a bitmap query into some
/// flat index within e.g. an array.
static inline size_t bit_to_index(bit_t bit) {
    return bit.word * BITS_PER_WORD + bit.offset;
}


/// Convert a linear index to a bitmap location
///
/// This is used when converting some flat e.g. index into a coordinate within
/// the underlying bitmap.
static inline bit_t index_to_bit(size_t index) {
    return {
        .word = index / BITS_PER_WORD,
        .offset = index % BITS_PER_WORD
    };
}


/// Get the value of the Nth bit of a bitmap
///
/// This tells whether a particular bit of a bitmap is set.
static inline bool bitmap_get(word_t bitmap[],
                              size_t capacity,
                              bit_t bit) {
    assert(bit_to_index(bit) < capacity);
    return (bitmap[bit.word] & (1 << bit.offset)) != 0;
}


/// Set the value of the Nth bit of a bitmap
///
/// This lets you adjust the value of a particular bit of a bitmap.
static inline void bitmap_set(word_t bitmap[],
                              size_t capacity,
                              bit_t bit,
                              bool value) {
    assert(bit_to_index(bit) < capacity);
    if (value) {
        bitmap[bit.word] |= (1 << bit.offset);
    } else {
        bitmap[bit.word] &= ~(1 << bit.offset);
    }
}


/// Find the first bit that has a certain value within a bitmap
//
// TODO: doc and impl
bit_t bitmap_find_first(word_t bitmap[],
                        size_t capacity,
                        bool value);


/// Find the next bit that has a certain value within a bitmap, wrapping around
/// when the end of the bitmap is reached but excluding the previous position
//
// TODO: doc and impl
bit_t bitmap_find_next(word_t bitmap[],
                       size_t capacity,
                       bool value,
                       bit_t previous);
