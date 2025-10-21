#ifdef UDIPE_BUILD_TESTS

    #include "bitmap.h"

    #include "error.h"


    static void test_homogeneous_bitmap(word_t bitmap[],
                                        size_t capacity,
                                        bool value) {
        tracef("Filling bitmap with %us...", value);
        bitmap_fill(bitmap, capacity, value);

        trace("Checking result of bitmap_all()...");
        ensure(bitmap_all(bitmap, capacity, value));
        ensure((!bitmap_all(bitmap, capacity, !value)) || (capacity == 0));

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
            return;
        }
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


    static void test_bitmap_hole(word_t bitmap[],
                                 size_t capacity,
                                 bool main_value,
                                 size_t other_value_idx) {
        tracef("Filling bitmap with %us...", main_value);
        bitmap_fill(bitmap, capacity, main_value);

        tracef("Putting a %u at position %zu...", !main_value, other_value_idx);
        const bit_pos_t other_value_pos = index_to_bit_pos(other_value_idx);
        bitmap_set(bitmap, capacity, other_value_pos, !main_value);

        trace("Checking result of bitmap_all()...");
        ensure((!bitmap_all(bitmap, capacity, main_value)) || (capacity == 0));
        ensure((!bitmap_all(bitmap, capacity, !main_value)) || (capacity <= 1));

        trace("Checking results of bitmap_get()...");
        for (size_t idx = 0; idx < capacity; ++idx) {
            tracef("Getting value at index %zu", idx);
            ensure_eq(bitmap_get(bitmap,
                                 capacity,
                                 index_to_bit_pos(idx)),
                      main_value ^ (idx == other_value_idx));
        }

        tracef("Checking results of bitmap_find_first() and _next() "
               "when looking for %u with no wraparound...", main_value);
        bit_pos_t pos = bitmap_find_first(bitmap,
                                          capacity,
                                          main_value);
        bit_pos_t first_pos;
        if (capacity <= 1) {
            ensure_eq(pos.word, NO_BIT_POS.word);
            ensure_eq(pos.offset, NO_BIT_POS.offset);
            first_pos = NO_BIT_POS;
        } else {
            ensure_eq(pos.word, (size_t)0);
            if (other_value_idx == (size_t)0) {
                assert(capacity > 1);
                ensure_eq(pos.offset, (size_t)1);
            } else {
                ensure_eq(pos.offset, (size_t)0);
            }
            first_pos = pos;
            const size_t last_idx = capacity - 1 - (other_value_idx == (capacity - 1));
            while (bit_pos_to_index(pos) < last_idx) {
                const size_t idx = bit_pos_to_index(pos);
                tracef("Searching from index %zu", idx);
                const bit_pos_t new_pos = bitmap_find_next(bitmap,
                                                           capacity,
                                                           main_value,
                                                           pos,
                                                           false);
                size_t expected_idx = idx + 1;
                if (expected_idx == other_value_idx) ++expected_idx;
                const bit_pos_t expected_pos = index_to_bit_pos(expected_idx);
                ensure_eq(new_pos.word, expected_pos.word);
                ensure_eq(new_pos.offset, expected_pos.offset);
                pos = new_pos;
            }
            const bit_pos_t past_the_end = bitmap_find_next(bitmap,
                                                            capacity,
                                                            main_value,
                                                            pos,
                                                            false);
            ensure_eq(past_the_end.word, NO_BIT_POS.word);
            ensure_eq(past_the_end.offset, NO_BIT_POS.offset);

            trace("Checking effect of wraparound...");
            const bit_pos_t back_from_end = bitmap_find_next(bitmap,
                                                             capacity,
                                                             main_value,
                                                             pos,
                                                             true);
            ensure_eq(back_from_end.word, first_pos.word);
            ensure_eq(back_from_end.offset, first_pos.offset);
            pos = back_from_end;
            while (bit_pos_to_index(pos) < last_idx) {
                const size_t idx = bit_pos_to_index(pos);
                tracef("Searching from index %zu", idx);
                const bit_pos_t new_pos = bitmap_find_next(bitmap,
                                                           capacity,
                                                           main_value,
                                                           pos,
                                                           true);
                size_t expected_idx = idx + 1;
                if (expected_idx == other_value_idx) ++expected_idx;
                const bit_pos_t expected_pos = index_to_bit_pos(expected_idx);
                ensure_eq(new_pos.word, expected_pos.word);
                ensure_eq(new_pos.offset, expected_pos.offset);
                pos = new_pos;
            }
        }

        tracef("Checking that search for %u only yields one result...", !main_value);
        const bit_pos_t other_pos = bitmap_find_first(bitmap,
                                                      capacity,
                                                      !main_value);
        const bit_pos_t expected_pos = index_to_bit_pos(other_value_idx);
        ensure_eq(other_pos.word, expected_pos.word);
        ensure_eq(other_pos.offset, expected_pos.offset);
        const bit_pos_t nope_pos = bitmap_find_next(bitmap,
                                                    capacity,
                                                    !main_value,
                                                    other_pos,
                                                    false);
        ensure_eq(nope_pos.word, NO_BIT_POS.word);
        ensure_eq(nope_pos.offset, NO_BIT_POS.offset);
        const bit_pos_t wrap_pos = bitmap_find_next(bitmap,
                                                    capacity,
                                                    !main_value,
                                                    other_pos,
                                                    true);
        ensure_eq(wrap_pos.word, other_pos.word);
        ensure_eq(wrap_pos.offset, other_pos.offset);
    }


    static void test_bitmap_with_capacity(size_t capacity) {
        debugf("Testing with a bitmap of capacity %zu.", capacity);
        INLINE_BITMAP(bitmap, capacity);
        const size_t bitmap_capacity = capacity;

        trace("Testing homogeneous bitmaps...");
        test_homogeneous_bitmap(bitmap, bitmap_capacity, false);
        test_homogeneous_bitmap(bitmap, bitmap_capacity, true);

        trace("Testing mostly-homogeneous bitmaps with a \"hole\"...");
        for (size_t hole_idx = 0; hole_idx < bitmap_capacity; ++hole_idx) {
            tracef("Hole at index %zu", hole_idx);
            test_bitmap_hole(bitmap, bitmap_capacity, false, hole_idx);
            test_bitmap_hole(bitmap, bitmap_capacity, true, hole_idx);
        }
    }


    void bitmap_unit_tests() {
        info("Running bitmap unit tests...");
        test_bitmap_with_capacity(0);
        test_bitmap_with_capacity(1);
        test_bitmap_with_capacity(2);
        test_bitmap_with_capacity(BITS_PER_WORD - 2);
        test_bitmap_with_capacity(BITS_PER_WORD - 1);
        test_bitmap_with_capacity(BITS_PER_WORD);
        test_bitmap_with_capacity(BITS_PER_WORD + 1);
        test_bitmap_with_capacity(BITS_PER_WORD + 2);
        test_bitmap_with_capacity(2 * BITS_PER_WORD - 2);
        test_bitmap_with_capacity(2 * BITS_PER_WORD - 1);
        test_bitmap_with_capacity(2 * BITS_PER_WORD);
        test_bitmap_with_capacity(2 * BITS_PER_WORD + 1);
        test_bitmap_with_capacity(2 * BITS_PER_WORD + 2);
        test_bitmap_with_capacity(3 * BITS_PER_WORD - 2);
        test_bitmap_with_capacity(3 * BITS_PER_WORD - 1);
        test_bitmap_with_capacity(3 * BITS_PER_WORD);
        test_bitmap_with_capacity(3 * BITS_PER_WORD + 1);
        test_bitmap_with_capacity(3 * BITS_PER_WORD + 2);
    }

#endif
