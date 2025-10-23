#ifdef UDIPE_BUILD_TESTS

    #include "bitmap.h"

    #include "error.h"


    /// Truth that a particular bitmap capacity or index is an "interesting"
    /// test input.
    ///
    /// Experience shows that if an bitwise algorithms works on edges of size
    /// <= 2 from both sides of a machine word, it is likely to work everywhere.
    static inline bool is_interesting(size_t capacity_or_index) {
        const size_t trailing_bits = capacity_or_index % BITS_PER_WORD;
        return (trailing_bits <= 2 || (BITS_PER_WORD - trailing_bits) <= 2);
    }

    /// Sub-test of test_bitmap_with_hole() that exercises bitmap_get()
    static void check_bitmap_get(word_t bitmap[],
                                 size_t capacity,
                                 size_t hole_start,
                                 size_t hole_end,
                                 bool main_value) {
        const bool hole_value = !main_value;
        for (size_t idx = 0; idx < capacity; ++idx) {
            tracef("- At index %zu.", idx);
            bool expected;
            if (idx < hole_start) {
                expected = main_value;
            } else if (idx < hole_end) {
                expected = hole_value;
            } else {
                expected = main_value;
            }
            ensure_eq(bitmap_get(bitmap,
                                 capacity,
                                 index_to_bit_pos(idx)),
                      expected);
        }
    }

    /// Sub-test of test_bitmap_with_hole() that exercises bitmap_count()
    static void check_bitmap_count(word_t bitmap[],
                                   size_t capacity,
                                   size_t hole_start,
                                   size_t hole_end,
                                   bool main_value) {
        const bool hole_value = !main_value;
        const size_t num_holes = hole_end > hole_start
                               ? hole_end - hole_start
                               : 0;
        ensure_eq(bitmap_count(bitmap, capacity, main_value),
                  capacity - num_holes);
        ensure_eq(bitmap_count(bitmap, capacity, hole_value),
                  num_holes);
    }

    /// Sub-test of test_bitmap_with_hole() that exercises bitmap_range_alleq()
    static void check_bitmap_range_alleq(word_t bitmap[],
                                         size_t capacity,
                                         size_t hole_start,
                                         size_t hole_end,
                                         bool main_value) {
        const bool hole_value = !main_value;
        #define all(start, end, value)  \
            bitmap_range_alleq(bitmap, capacity, (start), (end), (value))

        trace("Main region, before hole...");
        ensure(all(BITMAP_START,
                   index_to_bit_pos(hole_start),
                   main_value));
        ensure_eq(all(BITMAP_START,
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
                   bitmap_end(capacity),
                   main_value));
        ensure_eq(all(index_to_bit_pos(hole_end),
                      bitmap_end(capacity),
                      hole_value),
                  hole_end == capacity);

        trace("Shifting hole_start by -1...");
        if (hole_start > 0) {
            ensure(all(BITMAP_START,
                       index_to_bit_pos(hole_start - 1),
                       main_value));
            ensure_eq(all(BITMAP_START,
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
        if (hole_start < capacity - 1) {
            ensure_eq(all(BITMAP_START,
                          index_to_bit_pos(hole_start + 1),
                          main_value),
                      hole_start >= hole_end);
            ensure_eq(all(BITMAP_START,
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
                          bitmap_end(capacity),
                          main_value),
                      hole_start >= hole_end);
            ensure_eq(all(index_to_bit_pos(hole_end - 1),
                          bitmap_end(capacity),
                          hole_value),
                      hole_end == capacity);
        }

        trace("Shifting hole_end by +1...");
        if (hole_end < capacity - 1) {
            ensure_eq(all(index_to_bit_pos(hole_start),
                          index_to_bit_pos(hole_end + 1),
                          hole_value),
                      hole_start >= hole_end + 1);
            ensure_eq(all(index_to_bit_pos(hole_start),
                          index_to_bit_pos(hole_end + 1),
                          main_value),
                      hole_start >= hole_end);
            ensure(all(index_to_bit_pos(hole_end + 1),
                       bitmap_end(capacity),
                       main_value));
            ensure_eq(all(index_to_bit_pos(hole_end + 1),
                          bitmap_end(capacity),
                          hole_value),
                      hole_end + 1 == capacity);
        }

        #undef all
    }

    /// Sub-test of test_bitmap_with_hole() that exercises bitmap_find_first()
    static void check_bitmap_find_first(word_t bitmap[],
                                        size_t capacity,
                                        size_t hole_start,
                                        size_t hole_end,
                                        bool main_value) {
        const bool hole_value = !main_value;
        bit_pos_t result, expected;

        trace("Finding the first bit that's set to the main value...");
        result = bitmap_find_first(bitmap, capacity, main_value);
        if (hole_start > 0) {
            expected = BITMAP_START;
        } else if (hole_end < capacity) {
            expected = index_to_bit_pos(hole_end);
        } else {
            expected = NO_BIT_POS;
        }
        ensure_eq(result.word, expected.word);
        ensure_eq(result.offset, expected.offset);

        trace("Finding the first bit that's set to the hole value...");
        result = bitmap_find_first(bitmap, capacity, hole_value);
        if (hole_end > hole_start) {
            expected = index_to_bit_pos(hole_start);
        } else {
            expected = NO_BIT_POS;
        }
        ensure_eq(result.word, expected.word);
        ensure_eq(result.offset, expected.offset);
    }

    /// Sub-test of test_bitmap_with_hole() that exercises bitmap_find_next()
    static void check_bitmap_find_next(word_t bitmap[],
                                       size_t capacity,
                                       size_t hole_start,
                                       size_t hole_end,
                                       bool main_value) {
        const bool hole_value = !main_value;
        bit_pos_t result, expected;

        trace("Main value, without wraparound...");
        for (size_t idx = 0; idx < capacity; ++idx) {
            tracef("- At index %zu.", idx);
            const bit_pos_t start = index_to_bit_pos(idx);
            result = bitmap_find_next(bitmap,
                                   capacity,
                                   start,
                                   false,
                                   main_value);
            if (hole_start > 0 && idx < hole_start - 1) {
                expected = index_to_bit_pos(idx + 1);
            } else if (idx < hole_end) {
                if (hole_end < capacity) {
                    expected = index_to_bit_pos(hole_end);
                } else {
                    expected = NO_BIT_POS;
                }
            } else if (idx < capacity - 1) {
                expected = index_to_bit_pos(idx + 1);
            } else {
                expected = NO_BIT_POS;
            }
            ensure_eq(result.word, expected.word);
            ensure_eq(result.offset, expected.offset);
        }

        trace("Main value, with wraparound...");
        for (size_t idx = 0; idx < capacity; ++idx) {
            tracef("- At index %zu.", idx);
            const bit_pos_t start = index_to_bit_pos(idx);
            result = bitmap_find_next(bitmap,
                                   capacity,
                                   start,
                                   true,
                                   main_value);
            if (hole_start > 0 && idx < hole_start - 1) {
                expected = index_to_bit_pos(idx + 1);
            } else if (idx < hole_end) {
                if (hole_end < capacity) {
                    expected = index_to_bit_pos(hole_end);
                } else {
                    expected = bitmap_find_first(bitmap, capacity, main_value);
                }
            } else if (idx < capacity - 1) {
                expected = index_to_bit_pos(idx + 1);
            } else {
                expected = bitmap_find_first(bitmap, capacity, main_value);
            }
            ensure_eq(result.word, expected.word);
            ensure_eq(result.offset, expected.offset);
        }

        trace("Hole value, without wraparound...");
        for (size_t idx = 0; idx < capacity; ++idx) {
            tracef("- At index %zu.", idx);
            const bit_pos_t start = index_to_bit_pos(idx);
            result = bitmap_find_next(bitmap,
                                   capacity,
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

        trace("Hole value, with wraparound...");
        for (size_t idx = 0; idx < capacity; ++idx) {
            tracef("- At index %zu.", idx);
            const bit_pos_t start = index_to_bit_pos(idx);
            result = bitmap_find_next(bitmap,
                                   capacity,
                                   start,
                                   true,
                                   hole_value);
            if (idx < hole_start) {
                if (hole_end > hole_start) {
                    expected = index_to_bit_pos(hole_start);
                } else {
                    expected = bitmap_find_first(bitmap, capacity, hole_value);
                }
            } else if (idx < hole_end - 1 && hole_start < hole_end) {
                expected = index_to_bit_pos(idx + 1);
            } else {
                expected = bitmap_find_first(bitmap, capacity, hole_value);
            }
            ensure_eq(result.word, expected.word);
            ensure_eq(result.offset, expected.offset);
        }
    }

    /// Sub-test of test_bitmap_with_hole() that exercises bitmap_set()
    static void check_bitmap_set(word_t bitmap[],
                                 size_t capacity,
                                 size_t hole_start,
                                 size_t hole_end,
                                 bool main_value) {
        const bool hole_value = !main_value;
        const size_t hole_idx = rand() % capacity;
        tracef("Setting a random bit at index %zu to the hole value...", hole_idx);
        bitmap_set(bitmap, capacity, index_to_bit_pos(hole_idx), hole_value);
        trace("...then checking the resulting bit pattern");
        for (size_t idx = 0; idx < capacity; ++idx) {
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
            ensure_eq(bitmap_get(bitmap,
                                 capacity,
                                 index_to_bit_pos(idx)),
                      expected);
        }
    }

    /// Sub-test of test_bitmap() that exercises bitmaps with a uniform value on
    /// top of which a "hole" has been "punched" by writing the opposite value
    /// in linear index range [hole_start; hole_end[.
    static void test_bitmap_with_hole(word_t bitmap[],
                                      size_t capacity,
                                      size_t hole_start,
                                      size_t hole_end,
                                      bool main_value) {
        const bool hole_value = !main_value;
        tracef("Using main value %u and hole value %u.", main_value, hole_value);

        trace("Filling the bitmap with the desired pattern...");
        bitmap_range_set(bitmap,
                         capacity,
                         BITMAP_START,
                         bitmap_end(capacity),
                         main_value);
        bitmap_range_set(bitmap,
                         capacity,
                         index_to_bit_pos(hole_start),
                         index_to_bit_pos(hole_end),
                         hole_value);

        trace("Testing bitmap_get()...");
        check_bitmap_get(bitmap,
                         capacity,
                         hole_start,
                         hole_end,
                         main_value);

        trace("Testing bitmap_count()...");
        check_bitmap_count(bitmap,
                           capacity,
                           hole_start,
                           hole_end,
                           main_value);

        trace("Testing bitmap_range_alleq()...");
        check_bitmap_range_alleq(bitmap,
                                 capacity,
                                 hole_start,
                                 hole_end,
                                 main_value);

        trace("Testing bitmap_find_first()...");
        check_bitmap_find_first(bitmap,
                                capacity,
                                hole_start,
                                hole_end,
                                main_value);

        trace("Testing bitmap_find_next()...");
        check_bitmap_find_next(bitmap,
                               capacity,
                               hole_start,
                               hole_end,
                               main_value);

        trace("Testing bitmap_set()...");
        check_bitmap_set(bitmap,
                         capacity,
                         hole_start,
                         hole_end,
                         main_value);
    }

    /// Sub-test of bitmap_unit_tests() that runs with a certain bitmap capacity
    static void test_bitmap(word_t bitmap[], size_t capacity) {
        for (size_t hole_start = 0; hole_start < capacity; ++hole_start) {
            if (!is_interesting(hole_start)) continue;
            for (size_t hole_end = 0; hole_end <= capacity; ++hole_end) {
                if (!is_interesting(hole_end)) continue;
                tracef("Testing bitmaps with a \"hole\" at index range [%zu; %zu[...",
                       hole_start, hole_end);
                test_bitmap_with_hole(bitmap, capacity, hole_start, hole_end, false);
                test_bitmap_with_hole(bitmap, capacity, hole_start, hole_end, true);
            }
        }
    }

    void bitmap_unit_tests() {
        info("Running bitmap unit tests...");
        for (size_t capacity = 0; capacity <= 3 * BITS_PER_WORD; ++capacity) {
            if (is_interesting(capacity)) {
                debugf("Testing with a bitmap of capacity %zu.", capacity);
                INLINE_BITMAP(bitmap, capacity);
                test_bitmap(bitmap, capacity);
            }
        }
    }

#endif  // UDIPE_BUILD_TESTS
