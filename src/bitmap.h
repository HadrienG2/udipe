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
//!   exploit this granularity for optimization when it is present, along with
//!   other useful compile-time information

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
/// \param word must not be zero
static inline size_t count_trailing_zeros(word_t word) {
    return __builtin_ctzll(word);
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
/// \param capacity dictates how many bits the bitmap is capable of holding. It
///                 should be a compile-time constant, otherwise this macro will
///                 generate a Variable Lenght Array (VLA), which can fail in
///                 some circumstances and will have a negative effect on
///                 compiler optimizations in any case.
#define INLINE_BITMAP(name, capacity)  word_t name[BITMAP_WORDS(capacity)]

/// \}


/// \name Bit indexing
/// \{

/// Bit location within a bitmap
///
/// This designates either the `offset`-th bit within the `word`-th word of a
/// particular bitmap, or an invalid bit position (used for failed searches).
typedef struct bit_pos_s {
    size_t word; ///< Target word, or SIZE_MAX if invalid
    size_t offset; ///< Target bit within word, or SIZE_MAX if invalid
} bit_pos_t;

/// Invalid bit location within a bitmap
///
/// Used as the return value of failed bitmap searches.
#define NO_BIT_POS ((bit_pos_t) { .word = SIZE_MAX, .offset = SIZE_MAX })

/// Convert a bitmap location to a linear index
///
/// This is typically used when using the result of a bitmap search to inform
/// lookup into some associated array of resources.
static inline size_t bit_pos_to_index(bit_pos_t bit) {
    assert(bit.word != SIZE_MAX);
    assert(bit.offset < BITS_PER_WORD);
    return bit.word * BITS_PER_WORD + bit.offset;
}

/// Convert a linear index to a bitmap location
///
/// This is typically used when mapping an entry of an array of resource into
/// the associated entry within a bitmap.
static inline bit_pos_t index_to_bit_pos(size_t index) {
    assert(index != SIZE_MAX);
    return (bit_pos_t) {
        .word = index / BITS_PER_WORD,
        .offset = index % BITS_PER_WORD
    };
}

/// First bit location inside of a bitmap
///
/// This marks the start of a bitmap in commands that accept a bit location
/// range like bitmap_range_alleq(), as a left-inclusive bound, much like index
/// 0 designates the start of a C array.
///
/// See also bitmap_end().
#define BITMAP_START ((bit_pos_t) { .word = 0, .offset = 0 })

/// First invalid bit location after the end of a bitmap of length `capacity`
///
/// This marks the end of a bitmap in commands that accept a bit location range
/// like bitmap_range_alleq(), as a right-exclusive bound, much like typical C
/// loops over arrays are controlled by a `i < capacity` condition.
///
/// See also \ref BITMAP_START.
static inline bit_pos_t bitmap_end(size_t capacity) {
    assert(capacity != SIZE_MAX);
    return index_to_bit_pos(capacity);
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

/// Truth that a region of a bitmap contains only a certain value
///
/// Check if all entries within `bitmap` from bit `start` (included) to bit
/// `end` (excluded) are equal to `value`.
///
/// If you want to check if the entire bitmap is equal to `value`, use
/// `bitmap_range_alleq(bitmap, capacity, BITMAP_START, bitmap_end(capacity), value)`.
///
/// \param bitmap must be a valid bitmap of capacity `capacity`.
/// \param capacity must be the bit storage capacity of `bitmap`.
/// \param start designates the first bit to be checked, which must be in range
///              for this bitmap. Use \ref BITMAP_START if you want to cover
///              every bit from the start of the bitmap.
/// \param end designates the bit **past** the last bit to be checked. In other
///            words, if `start == end`, no bit will be checked. This bit
///            position can be in range or one bit past the end of the bitmap.
///            Use \link #bitmap_end `bitmap_end(capacity)` \endlink if you
///            want to cover every bit until the end of the bitmap.
/// \param value is the bit value that is expected.
///
/// \returns the truth that all bits in range `[start; end[` are set to `value`.
static inline bool bitmap_range_alleq(const word_t bitmap[],
                                      size_t capacity,
                                      bit_pos_t start,
                                      bit_pos_t end,
                                      bool value) {
    assert(bit_pos_to_index(start) < capacity
           || (start.word == end.word && start.offset == end.offset));
    assert(bit_pos_to_index(end) <= capacity);

    // For each word covered by the selected range...
    for (size_t word = start.word; word <= end.word; ++word) {
        // Ignore end word (which may not exist) if it has no active bit
        if ((word == end.word) && (end.offset == 0)) break;

        // Load the word of interest
        word_t target = bitmap[word];

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

/// Fill a region of a bitmap with a uniform bit pattern
///
/// Set all entries within `bitmap` from bit `start` (included) to bit
/// `end` (excluded) to `value`.
///
/// If you want to set the entire bitmap to `value`, use
/// `bitmap_range_set(bitmap, capacity, BITMAP_START, bitmap_end(capacity),
/// value)`.
///
/// \param bitmap must be a valid bitmap of capacity `capacity`
/// \param capacity must be the bit storage capacity of `bitmap`
/// \param start designates the first bit to be set, which must be in range
///              for this bitmap unless `start == end`. Use \ref BITMAP_START if
///              you want to cover every bit from the start of the bitmap.
/// \param end designates the bit **past** the last bit to be set. In other
///            words, if `start == end`, no bit will be set. This bit
///            position can be in range or one bit past the end of the bitmap.
///            Use \link #bitmap_end `bitmap_end(capacity)` \endlink if you
///            want to cover every bit until the end of the bitmap.
/// \param value is the bit value that will be set.
static inline void bitmap_range_set(word_t bitmap[],
                                    size_t capacity,
                                    bit_pos_t start,
                                    bit_pos_t end,
                                    bool value) {
    assert(bit_pos_to_index(start) < capacity
           || (start.word == end.word && start.offset == end.offset));
    assert(bit_pos_to_index(end) <= capacity);

    // Filling an entire bitmap word means assigning this value to it
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
            bitmap[word] = broadcast;
            continue;
        }

        // Ignore the end word (which may not exist) if it has no active bit
        if (word == end.word && end.offset == 0) break;

        // Load the current value of the word of interest
        const word_t current = bitmap[word];

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

        // Update bitmap with the masked mixture of the current and new value
        bitmap[word] = (broadcast & fill_mask) | (current & ~fill_mask);
    }
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
    word_t found_word = bitmap[word];

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
    const bool previous_incomplete = previous.word == num_full_words;
    const size_t previous_bits = previous_incomplete ? remaining_bits : BITS_PER_WORD;
    if (previous.offset != (previous_bits - 1)) {
        // Extract previous word
        word_t previous_word = bitmap[previous.word];

        // Normalize into the problem of looking for set bits
        if (!value) previous_word = ~previous_word;

        // If this was the last word of an incomplete bitmap, mask out its
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

    // Otherwise, look into bits before and including the `previous` bit
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
