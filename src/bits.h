
#pragma once

//! \file
//! \brief Performance tricks that exploit the binary representation of numbers
//!
//! This code module provides utilities for performing more efficient integer
//! and boolean computations in situations where the compiler optimizer cannot
//! figure out the bit trick on its own (typically because it's missing some
//! information at compile time or it is not allowed to perform the optimization
//! per C language rules).

#include <udipe/pointer.h>

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/// Divide `num` and `denom`, rounding upwards
///
/// \param num must be a side-effects-free integer expression
/// \param denom must be a side-effects-free integer expresion that does not
///              evaluate to zero
#define DIV_CEIL(num, denom) ((num / denom) + ((num % denom) != 0))


/// \name Machine words and SWAR primitives
/// \{

/// Largest unsigned machine word
///
/// This is the largest bag of bits that you can manipulate using Simd Within A
/// Register (SWAR) algorithms based on scalar machine operations.
///
/// By processing arrays of \ref word_t (see bit_array.h), you can additionally
/// get integer SIMD and superscalar execution. Combined with `-march=native`,
/// these three techniques can take you to peak single-core machine efficiency
/// for boolean data manipulation, once the bit array size gets large enough.
typedef size_t word_t;

/// Number of bits within a \ref word_t
///
/// This is the maximal degree of parallelism that can be achieved by
/// manipulating bits in parallel using only Simd Within A Register (SWAR)
/// algorithms (excluding vectorization, superscalar execution and multicore).
///
/// Bit array operations perform best when the length of a bit array is known at
/// compile time to be a multiple of this quantity.
#define BITS_PER_WORD (sizeof(word_t) * 8)

/// Maximum value of \ref word_t
///
/// From a Simd Within A Register (SWAR) perspective, this is a \ref word_t
/// where all bits are set.
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


/// \name Efficient manipulation of dynamic powers-of-two
/// \{

/// Dynamic power-of-two encoding that enables more compiler optimizations
///
/// Many useful low-level integer constants are powers of two, as this greatly
/// simplifies some integer computations like division and multiplication. For
/// constants that are known at compile time, like the size of machine words,
/// the compiler takes advantage of this fact automatically. But for constants
/// that are only known at runtime, like the page size used by the operating
/// system, the compiler doesn't know that the number that comes out of the
/// magical OS black box will always be a power of two, which results in a loss
/// of integer performance.
///
/// We can get this integer performance back by encoding powers of two as their
/// base-2 logarithm (number of trailing zeros after the only bit set to 1), and
/// decoding them using an inlined function that goes back to a power of two
/// using a `1 << n` bitshift operation. The bitshift is not a performance
/// problem as it is very cheap to begin with, and the compiler optimizer will
/// often be able to extract it out of hot loops.
typedef struct pow2_s {
    /// Base-2 logarithm of a power of two smaller than `1 << 31`
    ///
    /// We do not need larger powers of two at this point in time, and by
    /// capping them to this range we ensure that even on 32-bit machines the `1
    /// << log2` bitshift is guaranteed not to go out of range.
    ///
    /// Today out of range bitshifts are UB in C so the compiler doesn't care,
    /// but there is an ongoing effort to reduce the amount of UB in C at the
    /// time of writing, which means that tomorrow's C compilers may benefit
    /// from knowing that a particular bitshift will always be in range.
    ///
    /// The way we tell the compiler that the power of two is smaller than `1 <<
    /// 31` is by encoding the base-2 logarithm as a 5-bit integer, which by
    /// construction cannot be higher than or equal to `1 << 5` aka 32.
    unsigned log2: 5;
} pow2_t;

/// Encode a power of two into a format that lets the compile know it is one
///
/// You do not need this trick for compile-time constants, this is only needed
/// for run-time constants where you know that they are a power-of-two but the
/// compiler doesn't, like the OS page size.
///
/// \param power_of_two must be a power of two
/// \returns an encoding of the power of two that enabled optimizations
static inline pow2_t pow2_encode(uint32_t power_of_two) {
    assert(population_count(power_of_two) == 1);
    return (pow2_t){
        .log2 = count_trailing_zeros(power_of_two)
    };
}

/// Decode a power of two in such a way that the compiler know it is one
///
/// For this compiler optimization to work, you need to inline not just this
/// function, but also every other utility function that stands in the code path
/// between the point where this function is called and the point where the
/// compiler optimizer should know that the number is a power of two (e.g. where
/// some integer is divided by this number).
///
/// \param encoded must be a log2-based encoding of a power of two, which you
///                can produce using pow2_encode()
/// \returns the original power of two
static inline uint32_t pow2_decode(pow2_t encoded) {
    return 1 << encoded.log2;
}

/// \}


/// \name Chainable integer operations
/// \{

/// Compute the sum of two numbers, propagating carries in the process
///
/// \param carry is the carry flag from the previous operation, or
///              `false` is there was no previous operation.
/// \param augend is the first term of the addition.
/// \param addend is the second term of the addition.
/// \param out is the location where the result will be stored.
///
/// \returns the carry flag to be used for the next operation (if any)
UDIPE_NON_NULL_ARGS
static inline
bool add_with_carry_u64(bool carry,
                        uint64_t augend,
                        uint64_t addend,
                        uint64_t* out) {
    const uint64_t new_addend = addend + carry;
    const bool inc_carry = (new_addend < addend);
    const uint64_t sum = augend + new_addend;
    const bool add_carry = (sum < augend);
    *out = sum;
    return inc_carry || add_carry;
}

/// Compute the difference of two numbers, propagating carries in the process
///
/// \param carry is the carry flag from the previous operation, or
///              `false` is there was no previous operation.
/// \param x is the first addend.
/// \param y is the second addend.
/// \param out is the location where the result will be stored.
///
/// \returns the carry flag to be used for the next operation (if any)
UDIPE_NON_NULL_ARGS
static inline
bool subtract_with_carry_u64(bool carry,
                             uint64_t minuend,
                             uint64_t subtrahend,
                             uint64_t* out) {
    const uint64_t new_subtrahend = subtrahend + carry;
    const bool inc_carry = (new_subtrahend < subtrahend);
    const uint64_t difference = minuend - new_subtrahend;
    const bool sub_carry = (difference > minuend);
    *out = difference;
    return inc_carry || sub_carry;
}

/// \}


// TODO: Unit tests
