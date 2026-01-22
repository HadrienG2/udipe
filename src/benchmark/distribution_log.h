#ifdef UDIPE_BUILD_BENCHMARKS

    #pragma once

    //! \file
    //! \brief Logging sample distributions
    //!
    //! Statistics summarize a bunch of numbers into a single one, which is very
    //! convenient but inherently lossy. Sometimes the loss is acceptable, and
    //! sometimes it obscures important sample properties such as the
    //! multi-modal nature of some timing distributions.
    //!
    //! This is why unknown data should always be eyeballed through a more
    //! detailed display first, and this module provides the means to do that by
    //! logging raw data distributions.

    #include <udipe/log.h>
    #include <udipe/pointer.h>

    #include "distribution.h"

    #include <math.h>
    #include <stdint.h>


    /// Log the shape of a \ref distribution_t as a textual histogram
    ///
    /// This is typically done right before calling distribution_build(), to
    /// check out the final state of the distribution after performing all
    /// insertions.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't yet been recycled via
    ///             distribution_reset() or destroyed via
    ///             distribution_finalize(). It will be turned into the output
    ///             distribution builder returned by this function, and
    ///             therefore cannot be used after calling this function.
    /// \param level is the log level at which the distribution state should be
    ///              logged.
    /// \param header is a string that will precede the distribution display in
    ///                logs.
    UDIPE_NON_NULL_ARGS
    void distribution_log(const distribution_t* dist,
                          udipe_log_level_t level,
                          const char header[]);


    /// \name Implementation details
    /// \{

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

    /// Size of a buffer that can hold horizontal lines up to a certain width
    ///
    /// This function determines how many bytes must be allocated to a buffer
    /// that holds a horizontal line made of ─ or ═ box-drawing characters,
    /// typically generated using write_horizontal_line() or as part of
    /// write_title_borders().
    ///
    /// \param max_width is an upper bound on the width of the lines that will
    ///                  be stored into this buffer, in terminal columns.
    ///
    /// \returns the number of bytes needed to hold a horizontal textual line of
    ///          up to `max_width` segments.
    size_t line_buffer_size(size_t max_width);

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
    ///              be large enough to hold all of them and a terminating `NUL`.
    UDIPE_NON_NULL_ARGS
    void write_horizontal_line(char buffer[],
                               const char segment[],
                               size_t width);

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
    void write_title_borders(char left_buffer[],
                             const char title[],
                             char right_buffer[],
                             const char line_segment[],
                             size_t width);

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

    /// Compute the \ref axis_len_t of a certain type of plot
    ///
    /// \param type is the kind of plot that as being drawn
    ///
    /// \returns the number of abscissa and ordinate data points that the plot
    ///          will be composed of.
    axis_len_t plot_axis_len(plot_type_t type);

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
    ///             distribution_build() and hasn't been turned back into a \ref
    ///             distribution_builder_t or destroyed since.
    /// \param type is the kind of plot that is being drawn.
    ///
    /// \returns the maximal abscissa range for the plot of interest.
    UDIPE_NON_NULL_ARGS
    range_t plot_autoscale_abscissa(const distribution_t* dist,
                                    plot_type_t type);

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
    void plot_compute_abscissa(plot_type_t type,
                               coord_t abscissa[],
                               range_t range,
                               axis_len_t len);

    /// Compute the ordinates from a plot
    ///
    /// From a previously generated set of increasing abscissa values stored in
    /// `abscissa`, which can be generated via plot_compute_abscissa(), this
    /// function writes the matching set of ordinate values to `ordinate`.
    ///
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't been turned back into a \ref
    ///             distribution_builder_t or destroyed since.
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
    void plot_compute_ordinate(const distribution_t* dist,
                               plot_type_t type,
                               const coord_t abscissa[],
                               coord_t ordinate[],
                               axis_len_t len);

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
    range_t plot_autoscale_ordinate(plot_type_t type,
                                    const coord_t ordinate[],
                                    axis_len_t len);

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
    plot_layout_t plot_layout(plot_type_t type,
                              const coord_t abscissa[],
                              const coord_t ordinate[],
                              axis_len_t len);

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
    /// \param layout specifies the plot layout, used here to figure out the
    ///               terminal column budget for horizontal lines.
    /// \param ordinate_range specifies the minimum and maximum ordinate values
    ///                       outside of which the ordinate scale will saturate
    ///                       to a min/max bar length.
    /// \param ordinate is the ordinate whose display is meant to be drawn.
    /// \param output is the text buffer into which the ordinate line display
    ///               will be recorded. It must be able to hold at least the
    ///               amount of bytes specified by
    ///               `line_buffer_size(layout->max_bar_width)`.
    UDIPE_NON_NULL_ARGS
    void plot_draw_line(plot_type_t type,
                        const plot_layout_t* layout,
                        range_t ordinate_range,
                        coord_t ordinate,
                        char output[]);

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
    ///             distribution_build() and hasn't been turned back into a \ref
    ///             distribution_builder_t or destroyed since.
    /// \param type is the kind of plot that is being drawn.
    UDIPE_NON_NULL_ARGS
    void log_plot(udipe_log_level_t level,
                  const char title[],
                  const distribution_t* dist,
                  plot_type_t type);

    /// \}


    // TODO: Add unit tests

#endif  // UDIPE_BUILD_BENCHMARKS
