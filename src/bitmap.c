#ifdef UDIPE_BUILD_TESTS

    #include "bitmap.h"

    #include "error.h"


    #define TEST_BITMAP_CAPACITY (3*64 + 13)


    static void test_homogeneous_bitmap(word_t bitmap[],
                                        size_t capacity,
                                        bool value) {
        debugf("- Filling bitmap with %us...", value);
        bitmap_fill(bitmap, capacity, value);

        debug("- Checking result of bitmap_all()...");
        ensure(bitmap_all(bitmap, capacity, value));
        ensure(!bitmap_all(bitmap, capacity, !value));

        debug("- Checking results of bitmap_get()...");
        for (size_t idx = 0; idx < capacity; ++idx) {
            ensure_eq(bitmap_get(bitmap,
                                 capacity,
                                 index_to_bit_pos(idx)),
                      value);
        }

        debugf("- Checking results of bitmap_find_first() and _next() "
               "when looking for %u with no wraparound...", value);
        bit_pos_t pos = bitmap_find_first(bitmap,
                                          capacity,
                                          value);
        ensure_eq(pos.word, FIRST_BIT_POS.word);
        ensure_eq(pos.offset, FIRST_BIT_POS.offset);
        for (size_t idx = 1; idx < capacity; ++idx) {
            tracef("  * At index %zu", idx);
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

        debug("- Checking effect of wraparound...");
        const bit_pos_t back_from_end = bitmap_find_next(bitmap,
                                                         capacity,
                                                         value,
                                                         pos,
                                                         true);
        ensure_eq(back_from_end.word, FIRST_BIT_POS.word);
        ensure_eq(back_from_end.offset, FIRST_BIT_POS.offset);
        pos = back_from_end;
        for (size_t idx = 1; idx < capacity; ++idx) {
            tracef("  * At index %zu", idx);
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

        debugf("- Checking that search for %u fails...", !value);
        const bit_pos_t nonexistent = bitmap_find_first(bitmap,
                                                        capacity,
                                                        !value);
        ensure_eq(nonexistent.word, NO_BIT_POS.word);
        ensure_eq(nonexistent.offset, NO_BIT_POS.offset);
    }


    void bitmap_unit_tests() {
        info("Running bitmap unit tests...");

        debug("Setting up bitmap...");
        INLINE_BITMAP(bitmap, TEST_BITMAP_CAPACITY);
        const size_t bitmap_capacity = TEST_BITMAP_CAPACITY;

        debug("Testing homogeneous bitmaps...");
        test_homogeneous_bitmap(bitmap, bitmap_capacity, false);
        test_homogeneous_bitmap(bitmap, bitmap_capacity, true);

        // TODO: then proptests over random bitmaps
    }

#endif
