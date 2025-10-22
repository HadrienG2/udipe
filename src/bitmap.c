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

    /// Sub-test of test_bitmap() that exercises bitmaps with a uniform value
    static void test_homogeneous_bitmap(word_t bitmap[],
                                        size_t capacity,
                                        bool value) {
        tracef("Filling bitmap with %us...", value);
        bitmap_fill(bitmap, capacity, BITMAP_START, bitmap_end(capacity), value);

        trace("Checking result of bitmap_all()...");
        ensure(bitmap_all(bitmap,
                          capacity,
                          BITMAP_START,
                          bitmap_end(capacity),
                          value));
        ensure((!bitmap_all(bitmap,
                            capacity,
                            BITMAP_START,
                            bitmap_end(capacity),
                            !value))
               || (capacity == 0));

        trace("Checking results of bitmap_get()...");
        for (size_t idx = 0; idx < capacity; ++idx) {
            tracef("Getting value at index %zu", idx);
            ensure_eq(bitmap_get(bitmap,
                                 capacity,
                                 index_to_bit_pos(idx)),
                      value);
        }

        tracef("Checking results of bitmap_find_first() and _next() "
               "when looking for %u with no wraparound...", value);
        bit_pos_t pos = bitmap_find_first(bitmap,
                                          capacity,
                                          value);
        if (capacity == 0) {
            ensure_eq(pos.word, NO_BIT_POS.word);
            ensure_eq(pos.offset, NO_BIT_POS.offset);
        } else {
            ensure_eq(pos.word, (size_t)0);
            ensure_eq(pos.offset, (size_t)0);
            for (size_t idx = 1; idx < capacity; ++idx) {
                tracef("Searching from index %zu", idx);
                const bit_pos_t new_pos = bitmap_find_next(bitmap,
                                                           capacity,
                                                           value,
                                                           pos,
                                                           false);
                const bit_pos_t expected = index_to_bit_pos(idx);
                ensure_eq(new_pos.word, expected.word);
                ensure_eq(new_pos.offset, expected.offset);
                pos = new_pos;
            }
            const bit_pos_t past_the_end = bitmap_find_next(bitmap,
                                                            capacity,
                                                            value,
                                                            pos,
                                                            false);
            ensure_eq(past_the_end.word, NO_BIT_POS.word);
            ensure_eq(past_the_end.offset, NO_BIT_POS.offset);

            trace("Checking effect of wraparound...");
            const bit_pos_t back_from_end = bitmap_find_next(bitmap,
                                                             capacity,
                                                             value,
                                                             pos,
                                                             true);
            ensure_eq(back_from_end.word, (size_t)0);
            ensure_eq(back_from_end.offset, (size_t)0);
            pos = back_from_end;
            for (size_t idx = 1; idx < capacity; ++idx) {
                tracef("Searching from index %zu", idx);
                const bit_pos_t new_pos = bitmap_find_next(bitmap,
                                                           capacity,
                                                           value,
                                                           pos,
                                                           true);
                const bit_pos_t expected = index_to_bit_pos(idx);
                ensure_eq(new_pos.word, expected.word);
                ensure_eq(new_pos.offset, expected.offset);
                pos = new_pos;
            }

            tracef("Checking that search for %u fails...", !value);
            const bit_pos_t nonexistent = bitmap_find_first(bitmap,
                                                            capacity,
                                                            !value);
            ensure_eq(nonexistent.word, NO_BIT_POS.word);
            ensure_eq(nonexistent.offset, NO_BIT_POS.offset);
        }

        trace("Testing bitmap_set()...");
        for (size_t set_idx = 0; set_idx < capacity; set_idx += 2) {
            tracef("Setting index %zu.", set_idx);
            bitmap_set(bitmap,
                       capacity,
                       index_to_bit_pos(set_idx),
                       !value);
            for (size_t get_idx = 0; get_idx < capacity; ++get_idx) {
                tracef("Getting index %zu.", get_idx);
                const bool expected = ((get_idx <= set_idx) && ((get_idx % 2) == 0))
                                    ? !value
                                    : value;
                ensure_eq(bitmap_get(bitmap,
                                     capacity,
                                     index_to_bit_pos(get_idx)),
                          expected);
            }
        }
    }

    /// Sub-test of test_bitmap_with_hole() that exercises bitmap_all()
    static void check_bitmap_all_with_hole(word_t bitmap[],
                                           size_t capacity,
                                           size_t hole_start,
                                           size_t hole_end,
                                           bool main_value) {
        const bool hole_value = !main_value;
        #define all(start, end, value)  \
            bitmap_all(bitmap, capacity, (start), (end), (value))
        // Before hole
        ensure(all(BITMAP_START,
                   index_to_bit_pos(hole_start),
                   main_value));
        ensure_eq(all(BITMAP_START,
                      index_to_bit_pos(hole_start),
                      hole_value),
                  hole_start == 0);
        // During hole
        ensure(all(index_to_bit_pos(hole_start),
                   index_to_bit_pos(hole_end),
                   hole_value));
        ensure_eq(all(index_to_bit_pos(hole_start),
                      index_to_bit_pos(hole_end),
                      main_value),
                  hole_start >= hole_end);
        // After hole
        ensure(all(index_to_bit_pos(hole_end),
                   bitmap_end(capacity),
                   main_value));
        ensure_eq(all(index_to_bit_pos(hole_end),
                      bitmap_end(capacity),
                      hole_value),
                  hole_end == capacity);
        // Shifting hole_start by -1
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
        // Shifting hole_start by +1
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
        // Shifting hole_end by -1
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
        // Shifting hole_end by +1
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

        trace("Filling the bitmap with the specified pattern...");
        bitmap_fill(bitmap,
                    capacity,
                    BITMAP_START,
                    bitmap_end(capacity),
                    main_value);
        bitmap_fill(bitmap,
                    capacity,
                    index_to_bit_pos(hole_start),
                    index_to_bit_pos(hole_end),
                    hole_value);

        trace("Checking the value of each bit individually...");
        for (size_t idx = 0; idx < capacity; ++idx) {
            bool expected_value = main_value;
            expected_value ^= (idx >= hole_start) && (idx < hole_end);
            ensure_eq(bitmap_get(bitmap,
                                 capacity,
                                 index_to_bit_pos(idx)),
                      expected_value);
        }

        trace("Checking the value of bits collectively...");
        check_bitmap_all_with_hole(bitmap,
                                   capacity,
                                   hole_start,
                                   hole_end,
                                   main_value);

        // TODO: Test bitmap_find_first and bitmap_find_next
    }

    /// Sub-test of bitmap_unit_tests() that runs with a certain bitmap capacity
    static void test_bitmap(word_t bitmap[], size_t capacity) {
        trace("Testing homogeneous bitmaps...");
        test_homogeneous_bitmap(bitmap, capacity, false);
        test_homogeneous_bitmap(bitmap, capacity, true);

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
        for (size_t capacity = 0; capacity < 4 * BITS_PER_WORD; ++capacity) {
            if (is_interesting(capacity)) {
                debugf("Testing with a bitmap of capacity %zu.", capacity);
                INLINE_BITMAP(bitmap, capacity);
                test_bitmap(bitmap, capacity);
            }
        }
    }

#endif
