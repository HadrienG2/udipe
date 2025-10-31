#ifdef UDIPE_BUILD_TESTS

    #include "bit_array.h"

    #include "error.h"

    #include <stdlib.h>


    /// Truth that a particular bit array length or index is an "interesting"
    /// test input.
    ///
    /// Experience shows that if an bitwise algorithms works on edges of size
    /// <= 2 from both sides of a machine word, it is likely to work everywhere.
    static inline bool is_interesting_input(size_t length_or_index) {
        const size_t trailing_bits = length_or_index % BITS_PER_WORD;
        return (trailing_bits <= 2 || (BITS_PER_WORD - trailing_bits) <= 2);
    }

    /// Sub-test of test_bit_array_with_hole() that exercises bit_array_get()
    static void check_bit_array_get(const word_t bit_array[],
                                    size_t length,
                                    size_t hole_start,
                                    size_t hole_end,
                                    bool main_value) {
        const bool hole_value = !main_value;
        for (size_t idx = 0; idx < length; ++idx) {
            tracef("- At index %zu.", idx);
            bool expected;
            if (idx < hole_start) {
                expected = main_value;
            } else if (idx < hole_end) {
                expected = hole_value;
            } else {
                expected = main_value;
            }
            ensure_eq(bit_array_get(bit_array,
                                    length,
                                    index_to_bit_pos(idx)),
                      expected);
        }
    }

    /// Sub-test of test_bit_array_with_hole() that exercises bit_array_count()
    static void check_bit_array_count(const word_t bit_array[],
                                      size_t length,
                                      size_t hole_start,
                                      size_t hole_end,
                                      bool main_value) {
        const bool hole_value = !main_value;
        const size_t num_holes = hole_end > hole_start
                               ? hole_end - hole_start
                               : 0;
        ensure_eq(bit_array_count(bit_array, length, main_value),
                  length - num_holes);
        ensure_eq(bit_array_count(bit_array, length, hole_value),
                  num_holes);
    }

    /// Sub-test of test_bit_array_with_hole() that exercises
    /// bit_array_range_alleq()
    static void check_bit_array_range_alleq(const word_t bit_array[],
                                            size_t length,
                                            size_t hole_start,
                                            size_t hole_end,
                                            bool main_value) {
        const bool hole_value = !main_value;
        #define all(start, end, value)  \
            bit_array_range_alleq(bit_array, length, (start), (end), (value))

        trace("Main region, before hole...");
        ensure(all(BIT_ARRAY_START,
                   index_to_bit_pos(hole_start),
                   main_value));
        ensure_eq(all(BIT_ARRAY_START,
                      index_to_bit_pos(hole_start),
                      hole_value),
                  hole_start == 0);

        trace("Hole region...");
        ensure(all(index_to_bit_pos(hole_start),
                   index_to_bit_pos(hole_end),
                   hole_value));
        ensure_eq(all(index_to_bit_pos(hole_start),
                      index_to_bit_pos(hole_end),
                      main_value),
                  hole_start >= hole_end);

        trace("Main region, after hole...");
        ensure(all(index_to_bit_pos(hole_end),
                   bit_array_end(length),
                   main_value));
        ensure_eq(all(index_to_bit_pos(hole_end),
                      bit_array_end(length),
                      hole_value),
                  hole_end == length);

        trace("Shifting hole_start by -1...");
        if (hole_start > 0) {
            ensure(all(BIT_ARRAY_START,
                       index_to_bit_pos(hole_start - 1),
                       main_value));
            ensure_eq(all(BIT_ARRAY_START,
                          index_to_bit_pos(hole_start - 1),
                          hole_value),
                      hole_start == 1);
            ensure_eq(all(index_to_bit_pos(hole_start - 1),
                          index_to_bit_pos(hole_end),
                          hole_value),
                      hole_start - 1 >= hole_end);
            ensure_eq(all(index_to_bit_pos(hole_start - 1),
                          index_to_bit_pos(hole_end),
                          main_value),
                      hole_start >= hole_end);
        }

        trace("Shifting hole_start by +1...");
        if (hole_start < length - 1) {
            ensure_eq(all(BIT_ARRAY_START,
                          index_to_bit_pos(hole_start + 1),
                          main_value),
                      hole_start >= hole_end);
            ensure_eq(all(BIT_ARRAY_START,
                          index_to_bit_pos(hole_start + 1),
                          hole_value),
                      hole_start == 0 && hole_end >= 1);
            ensure(all(index_to_bit_pos(hole_start + 1),
                       index_to_bit_pos(hole_end),
                       hole_value));
            ensure_eq(all(index_to_bit_pos(hole_start + 1),
                          index_to_bit_pos(hole_end),
                          main_value),
                      hole_start + 1 >= hole_end);
        }

        trace("Shifting hole_end by -1...");
        if (hole_end > 0) {
            ensure(all(index_to_bit_pos(hole_start),
                       index_to_bit_pos(hole_end - 1),
                       hole_value));
            ensure_eq(all(index_to_bit_pos(hole_start),
                          index_to_bit_pos(hole_end - 1),
                          main_value),
                      hole_start >= hole_end - 1);
            ensure_eq(all(index_to_bit_pos(hole_end - 1),
                          bit_array_end(length),
                          main_value),
                      hole_start >= hole_end);
            ensure_eq(all(index_to_bit_pos(hole_end - 1),
                          bit_array_end(length),
                          hole_value),
                      hole_end == length);
        }

        trace("Shifting hole_end by +1...");
        if (hole_end < length - 1) {
            ensure_eq(all(index_to_bit_pos(hole_start),
                          index_to_bit_pos(hole_end + 1),
                          hole_value),
                      hole_start >= hole_end + 1);
            ensure_eq(all(index_to_bit_pos(hole_start),
                          index_to_bit_pos(hole_end + 1),
                          main_value),
                      hole_start >= hole_end);
            ensure(all(index_to_bit_pos(hole_end + 1),
                       bit_array_end(length),
                       main_value));
            ensure_eq(all(index_to_bit_pos(hole_end + 1),
                          bit_array_end(length),
                          hole_value),
                      hole_end + 1 == length);
        }

        #undef all
    }

    /// Sub-test of test_bit_array_with_hole() that exercises bit_array_find_first()
    static void check_bit_array_find_first(const word_t bit_array[],
                                           size_t length,
                                           size_t hole_start,
                                           size_t hole_end,
                                           bool main_value) {
        const bool hole_value = !main_value;
        bit_pos_t result, expected;

        trace("Finding the first bit that's set to the main value...");
        result = bit_array_find_first(bit_array, length, main_value);
        if (hole_start > 0) {
            expected = BIT_ARRAY_START;
        } else if (hole_end < length) {
            expected = index_to_bit_pos(hole_end);
        } else {
            expected = NO_BIT_POS;
        }
        ensure_eq(result.word, expected.word);
        ensure_eq(result.offset, expected.offset);

        trace("Finding the first bit that's set to the hole value...");
        result = bit_array_find_first(bit_array, length, hole_value);
        if (hole_end > hole_start) {
            expected = index_to_bit_pos(hole_start);
        } else {
            expected = NO_BIT_POS;
        }
        ensure_eq(result.word, expected.word);
        ensure_eq(result.offset, expected.offset);
    }

    /// Sub-test of check_bit_array_find_next() that looks for the main value
    static void check_bit_array_find_next_main(const word_t bit_array[],
                                               size_t length,
                                               size_t hole_start,
                                               size_t hole_end,
                                               bool main_value) {
        bit_pos_t result, expected;

        trace("Without wraparound...");
        for (size_t idx = 0; idx < length; ++idx) {
            tracef("- At index %zu.", idx);
            const bit_pos_t start = index_to_bit_pos(idx);
            result = bit_array_find_next(bit_array,
                                         length,
                                         start,
                                         false,
                                         main_value);
            if (hole_start > 0 && idx < hole_start - 1) {
                expected = index_to_bit_pos(idx + 1);
            } else if (idx < hole_end) {
                if (hole_end < length) {
                    expected = index_to_bit_pos(hole_end);
                } else {
                    expected = NO_BIT_POS;
                }
            } else if (idx < length - 1) {
                expected = index_to_bit_pos(idx + 1);
            } else {
                expected = NO_BIT_POS;
            }
            ensure_eq(result.word, expected.word);
            ensure_eq(result.offset, expected.offset);
        }

        trace("With wraparound...");
        for (size_t idx = 0; idx < length; ++idx) {
            tracef("- At index %zu.", idx);
            const bit_pos_t start = index_to_bit_pos(idx);
            result = bit_array_find_next(bit_array,
                                         length,
                                         start,
                                         true,
                                         main_value);
            if (hole_start > 0 && idx < hole_start - 1) {
                expected = index_to_bit_pos(idx + 1);
            } else if (idx < hole_end) {
                if (hole_end < length) {
                    expected = index_to_bit_pos(hole_end);
                } else {
                    expected = bit_array_find_first(bit_array,
                                                    length,
                                                    main_value);
                }
            } else if (idx < length - 1) {
                expected = index_to_bit_pos(idx + 1);
            } else {
                expected = bit_array_find_first(bit_array, length, main_value);
            }
            ensure_eq(result.word, expected.word);
            ensure_eq(result.offset, expected.offset);
        }
    }

    /// Sub-test of check_bit_array_find_next() that looks for the hole value
    static void check_bit_array_find_next_hole(const word_t bit_array[],
                                               size_t length,
                                               size_t hole_start,
                                               size_t hole_end,
                                               bool main_value) {
        const bool hole_value = !main_value;
        bit_pos_t result, expected;

        trace("Without wraparound...");
        for (size_t idx = 0; idx < length; ++idx) {
            tracef("- At index %zu.", idx);
            const bit_pos_t start = index_to_bit_pos(idx);
            result = bit_array_find_next(bit_array,
                                         length,
                                         start,
                                         false,
                                         hole_value);
            if (idx < hole_start) {
                if (hole_end > hole_start) {
                    expected = index_to_bit_pos(hole_start);
                } else {
                    expected = NO_BIT_POS;
                }
            } else if (idx < hole_end - 1 && hole_start < hole_end) {
                expected = index_to_bit_pos(idx + 1);
            } else {
                expected = NO_BIT_POS;
            }
            ensure_eq(result.word, expected.word);
            ensure_eq(result.offset, expected.offset);
        }

        trace("With wraparound...");
        for (size_t idx = 0; idx < length; ++idx) {
            tracef("- At index %zu.", idx);
            const bit_pos_t start = index_to_bit_pos(idx);
            result = bit_array_find_next(bit_array,
                                         length,
                                         start,
                                         true,
                                         hole_value);
            if (idx < hole_start) {
                if (hole_end > hole_start) {
                    expected = index_to_bit_pos(hole_start);
                } else {
                    expected = bit_array_find_first(bit_array,
                                                    length,
                                                    hole_value);
                }
            } else if (idx < hole_end - 1 && hole_start < hole_end) {
                expected = index_to_bit_pos(idx + 1);
            } else {
                expected = bit_array_find_first(bit_array, length, hole_value);
            }
            ensure_eq(result.word, expected.word);
            ensure_eq(result.offset, expected.offset);
        }
    }

    /// Sub-test of test_bit_array_with_hole() that exercises
    /// bit_array_find_next()
    static void check_bit_array_find_next(const word_t bit_array[],
                                          size_t length,
                                          size_t hole_start,
                                          size_t hole_end,
                                          bool main_value) {
        check_bit_array_find_next_main(bit_array,
                                       length,
                                       hole_start,
                                       hole_end,
                                       main_value);
        check_bit_array_find_next_hole(bit_array,
                                       length,
                                       hole_start,
                                       hole_end,
                                       main_value);
    }

    /// Sub-test of test_bit_array_with_hole() that exercises bit_array_set()
    static void check_bit_array_set(word_t bit_array[],
                                    size_t length,
                                    size_t hole_start,
                                    size_t hole_end,
                                    bool main_value) {
        const bool hole_value = !main_value;
        const size_t hole_idx = rand() % length;
        tracef("Setting a random bit at index %zu to the hole value...", hole_idx);
        bit_array_set(bit_array,
                      length,
                      index_to_bit_pos(hole_idx),
                      hole_value);

        trace("...then checking the resulting bit pattern");
        for (size_t idx = 0; idx < length; ++idx) {
            tracef("- At index %zu.", idx);
            bool expected;
            if (idx == hole_idx) {
                expected = hole_value;
            } else if (idx < hole_start) {
                expected = main_value;
            } else if (idx < hole_end) {
                expected = hole_value;
            } else {
                expected = main_value;
            }
            ensure_eq(bit_array_get(bit_array,
                                    length,
                                    index_to_bit_pos(idx)),
                      expected);
        }
    }

    /// Sub-test of test_bit_array() that exercises bit arrays with a uniform
    /// value on top of which a "hole" has been "punched" by writing the
    /// opposite value in linear index range [hole_start; hole_end[.
    static void test_bit_array_with_hole(word_t bit_array[],
                                         size_t length,
                                         size_t hole_start,
                                         size_t hole_end,
                                         bool main_value) {
        const bool hole_value = !main_value;
        tracef("Using main value %u and hole value %u.", main_value, hole_value);

        trace("Filling the bit array with the desired pattern...");
        bit_array_range_set(bit_array,
                            length,
                            BIT_ARRAY_START,
                            bit_array_end(length),
                            main_value);
        bit_array_range_set(bit_array,
                            length,
                            index_to_bit_pos(hole_start),
                            index_to_bit_pos(hole_end),
                            hole_value);

        trace("Testing bit_array_get()...");
        check_bit_array_get(bit_array,
                            length,
                            hole_start,
                            hole_end,
                            main_value);

        trace("Testing bit_array_count()...");
        check_bit_array_count(bit_array,
                              length,
                              hole_start,
                              hole_end,
                              main_value);

        trace("Testing bit_array_range_alleq()...");
        check_bit_array_range_alleq(bit_array,
                                    length,
                                    hole_start,
                                    hole_end,
                                    main_value);

        trace("Testing bit_array_find_first()...");
        check_bit_array_find_first(bit_array,
                                   length,
                                   hole_start,
                                   hole_end,
                                   main_value);

        trace("Testing bit_array_find_next()...");
        check_bit_array_find_next(bit_array,
                                  length,
                                  hole_start,
                                  hole_end,
                                  main_value);

        trace("Testing bit_array_set()...");
        check_bit_array_set(bit_array,
                            length,
                            hole_start,
                            hole_end,
                            main_value);
    }

    /// Sub-test of bit_array_unit_tests() that runs with a certain array length
    static void test_bit_array(word_t bit_array[], size_t length) {
        for (size_t hole_start = 0; hole_start < length; ++hole_start) {
            if (!is_interesting_input(hole_start)) continue;
            for (size_t hole_end = 0; hole_end <= length; ++hole_end) {
                if (!is_interesting_input(hole_end)) continue;
                tracef("Testing bit arrays with a \"hole\" at index range [%zu; %zu[...",
                       hole_start, hole_end);
                test_bit_array_with_hole(bit_array, length, hole_start, hole_end, false);
                test_bit_array_with_hole(bit_array, length, hole_start, hole_end, true);
            }
        }
    }

    void bit_array_unit_tests() {
        info("Running bit array unit tests...");
        for (size_t length = 0; length <= 3 * BITS_PER_WORD; ++length) {
            if (is_interesting_input(length)) {
                debugf("Testing with a bit array of length %zu.", length);
                INLINE_BIT_ARRAY(bit_array, length);
                test_bit_array(bit_array, length);
            }
        }
    }

#endif  // UDIPE_BUILD_TESTS
