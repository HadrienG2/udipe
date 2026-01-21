#ifdef UDIPE_BUILD_BENCHMARKS

    #include "benchmark.h"

    #include <udipe/log.h>
    #include <udipe/pointer.h>

    #include "memory.h"
    #include "unit_tests.h"
    #include "visibility.h"

    #include <assert.h>
    #include <hwloc.h>
    #include <math.h>
    #include <stddef.h>
    #include <stdint.h>
    #include <stdlib.h>
    #include <string.h>


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

    /// Number of samples used for median duration computations
    ///
    /// To reduce the impact of outliers, we don't directly handle raw
    /// durations, we handle medians of a small number of duration samples. This
    /// parameter controls the number of samples that are used.
    ///
    /// Tuning this parameter has many consequences:
    ///
    /// - It can only take odd values. No pseudo-median allowed.
    /// - Tuning it higher allows you to tolerate more OS interrupts, and thus
    ///   work with benchmark run durations that are closer to the
    ///   inter-interrupt spacing. Given a fixed run timing precision, these
    ///   longer benchmark runs let you achieve lower uncertainty on the
    ///   benchmark iteration duration.
    /// - Tuning it higher makes statistics more sensitive to the difference
    ///   between the empirical duration distribution and the true duration
    ///   distribution, therefore you need to collect more benchmark run
    ///   duration data points for the statistics to converge. When combined
    ///   with the use of longer benchmark runs, this means that benchmarks will
    ///   take longer to execute before stable results are achieved.
    #define NUM_MEDIAN_SAMPLES ((size_t)5)
    static_assert(NUM_MEDIAN_SAMPLES % 2 == 1,
                  "Medians are computed over an odd number of samples");

    /// Confidence interval used for all statistics
    ///
    /// Picked because 95% is kinda the standard in statistics, so it is what
    /// the end user will most likely be used to.
    #define CONFIDENCE 95.0

    /// Desired number of measurements on either side of the confidence interval
    ///
    /// Tune this up if you observe unstable duration statistics even though the
    /// underlying duration distributions are stable.
    ///
    /// Tuning it too high will increase the overhead of the statistical
    /// analysis process for no good reason.
    //
    // TODO: Tune on more system
    #define NUM_EDGE_MEASUREMENTS ((size_t)512)

    /// Warmup duration used for OS clock offset calibration
    //
    // TODO: Tune on more systems
    #define WARMUP_OFFSET_OS (1000*UDIPE_MILLISECOND)

    /// Number of benchmark runs used for OS clock offset calibration
    ///
    /// Tune this up if clock offset calibration is unstable, as evidenced by
    /// the fact that short loops get a nonzero median duration.
    //
    // TODO: Tune on more systems
    #define NUM_RUNS_OFFSET_OS ((size_t)64*1024)

    /// Warmup duration used for shortest loop calibration
    //
    // TODO: Tune on more systems
    #define WARMUP_SHORTEST_LOOP (3000*UDIPE_MILLISECOND)

    /// Number of benchmark runs used for shortest loop calibration
    ///
    /// Tune this up if the shortest loop calibration is unstable and does not
    /// converge to a constant loop size.
    //
    // TODO: Tune on more systems
    #define NUM_RUNS_SHORTEST_LOOP ((size_t)64*1024)

    /// Warmup duration used for best loop calibration
    //
    // TODO: Tune on more systems
    #define WARMUP_BEST_LOOP (3000*UDIPE_MILLISECOND)

    /// Number of benchmark run used for optimal loop calibration, when using
    /// the system clock to perform said calibration
    ///
    /// Tune this up if the optimal loop calibration is unstable and does not
    /// converge to sufficiently reproducible statistics.
    ///
    /// Tune this down if you observe multimodal timing laws, which indicates
    /// that the CPU switches performance states during the measurement, and
    /// this state instability is not fixed by using a longer warmup or
    /// adjusting the system's power management configuration.
    //
    // TODO: Tune on more systems
    #define NUM_RUNS_BEST_LOOP_OS ((size_t)64*1024)

    #ifdef X86_64

        /// Number of benchmark runs used when measuring the duration of the
        /// optimal loop using the x86 TimeStamp Counter
        ///
        /// Tune this up if the optimal loop calibration does not yield
        /// reproducible results.
        //
        // TODO: Tune on more systems
        #define NUM_RUNS_BEST_LOOP_X86 ((size_t)8*1024)

        /// Warmup duration used for TSC clock offset calibration
        //
        // TODO: Tune on more systems
        #define WARMUP_OFFSET_X86 (1*UDIPE_MILLISECOND)

        /// Number of benchmark runs used for TSC clock offset calibration
        ///
        /// Tune this up if the TSC offset calibration does not yield
        /// reproducible results.
        //
        // TODO: Tune on more systems
        #define NUM_RUNS_OFFSET_X86 ((size_t)16*1024)

    #endif  // X86_64


    /// Comparison function for applying qsort() to int64_t[]
    static inline int compare_i64(const void* v1, const void* v2) {
        const int64_t* const d1 = (const int64_t*)v1;
        const int64_t* const d2 = (const int64_t*)v2;
        if (*d1 < *d2) return -1;
        if (*d1 > *d2) return 1;
        return 0;
    }


    /// Logical size of a bin from a \ref distribution_t
    ///
    /// \ref distribution_t internally uses a structure-of-array layout, so it
    /// is not literally an array of `(int64_t, size_t)` pairs but rather an
    /// array of `int64_t` followed by an array of `size_t`.
    const size_t distribution_bin_size = sizeof(int64_t) + sizeof(size_t);

    distribution_t distribution_allocate(size_t capacity) {
        void* const allocation = malloc(capacity * distribution_bin_size);
        exit_on_null(allocation, "Failed to allocate distribution storage");
        debugf("Allocated storage for %zu bins at location %p.",
               capacity, allocation);
        return (distribution_t){
            .allocation = allocation,
            .num_bins = 0,
            .capacity = capacity
        };
    }

    distribution_builder_t distribution_initialize() {
        const size_t capacity = get_page_size() / distribution_bin_size;
        return (distribution_builder_t){
            .inner = distribution_allocate(capacity)
        };
    }

    UDIPE_NON_NULL_ARGS
    void distribution_create_bin(distribution_builder_t* builder,
                                 size_t pos,
                                 int64_t value) {
        distribution_t* dist = &builder->inner;
        distribution_layout_t layout = distribution_layout(dist);
        if (dist->num_bins < dist->capacity) {
            trace("There's enough room in the allocation for this new bin.");
            const size_t end_pos = dist->num_bins;
            if (pos == end_pos) {
                trace("New bin is at the end of the histogram, can append it directly.");
                layout.sorted_values[end_pos] = value;
                layout.counts[end_pos] = 1;
                ++(dist->num_bins);
                return;
            }

            tracef("Backing up current bin at position %zu...", pos);
            int64_t next_value = layout.sorted_values[pos];
            size_t next_count = layout.counts[pos];

            trace("Inserting new value...");
            layout.sorted_values[pos] = value;
            layout.counts[pos] = 1;

            trace("Shifting previous bins up...");
            for (size_t dst = pos + 1; dst < dist->num_bins; ++dst) {
                int64_t tmp_value = layout.sorted_values[dst];
                size_t tmp_count = layout.counts[dst];
                layout.sorted_values[dst] = next_value;
                layout.counts[dst] = next_count;
                next_value = tmp_value;
                next_count = tmp_count;
            }

            trace("Restoring last bin...");
            layout.sorted_values[end_pos] = next_value;
            layout.counts[end_pos] = next_count;
            ++(dist->num_bins);
        } else {
            debug("No room for extra bins, must reallocate...");
            assert(dist->num_bins == dist->capacity);
            distribution_t new_dist = distribution_allocate(2 * dist->capacity);
            distribution_layout_t new_layout = distribution_layout(&new_dist);

            trace("Transferring old values smaller than the new one...");
            for (size_t bin = 0; bin < pos; ++bin) {
                new_layout.sorted_values[bin] = layout.sorted_values[bin];
                new_layout.counts[bin] = layout.counts[bin];
            }

            trace("Inserting new value...");
            new_layout.sorted_values[pos] = value;
            new_layout.counts[pos] = 1;

            trace("Transferring old values larger than the new one...");
            for (size_t src = pos; src < dist->num_bins; ++src) {
                const size_t dst = src + 1;
                new_layout.sorted_values[dst] = layout.sorted_values[src];
                new_layout.counts[dst] = layout.counts[src];
            }

            trace("Replacing former distribution...");
            new_dist.num_bins = dist->num_bins + 1;
            distribution_finalize(dist);
            builder->inner = new_dist;
        }
    }

    /// Mark a distribution as poisoned so it cannot be used anymore
    ///
    /// This is used when a distribution is either liberated or moved to a
    /// different variable, in order to ensure that incorrect
    /// user-after-free/move can be detected.
    static inline void distribution_poison(distribution_t* dist) {
        *dist = (distribution_t){
            .allocation = NULL,
            .num_bins = 0,
            .capacity = 0
        };
    }

    /// printf display width of an integer
    ///
    /// This returns the argument that must be passed to the `%*` field width of
    /// a printf format argument in order to match the display width of a
    /// particular integer.
    ///
    /// Such a manual field width setup is needed when displaying justified
    /// columns of integers that have varying magnitude and sign.
    static inline int printf_width_i64(int64_t i) {
        return (i > 0) ? floor(log10(i)) + 1
                       : (i < 0) ? printf_width_i64(-i) + 1  // +1 for minus sign
                                 : 1;
    }

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

    /// Size of a buffer that can hold horizontal lines up to a certain width
    ///
    /// This function determines how many bytes must be allocated to a buffer
    /// that holds a horizontal line made of \ref SINGLE_SEGMENT or \ref
    /// DOUBLE_SEGMENT, typically generated using write_horizontal_line() or as
    /// part of write_title_borders().
    ///
    /// \param max_width is an upper bound on the width of the lines that will
    ///                  be stored into this buffer, in terminal columns.
    ///
    /// \returns the number of bytes needed to hold a horizontal textual line of
    ///          up to `max_width` segments.
    static size_t line_buffer_size(size_t max_width) {
        const size_t single_segment_size = strlen(SINGLE_SEGMENT);
        const size_t double_segment_size = strlen(DOUBLE_SEGMENT);
        const size_t max_segment_size =
            (single_segment_size <= double_segment_size) ? double_segment_size
                                                         : single_segment_size;
        return max_width*max_segment_size + 1;
    }

    /// Generate texte representing a horizontal line of a certain length
    ///
    /// \param buffer should be large enough to hold `width * strlen(segment) +
    ///               1` bytes of UTF-8 data. Consider using the
    ///               `line_buffer_size()` size calculation utility for
    ///               long-lived allocations.
    /// \param segment is the UTF-8 sequence used as a line segment. It should
    ///                typically contain a single Unicode code point, whose
    ///                repeated sequence looks like a horizontal line. For
    ///                optimal results, you should favor segment code points
    ///                that are widely supported by terminal fonts, span the
    ///                full width of a monospace font, and are displayed as a
    ///                single terminal column.
    /// \param width is the number of occurences of `segment` that you want to
    ///              draw instead of `buffer`. As noted above, `buffer` should
    ///              be large enough to hold all of them + a terminating NUL.
    UDIPE_NON_NULL_ARGS
    static void write_horizontal_line(char buffer[],
                                      const char segment[],
                                      size_t width) {
        const size_t segment_size = strlen(segment);
        for (size_t x = 0; x < width; ++x) {
            memcpy(buffer + x*segment_size, segment, segment_size);
        }
        buffer[width*segment_size] = '\0';
    }

    /// Surround a textual title with an horizontal line
    ///
    /// If we denote `half_width` the result of dividing `full_width` by 2 and
    /// rounding up, both `left_buffer` and `right_buffer` should be large
    /// enough to hold `half_width * strlen(segment) + 1` bytes of data.
    ///
    /// \param left_buffer will hold the line from the left side of the title.
    ///                    It should have the minimal size outlined above.
    /// \param title should be an ASCII string. General Unicode strings are only
    ///              partially supported and will result in a display of
    ///              incorrect width.
    /// \param right_buffer will hold the line from the right side of the title.
    ///                     It should have the minimal size outlined above.
    /// \param line_segment is a segment of the line that will surround `title`,
    ///                     see write_horizontal_line() for more info.
    /// \param width is the desired full width of the title in terminal
    ///                 columns.
    UDIPE_NON_NULL_ARGS
    static void write_title_borders(char left_buffer[],
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

    /// Kind of plot being drawn
    ///
    /// Some plot drawing logic depends on the kind of plot that is being drawn,
    /// this enum is used to select the appropriate logic for a plot of
    /// interest.
    ///
    /// It is very important that a consistent `plot_type_t` is used for all
    /// function calls below when generating a certain kind of plot, but the
    /// toplevel `log_plot()` function will take care of this for you.
    typedef enum plot_type_e {
        HISTOGRAM,
        QUANTILE_FUNCTION
    } plot_type_t;

    /// Number of abscissa and ordinate data points in a plot
    ///
    /// Some plots represent a function whose input is consecutive ranges of
    /// values, rather than individual values, and in this case there are more
    /// abscissa than ordinates because for N ordinates we need N+1 abscissas.
    typedef struct axis_len_s {
        size_t abscissa;  ///< Number of abscissa points
        size_t ordinate;  ///< Number of ordinate points
    } axis_len_t;
    //
    /// Compute the \ref axis_len_t of a certain type of plot
    ///
    /// \param type is the kind of plot that as being drawn
    ///
    /// \returns the number of abscissa and ordinate data points that the plot
    ///          will be composed of.
    UDIPE_NON_NULL_ARGS
    static axis_len_t plot_axis_len(plot_type_t type) {
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

    /// Horizontal or vertical plot coordinate
    ///
    /// This union, which is tagged by the \ref plot_type_t in use, is used to
    /// clarify which coordinate type is used in the plot's internal data
    /// buffers below. Indeed, from the perspective of plot type agnostic code,
    /// a coordinate is just an 64-bit data blob of unknown numeric type.
    typedef union coord_u {
        int64_t value;  ///< A value previously inserted into the distribution
        double percentile;  ///< A percentile between 0.0 and 100.0
        size_t count;  ///< A number of values matching some criterion
    } coord_t;

    /// Horizontal or vertical plot range
    ///
    /// This struct is used to set bounds on the value ranges represented by a
    /// plot's axis. The proper way to interpret it depends on the \ref
    /// plot_type_t and the target axis, but it is guaranteed that `first` and
    /// `last` will always use the same \ref coord_t variant.
    typedef struct range_u {
        coord_t first;  ///< Inclusive lower bound
        coord_t last;  ///< Inclusive upper bound
    } range_t;

    /// Automatically determine the full-scale abscissa range for a plot
    ///
    /// This sets up the abscissa axis such that the plot will display the
    /// function of interest over its full range of input values (distribution
    /// elements for histograms, probabilities for quantile functions).
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't yet been recycled via
    ///             distribution_reset() or destroyed via
    ///             distribution_finalize().
    /// \param type is the kind of plot that is being drawn.
    ///
    /// \returns the maximal abscissa range for the plot of interest.
    UDIPE_NON_NULL_ARGS
    static range_t plot_autoscale_abscissa(const distribution_t* dist,
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

    /// Tabulate the abscissa of a plot
    ///
    /// From an abscissa `range` which can be computed via
    /// plot_autoscale_abscissa(), and an axis length `len` which can be
    /// computed via plot_axis_len(), this function generates a linearly spaced
    /// set of abscissa coordinates inside of buffer `abscissa`.
    ///
    /// \param type is the kind of plot that is being drawn, which must be
    ///             consistent with the inputs previously given to
    ///             plot_autoscale_abscissa() and plot_axis_len().
    /// \param abscissa is a buffer that will receive a number of abscissa
    ///                 coordinates dictated by `len::abscissa` and should be
    ///                 dimensioned as such.
    /// \param range is the range that the abscissa can take. It can be
    ///              automatically inferred from data using
    ///              plot_autoscale_abscissa(), and should not be wider than the
    ///              actual data range.
    /// \param len is the length of the plot's axes, which can be generated
    ///            using plot_axis_len().
    UDIPE_NON_NULL_ARGS
    static void plot_compute_abscissa(plot_type_t type,
                                      coord_t abscissa[],
                                      range_t range,
                                      axis_len_t len) {
        assert(len.abscissa >= 2);
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

    /// Number of values smaller than or equal to `value` if `include_equal` is
    /// set, or strictly smaller than `value` otherwise.
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't yet been recycled via
    ///             distribution_reset() or destroyed via
    ///             distribution_finalize().
    /// \param value is the value which you want to position with respect to the
    ///              values inside of `dist`.
    /// \param include_equal specifies if values from `dist` that are equal to
    ///                      `value` should be included in the output count or
    ///                      not.
    ///
    /// \returns the number of values inside of `dist` that are smaller than
    ///          (and possibly equal to) `value`.
    UDIPE_NON_NULL_ARGS
    static inline size_t num_values_below(const distribution_t* dist,
                                          int64_t value,
                                          bool include_equal) {
        const distribution_layout_t layout = distribution_layout(dist);
        const ptrdiff_t pos = distribution_bin_by_value(dist,
                                                        value,
                                                        BIN_BELOW);
        if (pos < 0) return 0;
        const int64_t bin_value = layout.sorted_values[pos];
        if (bin_value < value || include_equal) {
            return layout.end_ranks[pos];
        } else {
            return (pos == 0) ? 0 : layout.end_ranks[pos - 1];
        }
    }

    /// Compute the ordinates from a plot
    ///
    /// From a previously generated set of increasing abscissa values stored in
    /// `abscissa`, which can be generated via plot_compute_abscissa(), this
    /// function writes the matching set of ordinate values to `ordinate`.
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't yet been recycled via
    ///             distribution_reset() or destroyed via
    ///             distribution_finalize().
    /// \param type is the kind of plot that is being drawn, which must be
    ///             consistent with the inputs previously given to
    ///             plot_compute_abscissa() and plot_axis_len().
    /// \param abscissa is the set of abscissa values at which the ordinates
    ///                 will be evaluated, which can be generated using
    ///                 plot_compute_abscissa(). Abscissa values should be
    ///                 sorted in increasing order, but are allowed to repeat.
    /// \param ordinate is a buffer that will receive a number of ordinate
    ///                 coordinates dictated by `len::ordinate` and should be
    ///                 dimensioned as such.
    /// \param len is the length of the plot's axes, which can be generated
    ///            using plot_axis_len().
    UDIPE_NON_NULL_ARGS
    static void plot_compute_ordinate(const distribution_t* dist,
                                      plot_type_t type,
                                      const coord_t abscissa[],
                                      coord_t ordinate[],
                                      axis_len_t len) {
        switch (type) {
        case HISTOGRAM:
            assert(len.abscissa == len.ordinate + 1);
            size_t start_rank = num_values_below(dist, abscissa[0].value, false);
            for (size_t o = 0; o < len.ordinate; ++o) {
                const size_t end_rank = num_values_below(dist,
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
            assert(len.abscissa == len.ordinate);
            for (size_t o = 0; o < len.ordinate; ++o) {
                const double probability = abscissa[o].percentile / 100.0;
                assert(probability >= 0.0 && probability <= 1.0);
                const int64_t quantile = distribution_quantile(dist, probability);
                ordinate[o] = (coord_t){ .value = quantile };
            }
            break;
        }
    }

    /// Automatically determine the full-scale ordinate range for a plot
    ///
    /// This sets up the ordinate axis such that the plot will display the full
    /// range of values from the function of interest, without saturating on the
    /// maximum side, and using either the minimum ordinate value or 0 on the
    /// minimum side depending on what's customary for a given plot type.
    ///
    /// \param type is the kind of plot that is being drawn. It must be
    ///             consistent with the parameter that was passed to
    ///             plot_compute_ordinate() and plot_axis_len().
    /// \param ordinate is the set of ordinates that were previously computed
    ///                 via plots_compute_ordinate().
    /// \param len is the length of the plot's axes, which can be generated
    ///            using plot_axis_len().
    ///
    /// \returns the full-scale ordinate range for the plot of interest.
    UDIPE_NON_NULL_ARGS
    static range_t plot_autoscale_ordinate(plot_type_t type,
                                           const coord_t ordinate[],
                                           axis_len_t len) {
        assert(len.ordinate >= 1);
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

    /// Visual layout parameters specific to histograms
    ///
    /// This information is needed when rendering \ref HISTOGRAM plots.
    typedef struct histogram_layout_s {
        size_t max_count;  ///< Maximal count used as an ordinate value
        int value_width;  ///< Width of abscissa values
    } histogram_layout_t;

    /// Visual layout parameters specific to quantile functions
    ///
    /// This information is needed when rendering \ref QUANTILE_FUNCTION plots.
    typedef struct quantile_function_layout_s {
        int percent_precision;  ///< Precision of abscissa percentiles
        int percent_width;  ///< Width of abscissa percentiles
    } quantile_function_layout_t;

    /// Visual layout parameters of a textual plot
    ///
    /// This visual layout information is needed when rendering a plot.
    typedef struct plot_layout_s {
        // Information specific to a particular plot type
        union {
            histogram_layout_t histogram;
            quantile_function_layout_t quantile_function;
        };

        /// Width of the data region, excluding abscissa legend
        ///
        /// This indicates how many terminal columns can be used when rendering
        /// plot titles, bars and ordinate legends.
        size_t data_width;

        /// Full width of the data bars, excluding abscissa and ordinate legend
        ///
        /// This indicates the number of terminal columns that the longest
        /// display bar should use.
        size_t max_bar_width;
    } plot_layout_t;

    /// Compute a plot's visual layout
    ///
    /// From a plot's abscissa and ordinate data, which were previously computed
    /// using plot_compute_abscissa() and plot_compute_ordinate(), this
    /// determines how the plot should be visually laid out in the terminal i.e.
    /// what are the width and precision parameters of the various print
    /// statements and how many terminal columns can be used by various visual
    /// elements.
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't yet been recycled via
    ///             distribution_reset() or destroyed via
    ///             distribution_finalize().
    /// \param type is the kind of plot that is being drawn, which must be
    ///             consistent with the inputs previously given to
    ///             plot_compute_abscissa() and plot_compute_ordinate().
    /// \param abscissa is the set of abscissa values at which the ordinates
    ///                 will be evaluated, which can be generated using
    ///                 plot_compute_abscissa(). Abscissa values should be
    ///                 sorted in increasing order, but are allowed to repeat.
    /// \param ordinate is the set of ordinate values evaluated at each position
    ///                 or interval from `abscissa`.
    /// \param len is the length of the plot's axes, which can be generated
    ///            using plot_axis_len().
    UDIPE_NON_NULL_ARGS
    static plot_layout_t plot_layout(const distribution_t* dist,
                                     plot_type_t type,
                                     const coord_t abscissa[],
                                     const coord_t ordinate[],
                                     axis_len_t len) {
        assert(len.ordinate >= 1);
        plot_layout_t result = { 0 };
        size_t legend_width, max_ordinate_width;
        switch (type) {
        case HISTOGRAM: {
            assert(len.abscissa >= 1);
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
            assert(max_count <= (size_t)INT64_MAX);
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
            assert(len.abscissa >= 2);
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

    /// Write the plot line associated with `ordinate` to `output`
    ///
    /// This draws the horizontal line used to display a certain `ordinate` into
    /// the buffer `output`, following the ordinate scaling specified by
    /// `ordinate_range` and the terminal column budget specified by `layout`.
    ///
    /// \param type is the kind of plot that is being drawn, which must be
    ///             consistent with the inputs previously given to
    ///             plot_compute_ordinate(), plot_autoscale_ordinate() and
    ///             plot_layout().
    /// \param ordinate_range specifies the minimum and maximum ordinate values
    ///                       outside of which the ordinate scale will saturate
    ///                       to a min/max bar length.
    /// \param layout specifies the plot layout, used here to figure out the
    ///               terminal column budget for horizontal lines.
    /// \param ordinate is the ordinate whose display is meant to be drawn.
    /// \param output is the text buffer into which the ordinate line display
    ///               will be recorded. It must be able to hold at least the
    ///               amount of bytes specified by
    ///               `line_buffer_size(layout->max_bar_width)`.
    UDIPE_NON_NULL_ARGS
    static void plot_draw_line(plot_type_t type,
                               range_t ordinate_range,
                               const plot_layout_t* layout,
                               coord_t ordinate,
                               char output[]) {
        double rel_ordinate;
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

    /// Emit a textual plot of some distribution as a log
    ///
    /// This function should normally be gated on `log_enabled(level)` to ensure
    /// that it is only called when the specified log level is enabled.
    ///
    /// \param level is the log level at which the plot should be emitted
    /// \param title is the caption that should be added on top of the plot. For
    ///              optimal visual results, it should preferably be composed of
    ///              ASCII chars only.
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't yet been recycled via
    ///             distribution_reset() or destroyed via
    ///             distribution_finalize().
    /// \param type is the kind of plot that is being drawn.
    UDIPE_NON_NULL_ARGS
    static void log_plot(udipe_log_level_t level,
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

        const plot_layout_t layout = plot_layout(dist,
                                                 type,
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
                               ordinate_range,
                               &layout,
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
                               ordinate_range,
                               &layout,
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

    UDIPE_NON_NULL_ARGS
    distribution_t distribution_build(distribution_builder_t* builder) {
        trace("Extracting the distribution from the builder...");
        distribution_t dist = builder->inner;
        distribution_poison(&builder->inner);

        trace("Ensuring the distribution can be sampled...");
        ensure_ge(dist.num_bins, (size_t)1);

        trace("Turning value counts into end ranks...");
        distribution_layout_t layout = distribution_layout(&dist);
        size_t end_rank = 0;
        for (size_t bin = 0; bin < dist.num_bins; ++bin) {
            end_rank += layout.counts[bin];
            layout.end_ranks[bin] = end_rank;
        }
        return dist;
    }

    UDIPE_NON_NULL_ARGS
    void distribution_discard(distribution_builder_t* builder) {
        distribution_finalize(&builder->inner);
    }

    UDIPE_NON_NULL_ARGS
    distribution_t distribution_sub(distribution_builder_t* builder,
                                    const distribution_t* left,
                                    const distribution_t* right) {
        // To avoid "amplifying" outliers by using multiple copies, we iterate
        // over the shortest distribution and sample from the longest one
        assert(builder->inner.num_bins == 0);
        const distribution_t* shorter;
        const distribution_t* longer;
        int64_t diff_sign;
        if (distribution_len(left) <= distribution_len(right)) {
            trace("Left distribution is shorter, will iterate over left and sample from right.");
            shorter = left;
            longer = right;
            diff_sign = +1;
        } else {
            trace("Right distribution is shorter, will iterate over right and sample from left.");
            shorter = right;
            longer = left;
            diff_sign = -1;
        }

        const distribution_layout_t short_layout = distribution_layout(shorter);
        const size_t short_bins = shorter->num_bins;
        tracef("Iterating over the %zu bins of the shorter distribution...",
               short_bins);
        size_t prev_short_end_rank = 0;
        for (size_t short_pos = 0; short_pos < short_bins; ++short_pos) {
            const int64_t short_value = short_layout.sorted_values[short_pos];
            const size_t short_end_rank = short_layout.end_ranks[short_pos];
            const size_t short_count = short_end_rank - prev_short_end_rank;
            tracef("- Bin #%zu contains %zu occurences of value %zd.",
                   short_pos, short_count, short_value);
            for (size_t long_sample = 0; long_sample < short_count; ++long_sample) {
                const int64_t diff = short_value - distribution_sample(longer);
                tracef("  * Random short-long difference is %zd.", diff);
                const int64_t signed_diff = diff_sign * diff;
                tracef("  * Random left-right difference is %zd.", signed_diff);
                distribution_insert(builder, signed_diff);
            }
            prev_short_end_rank = short_end_rank;
        }
        return distribution_build(builder);
    }

    UDIPE_NON_NULL_ARGS
    distribution_t distribution_scaled_div(distribution_builder_t* builder,
                                           const distribution_t* num,
                                           int64_t factor,
                                           const distribution_t* denom) {
        // To avoid "amplifying" outliers by using multiple copies, we iterate
        // over the shortest distribution and sample from the longest one
        assert(builder->inner.num_bins == 0);
        if (distribution_len(num) <= distribution_len(num)) {
            trace("Numerator distribution is shorter, will iterate over num and sample from denom.");
            const distribution_layout_t num_layout = distribution_layout(num);
            const size_t num_bins = num->num_bins;
            tracef("Iterating over the %zu bins of the numerator distribution...",
                   num_bins);
            size_t prev_end_rank = 0;
            for (size_t num_pos = 0; num_pos < num_bins; ++num_pos) {
                const int64_t num_value = num_layout.sorted_values[num_pos];
                const size_t curr_end_rank = num_layout.end_ranks[num_pos];
                const size_t num_count = curr_end_rank - prev_end_rank;
                tracef("- Numerator bin #%zu contains %zu occurences of value %zd.",
                       num_pos, num_count, num_value);
                for (size_t denom_sample = 0; denom_sample < num_count; ++denom_sample) {
                    const int64_t denom_value = distribution_sample(denom);
                    tracef("  * Sampled random denominator value %zd.", denom_value);
                    const int64_t scaled_ratio = num_value * factor / denom_value;
                    tracef("  * Scaled ratio sample is %zd.", scaled_ratio);
                    distribution_insert(builder, scaled_ratio);
                }
                prev_end_rank = curr_end_rank;
            }
            return distribution_build(builder);
        } else {
            trace("Denominator distribution is shorter, will iterate over denom and sample from num.");
            const distribution_layout_t denom_layout = distribution_layout(denom);
            const size_t denom_bins = denom->num_bins;
            tracef("Iterating over the %zu bins of the denominator distribution...",
                   denom_bins);
            size_t prev_end_rank = 0;
            for (size_t denom_pos = 0; denom_pos < denom_bins; ++denom_pos) {
                const int64_t denom_value = denom_layout.sorted_values[denom_pos];
                const size_t curr_end_rank = denom_layout.end_ranks[denom_pos];
                const size_t denom_count = curr_end_rank - prev_end_rank;
                tracef("- Denominator bin #%zu contains %zu occurences of value %zd.",
                       denom_pos, denom_count, denom_value);
                for (size_t num_sample = 0; num_sample < denom_count; ++num_sample) {
                    const int64_t num_value = distribution_sample(num);
                    tracef("  * Sampled random numerator value %zd.", num_value);
                    const int64_t scaled_ratio = num_value * factor / denom_value;
                    tracef("  * Scaled ratio sample is %zd.", scaled_ratio);
                    distribution_insert(builder, scaled_ratio);
                }
                prev_end_rank = curr_end_rank;
            }
            return distribution_build(builder);
        }
    }

    UDIPE_NON_NULL_ARGS
    distribution_builder_t distribution_reset(distribution_t* dist) {
        tracef("Resetting storage at location %p...", dist->allocation);
        distribution_builder_t result = (distribution_builder_t){
            .inner = (distribution_t){
                .allocation = dist->allocation,
                .num_bins = 0,
                .capacity = dist->capacity
            }
        };

        trace("Poisoning distribution state to detect invalid usage...");
        distribution_poison(dist);
        return result;
    }

    UDIPE_NON_NULL_ARGS
    void distribution_finalize(distribution_t* dist) {
        debugf("Liberating storage at location %p...", dist->allocation);
        free(dist->allocation);

        trace("Poisoning distribution state to detect invalid usage...");
        distribution_poison(dist);
    }

    stats_analyzer_t stats_analyzer_initialize(float confidence_f) {
        debug("Checking analysis parameters...");
        double confidence = (double)confidence_f;
        ensure_gt(confidence, 0.0);
        ensure_lt(confidence, 100.0);

        debug("Determining storage needs...");
        size_t num_medians = ceil((double)(2*NUM_EDGE_MEASUREMENTS)
                                  / (1.0-0.01*confidence));
        if ((num_medians % 2) == 0) num_medians += 1;

        debug("Allocating storage...");
        size_t medians_size = num_medians * sizeof(int64_t);
        int64_t* medians = realtime_allocate(medians_size);

        debug("Finishing setup...");
        double low_quantile = (1.0 - 0.01*confidence) / 2.0;
        double high_quantile = 1.0 - low_quantile;
        return (stats_analyzer_t){
            .medians = medians,
            .num_medians = num_medians,
            .low_idx = (size_t)(low_quantile * num_medians),
            .center_idx = num_medians / 2,
            .high_idx = (size_t)(high_quantile * num_medians)
        };
    }

    UDIPE_NON_NULL_ARGS
    stats_t stats_analyze(stats_analyzer_t* analyzer,
                          const distribution_t* dist) {
        trace("Computing medians...");
        int64_t median_samples[NUM_MEDIAN_SAMPLES];
        for (size_t median = 0; median < analyzer->num_medians; ++median) {
            tracef("- Computing medians[%zu]...", median);
            for (size_t sample = 0; sample < NUM_MEDIAN_SAMPLES; ++sample) {
                const int64_t value = distribution_sample(dist);
                tracef("  * Inserting sample %zd...", value);
                ptrdiff_t prev;
                for (prev = sample - 1; prev >= 0; --prev) {
                    int64_t pivot = median_samples[prev];
                    tracef("    - Checking median_samples[%zd] = %zd...",
                           prev, pivot);
                    if (pivot > value) {
                        trace("    - Too high, shift that up to make room.");
                        median_samples[prev + 1] = median_samples[prev];
                        continue;
                    } else {
                        trace("    - Small enough, value goes after that.");
                        break;
                    }
                }
                tracef("  * Sample inserted at median_samples[%zd].", prev + 1);
                median_samples[prev + 1] = value;
            }
            analyzer->medians[median] = median_samples[NUM_MEDIAN_SAMPLES / 2];
            tracef("  * medians[%zu] is therefore %zd.",
                   median, analyzer->medians[median]);
        }

        trace("Computing result...");
        qsort(analyzer->medians,
              analyzer->num_medians,
              sizeof(int64_t),
              compare_i64);
        return (stats_t){
            .center = analyzer->medians[analyzer->center_idx],
            .low = analyzer->medians[analyzer->low_idx],
            .high = analyzer->medians[analyzer->high_idx]
        };
    }

    UDIPE_NON_NULL_ARGS
    void stats_analyzer_finalize(stats_analyzer_t* analyzer) {
        debug("Liberating storage...");
        realtime_liberate(analyzer->medians,
                          analyzer->num_medians * sizeof(int64_t));

        debug("Poisoining analyzer state...");
        analyzer->medians = NULL;
        analyzer->num_medians = 0;
        analyzer->center_idx = SIZE_MAX;
        analyzer->low_idx = SIZE_MAX;
        analyzer->high_idx = SIZE_MAX;
    }


    UDIPE_NON_NULL_ARGS
    void empty_loop(void* context) {
        size_t num_iters = *((const size_t*)context);
        // Ensures that all loop lengths get the same codegen
        UDIPE_ASSUME_ACCESSED(num_iters);
        for (size_t iter = 0; iter < num_iters; ++iter) {
            // This is ASSUME_ACCESSED and not ASSUME_READ because with
            // ASSUME_READ the compiler can unroll the loop and this will reduce
            // timing reproducibility with respect to the pure dependency chain
            // of a non-unrolled loop.
            UDIPE_ASSUME_ACCESSED(iter);
        }
    }


    /// Log statistics from the calibration process
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param level is the verbosity level at which this log will be emitted
    /// \param header is a string that will be prepended to the log
    /// \param stats is \ref stats_t from the calibration process that will
    ///              be printed out
    /// \param unit is a string that spells out the measurement unit of
    ///             `stats`
    #define log_calibration_stats(level, header, stats, unit)  \
        do {  \
            stats_t udipe_duration = (stats);  \
            udipe_logf((level),  \
                       "%s: %zd %s with %g%% CI [%zd; %zd].",  \
                       (header),  \
                       udipe_duration.center,  \
                       (unit),  \
                       CONFIDENCE,  \
                       udipe_duration.low,  \
                       udipe_duration.high);  \
        } while(false)

    /// Compute the relative uncertainty from some \ref stats_t
    ///
    /// \param stats is \ref stats_t that directly or indirectly derive from
    ///              some measurements.
    /// \returns the associated statistical uncertainty in percentage points
    static inline double relative_uncertainty(stats_t stats) {
        return (double)(stats.high - stats.low) / stats.center * 100.0;
    }

    /// Log per-iteration statistics from the calibration process
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param level is the verbosity level at which this log will be emitted
    /// \param stats is \ref stats_t from the calibration process that will
    ///              be printed out
    /// \param unit is a string that spells out the measurement unit of
    ///             `stats`
    #define log_iteration_stats(level, bullet, stats, num_iters, unit)  \
        do {  \
            const stats_t udipe_stats = (stats);  \
            const size_t udipe_num_iters = (num_iters);  \
            const double udipe_center = (double)udipe_stats.center / udipe_num_iters;  \
            const double udipe_low = (double)udipe_stats.low / udipe_num_iters;  \
            const double udipe_high = (double)udipe_stats.high / udipe_num_iters;  \
            const double udipe_spread = udipe_high - udipe_low;  \
            const int udipe_stats_decimals = ceil(-log10(udipe_spread));  \
            const double udipe_uncertainty = relative_uncertainty(udipe_stats);  \
            int udipe_uncertainty_decimals = ceil(-log10(udipe_uncertainty)) + 1;  \
            if (udipe_uncertainty_decimals < 0) udipe_uncertainty_decimals = 0;  \
            udipe_logf((level),  \
                       "%s That's %.*f %s/iter with %g%% CI [%.*f; %.*f] (%.*f%% uncertainty).",  \
                       (bullet),  \
                       udipe_stats_decimals,  \
                       udipe_center,  \
                       (unit),  \
                       CONFIDENCE,  \
                       udipe_stats_decimals,  \
                       udipe_low,  \
                       udipe_stats_decimals,  \
                       udipe_high,  \
                       udipe_uncertainty_decimals,  \
                       udipe_uncertainty);  \
        } while(false)

    UDIPE_NON_NULL_ARGS
    distribution_t compute_duration_distribution(
        int64_t (*compute_duration)(void* /* context */,
                                    size_t /* run */),
        void* context,
        size_t num_runs,
        distribution_builder_t* result_builder
    ) {
        ensure_ge(num_runs, TEMPORAL_WINDOW);

        trace("Setting up statistics...");
        size_t num_normal_runs = 0;
        size_t num_initially_rejected = 0;
        size_t num_reclassified = 0;
        distribution_builder_t reject_builder;
        distribution_builder_t reclassify_builder;
        if (log_enabled(UDIPE_DEBUG)) {
            reject_builder = distribution_initialize();
            reclassify_builder = distribution_initialize();
        }

        trace("Seeding temporal outlier filter...");
        int64_t initial_window[TEMPORAL_WINDOW];
        for (size_t run = 0; run < TEMPORAL_WINDOW; ++run) {
            initial_window[run] = compute_duration(context, run);
        }
        temporal_filter_t filter = temporal_filter_initialize(initial_window);

        trace("Collecting temporally filtered durations...");
        TEMPORAL_FILTER_FOREACH_NORMAL(&filter, duration, {
            distribution_insert(result_builder, duration);
            ++num_normal_runs;
        });
        // There can be at most one outlier per input window
        ensure_le(TEMPORAL_WINDOW - num_normal_runs, (uint16_t)1);
        //
        for (size_t run = TEMPORAL_WINDOW; run < num_runs; ++run) {
            const int64_t duration = compute_duration(context, run);
            const temporal_filter_result_t result =
                temporal_filter_apply(&filter, duration);
            if (result.previous_not_outlier) {
                tracef("- Reclassified previous outlier duration %zd as non-outlier",
                       result.previous_input);
                for (size_t pos = 0; pos < TEMPORAL_WINDOW; ++pos) {
                    const size_t idx = (filter.next_idx + pos) % TEMPORAL_WINDOW;
                    const size_t age = TEMPORAL_WINDOW - 1 - pos;
                    tracef("  * duration[%zu aka -%zu] is %zd",
                           run - age,
                           age,
                           filter.window[idx]);
                }
                distribution_insert(result_builder, result.previous_input);
                ++num_normal_runs;
                if (log_enabled(UDIPE_DEBUG)) {
                    distribution_insert(&reclassify_builder, result.previous_input);
                    ++num_reclassified;
                }
            }
            if (!result.current_is_outlier) {
                distribution_insert(result_builder, duration);
                ++num_normal_runs;
            } else if (log_enabled(UDIPE_DEBUG)) {
                distribution_insert(&reject_builder, duration);
                ++num_initially_rejected;
            }
            ensure_le(num_normal_runs, run + 1);
        }

        trace("Reporting results...");
        distribution_t result = distribution_build(result_builder);
        if (log_enabled(UDIPE_DEBUG)) {
            if (num_initially_rejected > 0) {
                distribution_t reject = distribution_build(&reject_builder);
                distribution_log(&reject,
                                 UDIPE_DEBUG,
                                 "Durations initially rejected as outliers");
                distribution_finalize(&reject);
            } else {
                distribution_discard(&reject_builder);
            }
            //
            if (num_reclassified > 0) {
                distribution_t reclassify = distribution_build(&reclassify_builder);
                distribution_log(&reclassify,
                                 UDIPE_DEBUG,
                                 "Durations later reclassified to non-outlier");
                debugf("Reclassified %zu/%zu durations from outlier to normal.",
                       num_reclassified, num_runs);
                distribution_finalize(&reclassify);
            } else {
                distribution_discard(&reclassify_builder);
            }
            //
            if (num_normal_runs < num_runs) {
                const size_t num_outliers = num_runs - num_normal_runs;
                debugf("Eventually rejected %zu/%zu durations.",
                       num_outliers, num_runs);
            }
            distribution_log(&result,
                             UDIPE_DEBUG,
                             "Accepted durations");
        }
        assert(distribution_len(&result) == num_runs);
        return result;
    }


    UDIPE_NON_NULL_ARGS
    os_clock_t os_clock_initialize(stats_analyzer_t* analyzer) {
        // Zero out all clock fields initially
        //
        // This is a valid (if incorrect) value for some fields but not all of
        // them. We will take care of the missing fields later on.
        os_clock_t clock = { 0 };

        #ifdef _WIN32
            debug("Obtaining Windows performance counter frequency...");
            clock.win32_frequency = QueryPerformanceFrequency().QuadPart;
        #endif

        debug("Allocating timestamp buffer and duration distribution...");
        size_t max_runs = NUM_RUNS_OFFSET_OS;
        if (max_runs < NUM_RUNS_SHORTEST_LOOP) max_runs = NUM_RUNS_SHORTEST_LOOP;
        if (max_runs < NUM_RUNS_BEST_LOOP_OS) max_runs = NUM_RUNS_BEST_LOOP_OS;
        const size_t timestamps_size = (max_runs+1) * sizeof(os_timestamp_t);
        clock.timestamps = realtime_allocate(timestamps_size);
        clock.num_durations = max_runs;
        clock.builder = distribution_initialize();

        info("Bootstrapping clock offset to 0 ns...");
        distribution_insert(&clock.builder, 0);
        clock.offsets = distribution_build(&clock.builder);
        clock.builder = distribution_initialize();

        info("Measuring actual clock offset...");
        size_t num_iters = 0;
        distribution_t tmp_offsets = os_clock_measure(
            &clock,
            empty_loop,
            &num_iters,
            WARMUP_OFFSET_OS,
            NUM_RUNS_OFFSET_OS,
            &clock.builder
        );
        clock.builder = distribution_reset(&clock.offsets);
        clock.offsets = tmp_offsets;
        distribution_poison(&tmp_offsets);
        const stats_t offset_stats = stats_analyze(analyzer, &clock.offsets);
        log_calibration_stats(UDIPE_INFO, "- Clock offset", offset_stats, "ns");

        info("Deducing clock baseline...");
        distribution_t tmp_zeros = distribution_sub(&clock.builder,
                                                    &clock.offsets,
                                                    &clock.offsets);
        const stats_t zero_stats = stats_analyze(analyzer, &tmp_zeros);
        clock.builder = distribution_reset(&tmp_zeros);
        log_calibration_stats(UDIPE_INFO,
                              "- Baseline",
                              zero_stats,
                              "ns");

        info("Finding minimal measurable loop...");
        distribution_t loop_durations;
        stats_t loop_duration_stats;
        num_iters = 1;
        do {
            debugf("- Trying loop with %zu iteration(s)...", num_iters);
            loop_durations = os_clock_measure(
                &clock,
                empty_loop,
                &num_iters,
                WARMUP_SHORTEST_LOOP,
                NUM_RUNS_SHORTEST_LOOP,
                &clock.builder
            );
            loop_duration_stats = stats_analyze(analyzer, &loop_durations);
            log_calibration_stats(UDIPE_DEBUG,
                                  "  * Loop duration",
                                  loop_duration_stats,
                                  "ns");
            const signed_duration_ns_t loop_duration_spread =
                loop_duration_stats.high - loop_duration_stats.low;
            if (loop_duration_stats.low < 9*offset_stats.high) {
                debug("  * Clock contribution may still be >10%...");
            } else if(loop_duration_stats.low < 10*loop_duration_spread) {
                debug("  * Duration may still fluctuates by >10%...");
            } else {
                debug("  * That's long enough and stable enough.");
                clock.builder = distribution_initialize();
                break;
            }
            // If control reaches here, must still increase loop size
            num_iters *= 2;
            clock.builder = distribution_reset(&loop_durations);
        } while(true);
        infof("- Loops with >=%zu iterations have non-negligible duration.",
              num_iters);

        info("Finding optimal loop duration...");
        clock.best_empty_iters = num_iters;
        clock.best_empty_durations = loop_durations;
        distribution_poison(&loop_durations);
        clock.best_empty_stats = loop_duration_stats;
        const int64_t best_precision = loop_duration_stats.high - loop_duration_stats.low;
        double best_uncertainty = relative_uncertainty(loop_duration_stats);
        do {
            num_iters *= 2;
            debugf("- Trying loop with %zu iterations...", num_iters);
            loop_durations = os_clock_measure(
                &clock,
                empty_loop,
                &num_iters,
                WARMUP_BEST_LOOP,
                NUM_RUNS_BEST_LOOP_OS,
                &clock.builder
            );
            loop_duration_stats = stats_analyze(analyzer, &loop_durations);
            log_calibration_stats(UDIPE_DEBUG,
                                  "  * Loop duration",
                                  loop_duration_stats,
                                  "ns");
            log_iteration_stats(UDIPE_DEBUG,
                                "  *",
                                loop_duration_stats,
                                num_iters,
                                "ns");
            const double uncertainty = relative_uncertainty(loop_duration_stats);
            const int64_t precision = loop_duration_stats.high - loop_duration_stats.low;
            // In a regime of stable run timing precision, doubling the
            // iteration count should improve iteration timing uncertainty by
            // 2x. Ignore small improvements that don't justify a 2x longer run
            // duration, and thus fewer runs per unit of execution time...
            if (uncertainty < best_uncertainty/1.1) {
                debug("  * This is our new best loop. Can we do even better?");
                best_uncertainty = uncertainty;
                clock.best_empty_iters = num_iters;
                clock.builder = distribution_reset(&clock.best_empty_durations);
                clock.best_empty_durations = loop_durations;
                distribution_poison(&loop_durations);
                clock.best_empty_stats = loop_duration_stats;
                continue;
            } else if (precision <= 3*best_precision) {
                // ...but keep trying until the uncertainty degradation becomes
                // much worse than expected in a regime of stable iteration
                // timing uncertainty, in which case loop duration fluctuates 2x
                // more when loop iteration gets 2x higher.
                debug("  * That's not much better/worse, keep trying...");
                clock.builder = distribution_reset(&loop_durations);
                continue;
            } else {
                debug("  * Absolute precision degraded by >3x: time to stop!");
                clock.builder = distribution_reset(&loop_durations);
                break;
            }
        } while(true);
        infof("- Achieved optimal precision at %zu loop iterations.",
              clock.best_empty_iters);
        log_calibration_stats(UDIPE_INFO,
                              "- Best loop duration",
                              clock.best_empty_stats,
                              "ns");
        log_iteration_stats(UDIPE_INFO,
                            "-",
                            clock.best_empty_stats,
                            clock.best_empty_iters,
                            "ns");
        return clock;
    }

    /// compute_duration_distribution() callback used by os_clock_measure()
    ///
    /// `context` must be a pointer to the associated \ref os_clock_t.
    static inline int64_t compute_os_duration(void* context,
                                              size_t run) {
        os_clock_t* clock = (os_clock_t*)context;
        assert(run < clock->num_durations);
        return os_duration(clock,
                           clock->timestamps[run],
                           clock->timestamps[run+1]);
    }

    UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 6)
    distribution_t os_clock_measure(
        os_clock_t* clock,
        void (*workload)(void*),
        void* context,
        udipe_duration_ns_t warmup,
        size_t num_runs,
        distribution_builder_t* result_builder
    ) {
        if (num_runs > clock->num_durations) {
            trace("Reallocating storage from %zu to %zu durations...");
            realtime_liberate(
                clock->timestamps,
                (clock->num_durations+1) * sizeof(os_timestamp_t)
            );
            clock->num_durations = num_runs;
            clock->timestamps =
                realtime_allocate((num_runs+1) * sizeof(os_timestamp_t));
        }

        trace("Warming up...");
        os_timestamp_t* timestamps = clock->timestamps;
        udipe_duration_ns_t elapsed = 0;
        os_timestamp_t start = os_now();
        do {
            workload(context);
            os_timestamp_t now = os_now();
            elapsed = os_duration(clock, start, now);
        } while(elapsed < warmup);

        tracef("Performing %zu timed runs...", num_runs);
        timestamps[0] = os_now();
        for (size_t run = 0; run < num_runs; ++run) {
            UDIPE_ASSUME_READ(timestamps);
            workload(context);
            timestamps[run+1] = os_now();
        }
        UDIPE_ASSUME_READ(timestamps);

        trace("Computing duration distribution...");
        return compute_duration_distribution(compute_os_duration,
                                             (void*)clock,
                                             num_runs,
                                             result_builder);
    }

    UDIPE_NON_NULL_ARGS
    void os_clock_finalize(os_clock_t* clock) {
        debug("Liberating and poisoning timestamp storage...");
        realtime_liberate(clock->timestamps,
                          (clock->num_durations+1) * sizeof(os_timestamp_t));
        clock->timestamps = NULL;
        clock->num_durations = 0;

        debug("Destroying duration distributions...");
        distribution_finalize(&clock->offsets);
        distribution_finalize(&clock->best_empty_durations);
        distribution_finalize(&clock->builder.inner);

        debug("Poisoning the rest of the OS clock...");
        #ifdef _WIN32
            clock->win32_frequency = 0;
        #endif
        clock->best_empty_iters = SIZE_MAX;
        clock->best_empty_stats = (stats_t){
            .low = INT64_MIN,
            .center = INT64_MIN,
            .high = INT64_MIN
        };
    }


    #ifdef X86_64

        UDIPE_NON_NULL_ARGS
        x86_clock_t
        x86_clock_initialize(os_clock_t* os,
                             stats_analyzer_t* analyzer) {
            // Zero out all clock fields initially
            //
            // This is a valid (if incorrect) value for some fields but not all
            // of them. We will take care of the missing fields later on.
            x86_clock_t clock = { 0 };

            debug("Allocating timestamp and duration distribution...");
            size_t max_runs = NUM_RUNS_BEST_LOOP_X86;
            if (max_runs < NUM_RUNS_OFFSET_X86) max_runs = NUM_RUNS_OFFSET_X86;
            const size_t instants_size = 2 * max_runs * sizeof(x86_instant);
            clock.instants = realtime_allocate(instants_size);
            clock.num_durations = max_runs;
            distribution_builder_t builder = distribution_initialize();

            info("Bootstrapping clock offset to 0 ticks...");
            distribution_insert(&builder, 0);
            clock.offsets = distribution_build(&builder);
            builder = distribution_initialize();

            // This should happen as soon as possible to reduce the risk of CPU
            // clock frequency changes, which would degrade the quality of our
            // TSC frequency calibration
            //
            // TODO: Investigate paired benchmarking techniques as a more robust
            //       alternative to reducing the delay between these two
            //       measurements. The general idea is to alternatively measure
            //       durations with the OS and TSC clocks, use pairs of raw
            //       duration data points from each clock to compute frequency
            //       samples, and compute statistics over these frequency
            //       samples. This way we are using data that was acquired in
            //       similar system configurations, so even if the system
            //       configuration changes over time, the results remain stable.
            info("Measuring optimal loop again with the TSC...");
            size_t best_empty_iters = os->best_empty_iters;
            distribution_t raw_empty_ticks = x86_clock_measure(
                &clock,
                empty_loop,
                &best_empty_iters,
                WARMUP_BEST_LOOP,
                NUM_RUNS_BEST_LOOP_X86,
                &builder
            );
            log_calibration_stats(UDIPE_INFO,
                                  "- Offset-biased best loop",
                                  stats_analyze(analyzer, &raw_empty_ticks),
                                  "ticks");

            info("Measuring clock offset...");
            builder = distribution_initialize();
            size_t empty_loop_iters = 0;
            distribution_t tmp_offsets = x86_clock_measure(
                &clock,
                empty_loop,
                &empty_loop_iters,
                WARMUP_OFFSET_X86,
                NUM_RUNS_OFFSET_X86,
                &builder
            );
            builder = distribution_reset(&clock.offsets);
            clock.offsets = tmp_offsets;
            distribution_poison(&tmp_offsets);
            log_calibration_stats(UDIPE_INFO,
                                  "- Clock offset",
                                  stats_analyze(analyzer, &clock.offsets),
                                  "ticks");

            info("Deducing clock baseline...");
            distribution_t tmp_zeros = distribution_sub(&builder,
                                                        &clock.offsets,
                                                        &clock.offsets);
            const stats_t zero_stats = stats_analyze(analyzer, &tmp_zeros);
            builder = distribution_reset(&tmp_zeros);
            log_calibration_stats(UDIPE_INFO,
                                  "- Baseline",
                                  zero_stats,
                                  "ticks");

            debug("Applying offset correction to best loop duration...");
            distribution_t corrected_empty_ticks = distribution_sub(
                &builder,
                &raw_empty_ticks,
                &clock.offsets
            );
            builder = distribution_reset(&raw_empty_ticks);
            clock.best_empty_stats = stats_analyze(analyzer,
                                                   &corrected_empty_ticks);
            log_calibration_stats(UDIPE_DEBUG,
                                  "- Offset-corrected best loop",
                                  clock.best_empty_stats,
                                  "ticks");
            log_iteration_stats(UDIPE_DEBUG,
                                "-",
                                clock.best_empty_stats,
                                os->best_empty_iters,
                                "ticks");

            info("Deducing TSC tick frequency...");
            clock.frequencies = distribution_scaled_div(
                &builder,
                &corrected_empty_ticks,
                UDIPE_SECOND,
                &os->best_empty_durations
            );
            // `builder` cannot be used after this point
            log_calibration_stats(UDIPE_INFO,
                                  "- TSC frequency",
                                  stats_analyze(analyzer, &clock.frequencies),
                                  "ticks/sec");

            debug("Deducing best loop duration...");
            const stats_t best_empty_duration = x86_duration(
                &clock,
                &os->builder,
                &corrected_empty_ticks,
                analyzer
            );
            log_calibration_stats(UDIPE_DEBUG,
                                  "- Best loop duration",
                                  best_empty_duration,
                                  "ns");
            log_iteration_stats(UDIPE_DEBUG,
                                "-",
                                best_empty_duration,
                                os->best_empty_iters,
                                "ns");
            return clock;
        }

        /// compute_duration_distribution() context used by x86_clock_measure()
        ///
        typedef struct x86_measure_context_s {
            x86_clock_t* clock;  ///< x86 clock used for the measurement
            size_t num_runs;  ///< Number of benchmark runs
        } x86_measure_context_t;

        /// compute_duration_distribution() callback used by x86_clock_measure()
        ///
        /// `context` must be a pointer to the associated \ref
        /// x86_measure_context_t.
        static inline int64_t compute_x86_duration(void* context,
                                                   size_t run) {
            x86_measure_context_t* measure = (x86_measure_context_t*)context;
            x86_clock_t* clock = measure->clock;
            assert(run < measure->num_runs);
            assert(measure->num_runs < clock->num_durations);
            x86_instant* starts = clock->instants;
            x86_instant* ends = starts + measure->num_runs;
            const int64_t raw_ticks = ends[run] - starts[run];
            return raw_ticks - distribution_sample(&clock->offsets);
        }

        UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 6)
        distribution_t x86_clock_measure(
            x86_clock_t* xclock,
            void (*workload)(void*),
            void* context,
            udipe_duration_ns_t warmup,
            size_t num_runs,
            distribution_builder_t* result_builder
        ) {
            if (num_runs > xclock->num_durations) {
                trace("Reallocating storage from %zu to %zu durations...");
                realtime_liberate(
                    xclock->instants,
                    2 * xclock->num_durations * sizeof(x86_instant)
                );
                xclock->num_durations = num_runs;
                xclock->instants =
                    realtime_allocate(2 * num_runs * sizeof(x86_instant));
            }

            trace("Setting up measurement...");
            x86_instant* starts = xclock->instants;
            x86_instant* ends = xclock->instants + num_runs;
            const bool strict = false;
            x86_timestamp_t timestamp = x86_timer_start(strict);
            const x86_cpu_id initial_cpu_id = timestamp.cpu_id;

            trace("Warming up...");
            udipe_duration_ns_t elapsed = 0;
            clock_t start = clock();
            do {
                timestamp = x86_timer_start(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                UDIPE_ASSUME_READ(timestamp.ticks);

                workload(context);

                timestamp = x86_timer_end(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                UDIPE_ASSUME_READ(timestamp.ticks);

                clock_t now = clock();
                elapsed = (udipe_duration_ns_t)(now - start)
                          * UDIPE_SECOND / CLOCKS_PER_SEC;
            } while(elapsed < warmup);

            tracef("Performing %zu timed runs...", num_runs);
            for (size_t run = 0; run < num_runs; ++run) {
                timestamp = x86_timer_start(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                starts[run] = timestamp.ticks;
                UDIPE_ASSUME_READ(starts);

                workload(context);

                timestamp = x86_timer_end(strict);
                assert(timestamp.cpu_id == initial_cpu_id);
                ends[run] = timestamp.ticks;
                UDIPE_ASSUME_READ(ends);
            }

            trace("Computing duration distribution...");
            x86_measure_context_t measure_context = {
                .clock = xclock,
                .num_runs = num_runs
            };
            return compute_duration_distribution(compute_x86_duration,
                                                 (void*)&measure_context,
                                                 num_runs,
                                                 result_builder);
        }

        UDIPE_NON_NULL_ARGS
        stats_t x86_duration(x86_clock_t* clock,
                             distribution_builder_t* tmp_builder,
                             const distribution_t* ticks,
                             stats_analyzer_t* analyzer) {
            distribution_t tmp_durations =
                distribution_scaled_div(tmp_builder,
                                        ticks,
                                        UDIPE_SECOND,
                                        &clock->frequencies);
            const stats_t result = stats_analyze(analyzer, &tmp_durations);
            *tmp_builder = distribution_reset(&tmp_durations);
            return result;
        }

        UDIPE_NON_NULL_ARGS
        void x86_clock_finalize(x86_clock_t* clock) {
            debug("Liberating and poisoning timestamp storage...");
            realtime_liberate(clock->instants,
                              2 * clock->num_durations * sizeof(x86_instant));
            clock->instants = NULL;
            clock->num_durations = 0;

            debug("Destroying offset and frequency distributions...");
            distribution_finalize(&clock->offsets);
            distribution_finalize(&clock->frequencies);

            debug("Poisoning the now-invalid TSC clock...");
            clock->best_empty_stats = (stats_t){
                .low = INT64_MIN,
                .center = INT64_MIN,
                .high = INT64_MIN
            };
        }

    #endif  // X86_64


    benchmark_clock_t benchmark_clock_initialize() {
        // Zero out all clock fields initially
        //
        // This is a valid (if incorrect) value for some fields but not all of
        // them. We will take care of the missing fields later on.
        benchmark_clock_t clock = { 0 };

        debug("Setting up statistical analysis...");
        clock.analyzer = stats_analyzer_initialize(CONFIDENCE);

        info("Setting up the OS clock...");
        clock.os = os_clock_initialize(&clock.analyzer);

        #ifdef X86_64
            info("Setting up the TSC clock...");
            clock.x86 = x86_clock_initialize(&clock.os, &clock.analyzer);
        #endif
        return clock;
    }

    UDIPE_NON_NULL_ARGS
    void benchmark_clock_recalibrate(benchmark_clock_t* clock) {
        // TODO: Check if clock calibration still seems correct, recalibrate if
        //       needed.
        // TODO: This should probably be implemented by implementing recalibrate
        //       for the x86 and OS clocks, then calling these here.
        error("Not implemented yet!");
        exit(EXIT_FAILURE);
    }

    UDIPE_NON_NULL_ARGS
    void benchmark_clock_finalize(benchmark_clock_t* clock) {
        debug("Liberating the statistical analyzer...");
        stats_analyzer_finalize(&clock->analyzer);

        #ifdef X86_64
            debug("Liberating the TSC clock...");
            x86_clock_finalize(&clock->x86);
        #endif

        debug("Liberating the OS clock...");
        os_clock_finalize(&clock->os);
    }


    DEFINE_PUBLIC
    UDIPE_NON_NULL_ARGS
    UDIPE_NON_NULL_RESULT
    udipe_benchmark_t* udipe_benchmark_initialize(int argc, char *argv[]) {
        // Set up logging
        logger_t logger = logger_initialize((udipe_log_config_t){ 0 });
        udipe_benchmark_t* benchmark;
        with_logger(&logger, {
            debug("Setting up benchmark harness...");
            benchmark =
                (udipe_benchmark_t*)realtime_allocate(sizeof(udipe_benchmark_t));
            memset(benchmark, 0, sizeof(udipe_benchmark_t));
            benchmark->logger = logger;

            // Warn about bad build/runtime configurations
            #ifndef NDEBUG
                warn("You are running micro-benchmarks on a Debug build. "
                     "This will bias measurements!");
            #else
                if (benchmark->logger.min_level <= UDIPE_DEBUG) {
                    warn("You are running micro-benchmarks with DEBUG/TRACE "
                         "logging enabled. This will bias measurements!");
                }
            #endif

            debug("Setting up benchmark name filter...");
            ensure_le(argc, 2);
            const char* filter_key = (argc == 2) ? argv[1] : "";
            benchmark->filter = name_filter_initialize(filter_key);

            debug("Setting up the hwloc topology...");
            exit_on_negative(hwloc_topology_init(&benchmark->topology),
                             "Failed to allocate the hwloc hopology!");
            exit_on_negative(hwloc_topology_load(benchmark->topology),
                             "Failed to build the hwloc hopology!");

            debug("Pinning the benchmark timing thread...");
            benchmark->timing_cpuset = hwloc_bitmap_alloc();
            exit_on_null(benchmark->timing_cpuset,
                         "Failed to allocate timing thread cpuset");
            exit_on_negative(
                hwloc_get_last_cpu_location(benchmark->topology,
                                            benchmark->timing_cpuset,
                                            HWLOC_CPUBIND_THREAD),
                "Failed to query timing thread cpuset"
            );
            exit_on_negative(hwloc_set_cpubind(benchmark->topology,
                                               benchmark->timing_cpuset,
                                               HWLOC_CPUBIND_THREAD
                                               | HWLOC_CPUBIND_STRICT),
                             "Failed to pin the timing thread");

            // Set up the benchmark clock
            benchmark->clock = benchmark_clock_initialize();
        });
        return benchmark;
    }

    DEFINE_PUBLIC
    UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 3)
    bool udipe_benchmark_run(udipe_benchmark_t* benchmark,
                             const char name[],
                             udipe_benchmark_runnable_t runnable,
                             void* context) {
        bool name_matches;
        with_logger(&benchmark->logger, {
            name_matches = name_filter_matches(benchmark->filter, name);
            if (name_matches) {
                trace("Pinning the benchmark timing thread...");
                exit_on_negative(hwloc_set_cpubind(benchmark->topology,
                                                   benchmark->timing_cpuset,
                                                   HWLOC_CPUBIND_THREAD
                                                   | HWLOC_CPUBIND_STRICT),
                                 "Failed to pin benchmark timing thread");

                tracef("Running benchmark \"%s\"...", name);
                runnable(context, benchmark);

                trace("Recalibrating benchmark clock...");
                benchmark_clock_recalibrate(&benchmark->clock);
            }
        });
        return name_matches;
    }

    DEFINE_PUBLIC
    UDIPE_NON_NULL_ARGS
    void udipe_benchmark_finalize(udipe_benchmark_t** benchmark) {
        logger_t logger = (*benchmark)->logger;
        with_logger(&logger, {
            info("All benchmarks executed successfully!");

            debug("Finalizing the benchmark clock...");
            benchmark_clock_finalize(&(*benchmark)->clock);

            debug("Freeing and poisoning the timing thread cpuset...");
            hwloc_bitmap_free((*benchmark)->timing_cpuset);
            (*benchmark)->timing_cpuset = NULL;

            debug("Destroying and poisoning the hwloc topology...");
            hwloc_topology_destroy((*benchmark)->topology);
            (*benchmark)->topology = NULL;

            debug("Finalizing the benchmark name filter...");
            name_filter_finalize(&(*benchmark)->filter);

            debug("Liberating and poisoning the benchmark...");
            realtime_liberate(*benchmark, sizeof(udipe_benchmark_t));
            *benchmark = NULL;

            debug("Finalizing the logger...");
        });
        logger_finalize(&logger);
    }


    DEFINE_PUBLIC void udipe_micro_benchmarks(udipe_benchmark_t* benchmark) {
        // Microbenchmarks are ordered such that a piece of code is
        // benchmarked before other pieces of code that may depend on it
        // TODO: UDIPE_BENCHMARK(benchmark, xyz_micro_benchmarks, NULL);
    }


    #ifdef UDIPE_BUILD_TESTS

        /// Test distribution_builder_t and distribution_t
        ///
        static void test_distibution() {
            trace("Setting up a distribution...");
            distribution_builder_t builder = distribution_initialize();
            const void* const initial_allocation = builder.inner.allocation;
            const size_t initial_capacity = builder.inner.capacity;
            ensure_ne(initial_allocation, NULL);
            ensure_eq(builder.inner.num_bins, (size_t)0);
            ensure_ge(initial_capacity, (size_t)5);

            trace("Checking initial layout");
            const distribution_layout_t initial_layout =
                distribution_layout(&builder.inner);
            ensure_ne((void*)initial_layout.sorted_values, NULL);
            ensure_ne((void*)initial_layout.counts, NULL);
            const size_t values_size =
                (char*)initial_layout.counts
                    - (char*)initial_layout.sorted_values;
            ensure_eq(values_size, initial_capacity * sizeof(int64_t));

            assert(RAND_MAX <= INT64_MAX);
            const int64_t value3 = rand() - RAND_MAX / 2;
            tracef("Inserting value3 = %zd for the first time...", value3);
            distribution_insert(&builder, value3);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)1);
            ensure_eq(builder.inner.capacity, initial_capacity);
            distribution_layout_t layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value3);
            ensure_eq(layout.counts[0], (size_t)1);

            trace("Inserting value3 again...");
            distribution_insert(&builder, value3);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)1);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value3);
            ensure_eq(layout.counts[0], (size_t)2);

            const int64_t value5 = value3 + 2 + rand() % (INT64_MAX - value3 - 1);
            tracef("Inserting value5 = %zd for the first time...", value5);
            distribution_insert(&builder, value5);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)2);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value3);
            ensure_eq(layout.counts[0], (size_t)2);
            ensure_eq(layout.sorted_values[1], value5);
            ensure_eq(layout.counts[1], (size_t)1);

            trace("Inserting value5 again two times...");
            distribution_insert(&builder, value5);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)2);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value3);
            ensure_eq(layout.counts[0], (size_t)2);
            ensure_eq(layout.sorted_values[1], value5);
            ensure_eq(layout.counts[1], (size_t)2);
            //
            distribution_insert(&builder, value5);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)2);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value3);
            ensure_eq(layout.counts[0], (size_t)2);
            ensure_eq(layout.sorted_values[1], value5);
            ensure_eq(layout.counts[1], (size_t)3);

            const int64_t value1 = value3 - 2 - rand() % (value3 - 1 - INT64_MIN);
            tracef("Inserting value1 = %zd for the first time...", value1);
            distribution_insert(&builder, value1);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)3);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)1);
            ensure_eq(layout.sorted_values[1], value3);
            ensure_eq(layout.counts[1], (size_t)2);
            ensure_eq(layout.sorted_values[2], value5);
            ensure_eq(layout.counts[2], (size_t)3);

            trace("Inserting value1 again three times...");
            distribution_insert(&builder, value1);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)3);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)2);
            ensure_eq(layout.sorted_values[1], value3);
            ensure_eq(layout.counts[1], (size_t)2);
            ensure_eq(layout.sorted_values[2], value5);
            ensure_eq(layout.counts[2], (size_t)3);
            //
            distribution_insert(&builder, value1);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)3);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)3);
            ensure_eq(layout.sorted_values[1], value3);
            ensure_eq(layout.counts[1], (size_t)2);
            ensure_eq(layout.sorted_values[2], value5);
            ensure_eq(layout.counts[2], (size_t)3);
            //
            distribution_insert(&builder, value1);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)3);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value3);
            ensure_eq(layout.counts[1], (size_t)2);
            ensure_eq(layout.sorted_values[2], value5);
            ensure_eq(layout.counts[2], (size_t)3);

            const int64_t value2 = value1 + 1 + rand() % (value3 - value1 - 1);
            tracef("Inserting value2 = %zd for the first time...", value2);
            distribution_insert(&builder, value2);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)4);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts,
                      (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value2);
            ensure_eq(layout.counts[1], (size_t)1);
            ensure_eq(layout.sorted_values[2], value3);
            ensure_eq(layout.counts[2], (size_t)2);
            ensure_eq(layout.sorted_values[3], value5);
            ensure_eq(layout.counts[3], (size_t)3);

            trace("Inserting value2 again two times...");
            distribution_insert(&builder, value2);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)4);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts,
                      (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value2);
            ensure_eq(layout.counts[1], (size_t)2);
            ensure_eq(layout.sorted_values[2], value3);
            ensure_eq(layout.counts[2], (size_t)2);
            ensure_eq(layout.sorted_values[3], value5);
            ensure_eq(layout.counts[3], (size_t)3);
            //
            distribution_insert(&builder, value2);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)4);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value2);
            ensure_eq(layout.counts[1], (size_t)3);
            ensure_eq(layout.sorted_values[2], value3);
            ensure_eq(layout.counts[2], (size_t)2);
            ensure_eq(layout.sorted_values[3], value5);
            ensure_eq(layout.counts[3], (size_t)3);

            const int64_t value4 = value3 + 1 + rand() % (value5 - value3 - 1);
            tracef("Inserting value4 = %zd for the first time...", value4);
            distribution_insert(&builder, value4);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)5);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value2);
            ensure_eq(layout.counts[1], (size_t)3);
            ensure_eq(layout.sorted_values[2], value3);
            ensure_eq(layout.counts[2], (size_t)2);
            ensure_eq(layout.sorted_values[3], value4);
            ensure_eq(layout.counts[3], (size_t)1);
            ensure_eq(layout.sorted_values[4], value5);
            ensure_eq(layout.counts[4], (size_t)3);

            trace("Inserting value4 again three times...");
            distribution_insert(&builder, value4);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)5);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value2);
            ensure_eq(layout.counts[1], (size_t)3);
            ensure_eq(layout.sorted_values[2], value3);
            ensure_eq(layout.counts[2], (size_t)2);
            ensure_eq(layout.sorted_values[3], value4);
            ensure_eq(layout.counts[3], (size_t)2);
            ensure_eq(layout.sorted_values[4], value5);
            ensure_eq(layout.counts[4], (size_t)3);
            //
            distribution_insert(&builder, value4);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)5);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value2);
            ensure_eq(layout.counts[1], (size_t)3);
            ensure_eq(layout.sorted_values[2], value3);
            ensure_eq(layout.counts[2], (size_t)2);
            ensure_eq(layout.sorted_values[3], value4);
            ensure_eq(layout.counts[3], (size_t)3);
            ensure_eq(layout.sorted_values[4], value5);
            ensure_eq(layout.counts[4], (size_t)3);
            //
            distribution_insert(&builder, value4);
            ensure_eq(builder.inner.allocation, initial_allocation);
            ensure_eq(builder.inner.num_bins, (size_t)5);
            ensure_eq(builder.inner.capacity, initial_capacity);
            layout = distribution_layout(&builder.inner);
            ensure_eq((void*)layout.sorted_values,
                      (void*)initial_layout.sorted_values);
            ensure_eq((void*)layout.counts, (void*)initial_layout.counts);
            ensure_eq(layout.sorted_values[0], value1);
            ensure_eq(layout.counts[0], (size_t)4);
            ensure_eq(layout.sorted_values[1], value2);
            ensure_eq(layout.counts[1], (size_t)3);
            ensure_eq(layout.sorted_values[2], value3);
            ensure_eq(layout.counts[2], (size_t)2);
            ensure_eq(layout.sorted_values[3], value4);
            ensure_eq(layout.counts[3], (size_t)4);
            ensure_eq(layout.sorted_values[4], value5);
            ensure_eq(layout.counts[4], (size_t)3);

            trace("Setting up an allocation backup...");
            size_t allocation_size =
                builder.inner.capacity * distribution_bin_size;
            void* prev_data = malloc(allocation_size);
            int64_t* prev_values = (int64_t*)prev_data;
            size_t* prev_counts = (size_t*)(
                (char*)prev_data + builder.inner.capacity * sizeof(int64_t)
            );

            trace("Inserting new values until reallocation...");
            while (builder.inner.num_bins < builder.inner.capacity) {
                trace("- No reallocation expected here. Backing up state...");
                memcpy(prev_data, builder.inner.allocation, allocation_size);
                const size_t prev_bins = builder.inner.num_bins;

                const int64_t value = rand() - RAND_MAX / 2;
                tracef("- Inserting value %zd...", value);
                distribution_insert(&builder, value);

                trace("- Checking global metadata which shouldn't change...");
                ensure_eq(builder.inner.allocation, initial_allocation);
                ensure_eq(builder.inner.capacity, initial_capacity);

                trace("- Checking bin contents...");
                size_t after_offset;
                size_t insert_pos = SIZE_MAX;
                for (size_t src_pos = 0; src_pos < prev_bins; ++src_pos) {
                    tracef("  * Checking former bin #%zu...", src_pos);
                    if (prev_values[src_pos] < value) {
                        ensure_eq(layout.sorted_values[src_pos],
                                  prev_values[src_pos]);
                        ensure_eq(layout.counts[src_pos], prev_counts[src_pos]);
                    } else if (prev_values[src_pos] > value) {
                        if (insert_pos == SIZE_MAX) {
                            insert_pos = src_pos;
                            after_offset = 1;
                        }
                        const size_t dst_pos = src_pos + after_offset;
                        ensure_eq(layout.sorted_values[dst_pos],
                                  prev_values[src_pos]);
                        ensure_eq(layout.counts[dst_pos], prev_counts[src_pos]);
                    } else {
                        assert(prev_values[src_pos] == value);
                        insert_pos = src_pos;
                        after_offset = 0;
                        ensure_eq(layout.sorted_values[src_pos], value);
                        ensure_eq(layout.counts[src_pos],
                                  prev_counts[src_pos] + 1);
                    }
                }

                const size_t prev_end = prev_bins;
                const size_t prev_last = prev_end - 1;
                if (insert_pos == SIZE_MAX) {
                    trace("- Checking past-the-end insertion...");
                    ensure_eq(builder.inner.num_bins, prev_end + 1);
                    ensure_eq(layout.sorted_values[prev_end], value);
                    ensure_eq(layout.counts[prev_end], (size_t)1);
                } else {
                    trace("- Checking internal insertion...");
                    ensure_eq(builder.inner.num_bins, prev_bins + after_offset);
                }
            }

            trace("Testing reallocation...");
            retry:
                const int64_t value = rand() - RAND_MAX / 2;
                tracef("- Checking candidate value %zd...", value);
                size_t insert_pos = SIZE_MAX;
                for (size_t pos = 0; pos < builder.inner.num_bins; ++pos) {
                    if (layout.sorted_values[pos] > value) {
                        insert_pos = pos;
                        break;
                    } else if (layout.sorted_values[pos] == value) {
                        tracef("  * Value already present in bin #%zu, try again...",
                               pos);
                        goto retry;
                    }
                }
                if (insert_pos == SIZE_MAX) insert_pos = builder.inner.num_bins;
                tracef("  * Value will be inserted as bin #%zu", insert_pos);
            //
            trace("- Backing up state...");
            memcpy(prev_data, builder.inner.allocation, allocation_size);
            const void* const prev_allocation = builder.inner.allocation;
            const size_t prev_bins = builder.inner.num_bins;
            const size_t prev_capacity = builder.inner.capacity;
            //
            trace("- Performing insertion which should reallocate...");
            distribution_insert(&builder, value);
            //
            trace("- Checking that reallocation occured...");
            ensure_ne(builder.inner.allocation, prev_allocation);
            ensure_eq(builder.inner.num_bins, prev_bins + 1);
            ensure_gt(builder.inner.capacity, prev_capacity);
            //
            trace("- Checking that reallocation changes the layout...");
            const distribution_layout_t new_layout =
                distribution_layout(&builder.inner);
            ensure_ne((void*)new_layout.sorted_values,
                      (void*)layout.sorted_values);
            ensure_ne((void*)new_layout.counts, (void*)layout.counts);
            layout = new_layout;
            //
            trace("- Checking bin contents...");
            ensure_eq(layout.sorted_values[insert_pos], value);
            ensure_eq(layout.counts[insert_pos], (size_t)1);
            for (size_t src_pos = 0; src_pos < prev_bins; ++src_pos) {
                tracef("  * Checking former bin #%zu...", src_pos);
                const size_t dst_pos = src_pos + (size_t)(src_pos >= insert_pos);
                ensure_eq(layout.sorted_values[dst_pos],
                          prev_values[src_pos]);
                ensure_eq(layout.counts[dst_pos],
                          prev_counts[src_pos]);
            }

            trace("Reallocating backup storage to match new capacity...");
            free(prev_data);
            allocation_size = builder.inner.capacity * distribution_bin_size;
            prev_data = malloc(allocation_size);
            prev_values = (int64_t*)prev_data;
            prev_counts = (size_t*)(
                (char*)prev_data + builder.inner.capacity * sizeof(int64_t)
            );

            trace("Backing up the final distribution builder...");
            memcpy(prev_data, builder.inner.allocation, allocation_size);

            trace("Building the distribution...");
            distribution_t prev_dist = builder.inner;
            distribution_t dist = distribution_build(&builder);
            ensure_eq(builder.inner.allocation, NULL);
            ensure_eq(builder.inner.num_bins, (size_t)0);
            ensure_eq(builder.inner.capacity, (size_t)0);
            ensure_eq(dist.allocation, prev_dist.allocation);
            ensure_eq(dist.num_bins, prev_dist.num_bins);
            ensure_eq(dist.capacity, prev_dist.capacity);

            trace("Checking the final distribution's bins...");
            size_t expected_end_idx = 0;
            size_t* prev_end_ranks = prev_counts;
            for (size_t bin = 0; bin < dist.num_bins; ++bin) {
                ensure_eq(layout.sorted_values[bin], prev_values[bin]);
                expected_end_idx += prev_counts[bin];
                ensure_eq(layout.end_ranks[bin], expected_end_idx);
                prev_end_ranks[bin] = expected_end_idx;
            }
            prev_counts = NULL;
            ensure_eq(distribution_len(&dist), expected_end_idx);

            trace("Testing distribution sampling...");
            const size_t num_samples = 10 * dist.num_bins;
            for (size_t i = 0; i < num_samples; ++i) {
                trace("- Grabbing one sample...");
                const int64_t sample = distribution_sample(&dist);

                trace("- Checking const correctness and locating sampled bin...");
                size_t sampled_bin = SIZE_MAX;
                for (size_t bin = 0; bin < dist.num_bins; ++bin) {
                    if (layout.sorted_values[bin] == sample) sampled_bin = bin;
                    ensure_eq(layout.sorted_values[bin], prev_values[bin]);
                    ensure_eq(layout.end_ranks[bin], prev_end_ranks[bin]);
                }
                ensure_ne(sampled_bin, SIZE_MAX);
            }

            trace("Deallocating backup storage...");
            free(prev_data);
            prev_data = NULL;
            prev_values = NULL;
            prev_end_ranks = NULL;

            trace("Resetting the distribution...");
            prev_dist = dist;
            builder = distribution_reset(&dist);
            ensure_eq(dist.allocation, NULL);
            ensure_eq(dist.num_bins, (size_t)0);
            ensure_eq(dist.capacity, (size_t)0);
            ensure_eq(builder.inner.allocation, prev_dist.allocation);
            ensure_eq(builder.inner.num_bins, (size_t)0);
            ensure_eq(builder.inner.capacity, prev_dist.capacity);

            trace("Destroying the distribution...");
            distribution_finalize(&builder.inner);
            ensure_eq(builder.inner.allocation, NULL);
            ensure_eq(builder.inner.num_bins, (size_t)0);
            ensure_eq(builder.inner.capacity, (size_t)0);
        }

        // TODO: Remove once everything is broken up in separate modules
        void benchmark_unit_tests() {
            info("Running benchmark harness unit tests...");
            configure_rand();

            debug("Running distribution unit tests...");
            with_log_level(UDIPE_TRACE, {
                test_distibution();
            });

            // TODO: Add unit tests for stats, then clocks

            // TODO: Test other components
        }

    #endif  // UDIPE_BUILD_TESTS

#endif  // UDIPE_BUILD_BENCHMARKS