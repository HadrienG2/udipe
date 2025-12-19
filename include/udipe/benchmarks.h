#ifdef UDIPE_BUILD_BENCHMARKS

    #pragma once

    //! \file
    //! \brief Benchmarking infrastructure
    //!
    //! This header contains the benchmarking infrastructure of `libudipe`. It
    //! is an implementation detail of binaries from the benches/ directory that
    //! you should not use directly. Please run the benchmark binaries instead.

    #include "pointer.h"
    #include "visibility.h"


    /// \name Benchmark harness
    /// \{

    /// Benchmarking harness
    ///
    /// This is an implementation detail of binaries from the benches/
    /// directory that you should not use directly.
    typedef struct udipe_benchmark_s udipe_benchmark_t;

    /// Set up a benchmarking harness according to CLI arguments
    ///
    /// This is an implementation detail of binaries from the benches/
    /// directory that you should not use directly.
    ///
    /// \internal
    ///
    /// \param argc must be the unmodified `argc` argument to the benchmark
    ///             binary's main function
    /// \param argv must be the unmodified `argv` argument to the benchmark
    ///             binary's main function.
    /// \returns a benchmark harness that can be used until it is destroyed with
    ///          benchmark_finalize().
    UDIPE_PUBLIC
    UDIPE_NON_NULL_ARGS
    UDIPE_NON_NULL_RESULT
    udipe_benchmark_t* udipe_benchmark_initialize(int argc, char *argv[]);

    /// Callable that UDIPE_BENCHMARK()/udipe_benchmark_run() may execute
    ///
    /// Notice the traditional `void* context` argument for providing
    /// user-defined args to this callable.
    typedef void (*udipe_callable_t)(void* /*context */,
                                     udipe_benchmark_t* /* benchmark */);

    /// Execute a benchmark if it passes user filtering conditions
    ///
    /// This is an implementation detail of binaries from the benches/
    /// directory that you should not use directly.
    ///
    /// \internal
    ///
    /// The benchmark function will be called in the scope of with_logger(), and
    /// can thus freely use internal logging primitives.
    ///
    /// \param benchmark must be a benchmark harness that has been initialized
    ///                  with benchmark_initialize() and hasn't been destroyed
    ///                  with benchmark_finalize() yet.
    /// \param callable is the function that should be called to execute the
    ///                 benchmark if its name passes the filter.
    /// \param context is an arbitrary user-defined pointer that is passed to
    ///                the callable if it is called.
    #define UDIPE_BENCHMARK(benchmark, callable, context)  \
        udipe_benchmark_run(benchmark,  \
                            #callable,  \
                            callable,  \
                            context)

    /// Implementation of the UDIPE_BENCHMARK() macro
    ///
    /// This is an implementation detail of binaries from the benches/
    /// directory that you should not use directly.
    ///
    /// \internal
    ///
    /// This function executes a benchmark if its name passes the user-specified
    /// filtering conditions.
    ///
    /// You should usually prefer using the higher-level UDIPE_BENCHMARK() macro
    /// over direct calls this function.
    ///
    /// \param benchmark must be a benchmark harness that has been initialized
    ///                  with benchmark_initialize() and hasn't been destroyed
    ///                  with benchmark_finalize() yet.
    /// \param name is the name of the function that one is intending to
    ///             benchmark.
    /// \param callable is the function that should be called to execute the
    ///                 benchmark if its name passes the filter.
    /// \param context is an arbitrary user-defined pointer that is passed to
    ///                the callable if it is called.
    /// \returns the truth that the benchmark has been run.
    UDIPE_PUBLIC
    UDIPE_NON_NULL_SPECIFIC_ARGS(1, 2, 3)
    bool udipe_benchmark_run(udipe_benchmark_t* benchmark,
                             const char* name,
                             udipe_callable_t callable,
                             void* context);

    /// Tear down a benchmarking harness
    ///
    /// This is an implementation detail of binaries from the benches/
    /// directory that you should not use directly.
    ///
    /// \internal
    ///
    /// This destroys the benchmark harness, which cannot be used afterwards.
    ///
    /// \param benchmark must be a benchmark harness that has been initialized
    ///                  with benchmark_initialize() and hasn't been destroyed
    ///                  with benchmark_finalize() yet.
    UDIPE_PUBLIC
    UDIPE_NON_NULL_ARGS
    void udipe_benchmark_finalize(udipe_benchmark_t** benchmark);

    /// \}


    /// Run all the libudipe micro-benchmarks
    ///
    /// This is an implementation detail of the benches/micro_benchmarks.c
    /// binary. Please run this binary instead of calling this internal function
    /// whose API may change without warnings.
    ///
    /// \internal
    ///
    /// \param benchmark must be a benchmark harness that has been initialized
    ///                  with benchmark_initialize() and hasn't been destroyed
    ///                  with benchmark_finalize() yet.
    UDIPE_PUBLIC
    UDIPE_NON_NULL_ARGS
    void udipe_micro_benchmarks(udipe_benchmark_t* benchmark);

#endif  // UDIPE_BUILD_BENCHMARKS
