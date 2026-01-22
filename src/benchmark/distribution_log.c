#ifdef UDIPE_BUILD_BENCHMARKS

    #include "distribution_log.h"

    #include <udipe/log.h>
    #include <udipe/pointer.h>

    #include "distribution.h"

    #include "../error.h"
    #include "../log.h"

    #include <assert.h>
    #include <math.h>
    #include <stddef.h>
    #include <stdint.h>
    #include <stdlib.h>
    #include <string.h>


    // === Configuration constants ===

    /// Width of the distribution_log() textual display
    ///
    /// Increasing this improves the value count resolution of the textual
    /// histogram, but the client needs a wider terminal to avoid getting a
    /// garbled visual output.
    #define DISTRIBUTION_WIDTH ((size_t)80)

    /// Height of the distribution_log() textual display
    ///
    /// Increasing this improves the value resolution of the textual histogram,
    /// but the client needs a taller terminal to avoid seeing the entire
    /// distribution at once without scrolling.
    #define DISTRIBUTION_HEIGHT ((size_t)25)

    /// Segment from a single Unicode box-drawing line
    ///
    /// These ancient box drawing code points were already supported by the
    /// original IBM PC, and should therefore be available in any
    /// self-respecting modern terminal font.
    static const char* const SINGLE_SEGMENT = "─";

    /// Segment from a double Unicode bow-drawing line
    ///
    /// See \ref SINGLE_LINE_SEGMENT for terminal font compatibility notes.
    static const char* const DOUBLE_SEGMENT = "═";


    UDIPE_NON_NULL_ARGS
    void distribution_log(const distribution_t* dist,
                          udipe_log_level_t level,
                          const char header[]) {
        if (log_enabled(level)) {
            const size_t line_size = line_buffer_size(DISTRIBUTION_WIDTH);
            char* const left_line = alloca(line_size);
            char* const right_line = alloca(line_size);

            write_title_borders(left_line,
                                header,
                                right_line,
                                SINGLE_SEGMENT,
                                DISTRIBUTION_WIDTH);
            udipe_logf(level, "%s%s%s", left_line, header, right_line);

            log_plot(level,
                     "Histogram",
                     dist,
                     HISTOGRAM);

            log_plot(level,
                     "Quantile function",
                     dist,
                     QUANTILE_FUNCTION);
        }
    }


    // === Implementation details ===

    size_t line_buffer_size(size_t max_width) {
        const size_t single_segment_size = strlen(SINGLE_SEGMENT);
        const size_t double_segment_size = strlen(DOUBLE_SEGMENT);
        const size_t max_segment_size =
            (single_segment_size <= double_segment_size) ? double_segment_size
                                                         : single_segment_size;
        return max_width*max_segment_size + 1;
    }

    UDIPE_NON_NULL_ARGS
    void write_horizontal_line(char buffer[],
                                      const char segment[],
                                      size_t width) {
        const size_t segment_size = strlen(segment);
        for (size_t x = 0; x < width; ++x) {
            memcpy(buffer + x*segment_size, segment, segment_size);
        }
        buffer[width*segment_size] = '\0';
    }

    UDIPE_NON_NULL_ARGS
    void write_title_borders(char left_buffer[],
                             const char title[],
                             char right_buffer[],
                             const char line_segment[],
                             size_t width) {
        const size_t min_width = 2 + strlen(title);
        const size_t line_width = (min_width < width) ? width - min_width
                                                      : 0;
        const size_t right_width = line_width / 2;
        const size_t left_width = line_width - right_width;
        write_horizontal_line(left_buffer, line_segment, left_width);
        const size_t segment_size = strlen(line_segment);
        left_buffer[left_width * segment_size] = ' ';
        left_buffer[left_width * segment_size + 1] = '\0';

        right_buffer[0] = ' ';
        write_horizontal_line(right_buffer + 1, line_segment, right_width);
    }

    axis_len_t plot_axis_len(plot_type_t type) {
        // -1 because there is no data on the title line
        const size_t ordinate_len = DISTRIBUTION_HEIGHT - 1;
        switch (type) {
        case HISTOGRAM:
            return (axis_len_t){
                // Histograms have the start position on the title line followed
                // by one value per bin which represents the end of the previous
                // bin (inclusive) and the start of the next bin (exclusive).
                .abscissa = ordinate_len + 1,
                .ordinate = ordinate_len
            };
        case QUANTILE_FUNCTION:
            return (axis_len_t){
                // Quantile functions do not have anything on the title line
                .abscissa = ordinate_len,
                .ordinate = ordinate_len
            };
        }
        exit_with_error("Control should never reach this point!");
    }

    UDIPE_NON_NULL_ARGS
    range_t plot_autoscale_abscissa(const distribution_t* dist,
                                    plot_type_t type) {
        switch (type) {
        case HISTOGRAM:
            return (range_t){
                .first = (coord_t){ .value = distribution_min(dist) },
                .last = (coord_t){ .value = distribution_max(dist) }
            };
        case QUANTILE_FUNCTION:
            return (range_t){
                .first = (coord_t){ .percentile = 0.0 },
                .last = (coord_t){ .percentile = 100.0 }
            };
        }
        exit_with_error("Control should never reach this point!");
    }

    UDIPE_NON_NULL_ARGS
    void plot_compute_abscissa(plot_type_t type,
                               coord_t abscissa[],
                               range_t range,
                               axis_len_t len) {
        ensure_ge(len.abscissa, (size_t)2);
        switch (type) {
        case HISTOGRAM: {
            const int64_t first = range.first.value;
            const int64_t last = range.last.value;
            for (size_t a = 0; a < len.abscissa; ++a) {
                const int64_t value = first + (last - first) * a / (len.abscissa - 1);
                abscissa[a] = (coord_t){ .value = value };
            }
            break;
        }
        case QUANTILE_FUNCTION: {
            const double first = range.first.percentile;
            const double last = range.last.percentile;
            for (size_t a = 0; a < len.abscissa; ++a) {
                const double percentile = first + (last - first) * a / (len.abscissa - 1);
                abscissa[a] = (coord_t){ .percentile = percentile };
            }
            break;
        }}
    }

    UDIPE_NON_NULL_ARGS
    void plot_compute_ordinate(const distribution_t* dist,
                               plot_type_t type,
                               const coord_t abscissa[],
                               coord_t ordinate[],
                               axis_len_t len) {
        switch (type) {
        case HISTOGRAM:
            ensure_eq(len.abscissa, len.ordinate + 1);
            size_t start_rank = distribution_count_below(dist,
                                                         abscissa[0].value,
                                                         false);
            for (size_t o = 0; o < len.ordinate; ++o) {
                const size_t end_rank =
                    distribution_count_below(dist,
                                             abscissa[o+1].value,
                                             true);
                if (abscissa[o+1].value > abscissa[o].value || o == 0) {
                    const size_t count = end_rank - start_rank;
                    ordinate[o] = (coord_t){ .count = count };
                } else {
                    assert(abscissa[o+1].value == abscissa[o].value);
                    ordinate[o] = ordinate[o-1];
                }
                start_rank = end_rank;
            }
            break;
        case QUANTILE_FUNCTION:
            ensure_eq(len.abscissa, len.ordinate);
            for (size_t o = 0; o < len.ordinate; ++o) {
                const double probability = abscissa[o].percentile / 100.0;
                assert(probability >= 0.0 && probability <= 1.0);
                const int64_t quantile = distribution_quantile(dist, probability);
                ordinate[o] = (coord_t){ .value = quantile };
            }
            break;
        }
    }

    UDIPE_NON_NULL_ARGS
    range_t plot_autoscale_ordinate(plot_type_t type,
                                    const coord_t ordinate[],
                                    axis_len_t len) {
        ensure_ge(len.ordinate, (size_t)1);
        switch (type) {
        case HISTOGRAM:
            size_t max_count = 0;
            for (size_t o = 0; o < len.ordinate; ++o) {
                const size_t count = ordinate[o].count;
                if (count > max_count) max_count = count;
            }
            return (range_t){
                .first = (coord_t){ .count = 0 },
                .last = (coord_t){ .count = max_count }
            };
        case QUANTILE_FUNCTION:
            return (range_t){
                .first = (coord_t){ .value = ordinate[0].value },
                .last = (coord_t){ .count = ordinate[len.ordinate - 1].value }
            };
        }
        exit_with_error("Control should never reach this point!");
    }

    UDIPE_NON_NULL_ARGS
    plot_layout_t plot_layout(plot_type_t type,
                              const coord_t abscissa[],
                              const coord_t ordinate[],
                              axis_len_t len) {
        ensure_ge(len.ordinate, (size_t)1);
        plot_layout_t result = { 0 };
        size_t legend_width = 0, max_ordinate_width = 0;
        switch (type) {
        case HISTOGRAM: {
            ensure_ge(len.abscissa, (size_t)1);
            const int min_width = printf_width_i64(abscissa[0].value);
            const int max_width =
                printf_width_i64(abscissa[len.abscissa - 1].value);
            const int value_width = (min_width <= max_width) ? max_width
                                                             : min_width;

            // 4 columns for the leading "to " and trailing ╔/╟ separator
            legend_width = value_width + 4;

            size_t max_count = 0;
            for (size_t o = 0; o < len.ordinate; ++o) {
                const size_t count = ordinate[o].count;
                if (count > max_count) max_count = count;
            }
            ensure_le(max_count, (size_t)INT64_MAX);
            max_ordinate_width = printf_width_i64(max_count);

            result = (plot_layout_t){
                .histogram = (histogram_layout_t){
                    .max_count = max_count,
                    .value_width = value_width
                }
            };
            break;
        }
        case QUANTILE_FUNCTION: {
            ensure_ge(len.abscissa, (size_t)2);
            const double min_percent_delta =
                abscissa[1].percentile - abscissa[0].percentile;
            const int percent_precision = (min_percent_delta >= 1.0)
                                        ? 1
                                        : 1 + ceil(-logf(min_percent_delta));
            // 4 extra columns for the largest leading "100." of last percentile
            const int percent_width = percent_precision + 4;

            // 1 column for the trailing % and ╔/╟ separator
            legend_width = percent_width + 2;

            const int64_t max_value = ordinate[len.ordinate - 1].value;
            max_ordinate_width = printf_width_i64(max_value);

            result = (plot_layout_t){
                .quantile_function = (quantile_function_layout_t){
                    .percent_precision = percent_precision,
                    .percent_width = percent_width
                }
            };
            break;
        }}

        result.data_width  = (DISTRIBUTION_WIDTH > legend_width)
                           ? DISTRIBUTION_WIDTH - legend_width
                           : 0;

        // Extra columns for the ┤ bar/value separator and value display
        const size_t non_bar_width = max_ordinate_width + 1;
        result.max_bar_width = (result.data_width > non_bar_width)
                             ? result.data_width - non_bar_width
                             : 0;
        return result;
    }

    UDIPE_NON_NULL_ARGS
    void plot_draw_line(plot_type_t type,
                        const plot_layout_t* layout,
                        range_t ordinate_range,
                        coord_t ordinate,
                        char output[]) {
        double rel_ordinate = NAN;
        switch (type) {
        case HISTOGRAM: {
            const size_t count = ordinate.count;
            const size_t first_count = ordinate_range.first.count;
            const size_t last_count = ordinate_range.last.count;
            if (first_count < last_count) {
                rel_ordinate = (count - first_count)
                             / (double)(last_count - first_count);
            } else {
                assert(first_count == last_count);
                rel_ordinate = 0.5;
            }
            break;
        }
        case QUANTILE_FUNCTION: {
            const int64_t value = ordinate.value;
            const int64_t first_value = ordinate_range.first.value;
            const int64_t last_value = ordinate_range.last.value;
            if (first_value < last_value) {
                rel_ordinate = (value - first_value)
                             / (double)(last_value - first_value);
            } else {
                assert(first_value == last_value);
                rel_ordinate = 0.5;
            }
            break;
        }}

        const double clamped_ordinate =
            (rel_ordinate < 0.0) ? 0.0
                                 : (rel_ordinate > 1.0) ? 1.0
                                                        : rel_ordinate;
        const size_t bar_width =
            ceil(layout->max_bar_width * clamped_ordinate);
        write_horizontal_line(output, SINGLE_SEGMENT, bar_width);
    }

    UDIPE_NON_NULL_ARGS
    void log_plot(udipe_log_level_t level,
                  const char title[],
                  const distribution_t* dist,
                  plot_type_t type) {
        const axis_len_t len = plot_axis_len(type);

        coord_t* const abscissa = alloca(len.abscissa * sizeof(coord_t));
        const range_t abscissa_range = plot_autoscale_abscissa(dist, type);
        plot_compute_abscissa(type, abscissa, abscissa_range, len);

        coord_t* const ordinate = alloca(len.ordinate * sizeof(coord_t));
        plot_compute_ordinate(dist, type, abscissa, ordinate, len);
        const range_t ordinate_range = plot_autoscale_ordinate(type,
                                                               ordinate,
                                                               len);

        const plot_layout_t layout = plot_layout(type,
                                                 abscissa,
                                                 ordinate,
                                                 len);

        const size_t line_size = line_buffer_size(layout.data_width);
        char* const left_line = alloca(line_size);
        char* const right_line = alloca(line_size);

        write_title_borders(left_line,
                            title,
                            right_line,
                            DOUBLE_SEGMENT,
                            layout.data_width);
        char* const bar_line = left_line;
        switch (type) {
        case HISTOGRAM:
            const int value_width = layout.histogram.value_width;
            udipe_logf(level,
                       "   %*zd╔"
                       "%s%s%s",
                       value_width, abscissa[0].value,
                       left_line, title, right_line);
            for (size_t o = 0; o < len.ordinate; ++o) {
                plot_draw_line(type,
                               &layout,
                               ordinate_range,
                               ordinate[o],
                               bar_line);
                udipe_logf(level,
                           "to %*zd╟"
                           "%s┤%zu",
                           value_width, abscissa[o+1].value,
                           bar_line, ordinate[o].count);
            }
            break;
        case QUANTILE_FUNCTION:
            udipe_logf(level,
                       "%*s ╔"
                       "%s%s%s",
                       layout.quantile_function.percent_width, "",
                       left_line, title, right_line);
            for (size_t o = 0; o < len.ordinate; ++o) {
                plot_draw_line(type,
                               &layout,
                               ordinate_range,
                               ordinate[o],
                               bar_line);
                const int percent_width = layout.quantile_function.percent_width;
                const int percent_precision = layout.quantile_function.percent_precision;
                const double percentile = abscissa[o].percentile;
                udipe_logf(level,
                           "%*.*f%%"
                           "╟%s┤%zd",
                           percent_width, percent_precision, percentile,
                           bar_line, ordinate[o].value);
            }
            break;
        }
    }

#endif  // UDIPE_BUILD_BENCHMARKS
