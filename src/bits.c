#include "bits.h"

#include <udipe/pointer.h>

#include "error.h"
#include "log.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


UDIPE_NON_NULL_ARGS
void generate_entropy(uint64_t output[], size_t length) {
    ensure_eq(count_trailing_zeros(RAND_MAX), (word_t)0);
    const size_t bits_per_rand = population_count(RAND_MAX);
    memset(output, 0, length * sizeof(uint64_t));
    size_t bits_so_far = 0;
    while (bits_so_far < length * BITS_PER_ENTROPY_WORD) {
        const size_t word = bits_so_far / BITS_PER_ENTROPY_WORD;
        const size_t offset = bits_so_far % BITS_PER_ENTROPY_WORD;
        tracef("- At global bit #%zu (word #%zu, local bit #%zu)",
               bits_so_far, word, offset);
        const uint64_t entropy = rand();
        output[word] |= entropy << offset;
        const size_t local_bits = BITS_PER_ENTROPY_WORD - offset;
        if (local_bits < bits_per_rand && word + 1 < length) {
            const size_t remaining_bits = bits_per_rand - local_bits;
            output[word + 1] = entropy >> local_bits;
        }
        bits_so_far += bits_per_rand;
    }
}

UDIPE_NON_NULL_ARGS
void entropy_to_bits(size_t bits_per_output,
                     uint64_t outputs[],
                     size_t num_outputs,
                     size_t* consumed_input_bits,
                     const uint64_t inputs[],
                     size_t num_inputs) {
    assert(bits_per_output <= BITS_PER_ENTROPY_WORD);
    size_t next_input_bit = *consumed_input_bits;
    const size_t remaining_input_bits =
        num_inputs * BITS_PER_ENTROPY_WORD - next_input_bit;
    assert(remaining_input_bits >= num_outputs * bits_per_output);

    for (size_t output_word = 0; output_word < num_outputs; ++output_word) {
        const size_t input_word = next_input_bit / BITS_PER_ENTROPY_WORD;
        assert(input_word < num_inputs);
        const size_t input_offset = next_input_bit % BITS_PER_ENTROPY_WORD;

        uint64_t result = inputs[input_word] >> input_offset;
        const size_t bits_so_far = BITS_PER_ENTROPY_WORD - input_offset;
        if (bits_so_far < bits_per_output) {
            assert(input_word + 1 < num_inputs);
            result |= inputs[input_word+1] << bits_so_far;
        }
        if (bits_per_output < BITS_PER_ENTROPY_WORD) {
            result &= ((uint64_t)1 << bits_per_output) - 1;
        }
        outputs[output_word] = result;

        next_input_bit += bits_per_output;
    }

    *consumed_input_bits = next_input_bit;
}
