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

    #include <stdbool.h>


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


    /// \name Compiler optimization barriers
    /// \{

    #ifdef __GNUC__

        // On GCC and clang, we use inline assembly and statement expression
        // tricks to provide very low-overhead optimization barriers.

        /// UDIPE_ASSUME_READ() fast path for self-contained values that
        /// naturally fit in general-purpose CPU registers
        ///
        /// This is a compiler-specific implementation detail of
        /// UDIPE_ASSUME_READ() that you should not use directly. See
        /// UDIPE_ASSUME_READ() for more information about general semantics.
        ///
        /// \internal
        ///
        /// Although the more general `r,m` constraint should in principle be
        /// better by virtue of avoiding unnecessary loads and stores when the
        /// source operand is resident in memory, at the time of writing GCC and
        /// clang both handle it poorly by taking the opposite stance that if an
        /// ASM operand can be in memory rather than in registers, then it
        /// should be in memory.
        ///
        /// This results in a performance degradation when the result is
        /// initially resident in registers, and gets spilled unnecessarily as a
        /// result. So given that the performance of register-resident values is
        /// usually more critical than that of memory-resident values, we
        /// priorize the former.
        #define UDIPE_ASSUME_READ_GPR(value)  \
            __asm__ volatile ("" : : "r" ((value)))

        /// UDIPE_ASSUME_ACCESSED() fast path for self-contained values that
        /// naturally fit in general-purpose CPU registers
        ///
        /// This is a compiler-specific implementation detail of
        /// UDIPE_ASSUME_ACCESSED() that you should not use directly. See
        /// UDIPE_ASSUME_ACCESSED() for more information about general
        /// semantics.
        ///
        /// \internal
        ///
        /// See UDIPE_ASSUME_READ_GPR() for more info about why the `r`
        /// constraint is used as opposed to the more general `r,m` constraint.
        #define UDIPE_ASSUME_ACCESSED_GPR(value)  \
            __asm__ volatile ("" : "+r" ((value)))

        #ifdef __x86_64__

            /// UDIPE_ASSUME_READ() fast path for floating-point numbers
            ///
            /// This is a compiler-specific implementation detail of
            /// UDIPE_ASSUME_READ() that you should not use directly. See
            /// UDIPE_ASSUME_READ() for more information about general
            /// semantics.
            ///
            /// \internal
            ///
            /// Floating-point scalars get put in SIMD registers because that's
            /// their normal place on x86_64 (as encoded into OS calling
            /// conventions), and thus doing so reduces the odds that
            /// register-register motion and/or CPU computation domain crossing
            /// will be necessary to get the value to the requested place.
            ///
            /// However this barrier may still cause significant overhead if
            /// applied to a computation that was vectorized.
            ///
            /// See UDIPE_ASSUME_READ_GPR() for more info about why the `x`
            /// constraint is used as opposed to the more general `x,m`
            /// constraint.
            #define UDIPE_ASSUME_READ_FP(value)  \
                __asm__ volatile ("" : : "x" ((value)))

            /// UDIPE_ASSUME_ACCESSED() fast path for floating-point numbers
            ///
            /// This is a compiler-specific implementation detail of
            /// UDIPE_ASSUME_ACCESSED() that you should not use directly. See
            /// UDIPE_ASSUME_ACCESSED() for more information about general
            /// semantics.
            ///
            /// \internal
            ///
            /// See UDIPE_ASSUME_READ_FP() for more information about this
            /// implementation.
            #define UDIPE_ASSUME_ACCESSED_FP(value)  \
                __asm__ volatile ("" : "+x" ((value)))

        #else

            #warn "Floating-point optimization barriers have not been optimized for this CPU architecture, and may therefore have unusually high overhead!"

            /// UDIPE_ASSUME_READ() slow path for floating-point numbers
            ///
            /// This is a compiler-specific implementation detail of
            /// UDIPE_ASSUME_READ() that you should not use directly. See
            /// UDIPE_ASSUME_READ() for more information about general
            /// semantics.
            ///
            /// \internal
            ///
            /// When the floating-point register usage convention of a platform
            /// isn't known, UDIPE_ASSUME_READ() simply puts floating-point
            /// values in general-purpose registers. This is likely to cause
            /// extra register shuffling and FP/int domain transition overhead.
            #define UDIPE_ASSUME_READ_FP(value)  \
                UDIPE_ASSUME_READ_GPR(value)

            /// UDIPE_ASSUME_ACCESSED() slow path for floating-point numbers
            ///
            /// This is a compiler-specific implementation detail of
            /// UDIPE_ASSUME_ACCESSED() that you should not use directly. See
            /// UDIPE_ASSUME_ACCESSED() for more information about general
            /// semantics.
            ///
            /// \internal
            ///
            /// See UDIPE_ASSUME_READ_FP() for more information about this
            /// implementation.
            #define UDIPE_ASSUME_ACCESSED_FP(value)  \
                UDIPE_ASSUME_ACCESSED_GPR(value)

        #endif

        /// UDIPE_ASSUME_READ() fast path for pointers
        ///
        /// This is a compiler-specific implementation detail of
        /// UDIPE_ASSUME_READ() that you should not use directly. See
        /// UDIPE_ASSUME_READ() for more information about general semantics.
        ///
        /// \internal
        ///
        /// Pointers fit in GPRs but must additionally use memory clobbers,
        /// which will cause their target data to spill to memory.
        ///
        /// This will have the side effect of making the compiler assume that
        /// global and thread-local variables are modified too. And due to
        /// compiler limitations, it may affect unrelated pointers too.
        #define UDIPE_ASSUME_READ_PTR(value)  \
            __asm__ volatile ("" : : "r" ((value)) : "memory")


        /// UDIPE_ASSUME_ACCESSED() fast path for pointers
        ///
        /// This is a compiler-specific implementation detail of
        /// UDIPE_ASSUME_ACCESSED() that you should not use directly. See
        /// UDIPE_ASSUME_ACCESSED() for more information about general
        /// semantics.
        ///
        /// \internal
        ///
        /// See UDIPE_ASSUME_READ_PTR() for more info about how pointers are
        /// handled internally by this implementation.
        #define UDIPE_ASSUME_ACCESSED_PTR(value)  \
            __asm__ volatile ("" : "+r" ((value)) : : "memory")

        /// UDIPE_ASSUME_READ() slow path for unknown data types
        ///
        /// This is a compiler-specific implementation detail of
        /// UDIPE_ASSUME_READ() that you should not use directly. See
        /// UDIPE_ASSUME_READ() for more information about general semantics.
        ///
        /// \internal
        ///
        /// Compared to the pointer path, the main thing we lose is that we
        /// cannot assume that the user type fits in GPRs anymore, which means
        /// that we must allow memory operands and thus allow GCC and clang to
        /// spill data memory when it should stay resident in registers.
        #define UDIPE_ASSUME_READ_ANYTHING(value)  \
            __asm__ volatile ("" : : "r,m" ((value)) : "memory")


        /// UDIPE_ASSUME_ACCESSED() slow path for unknown data types
        ///
        /// This is a compiler-specific implementation detail of
        /// UDIPE_ASSUME_ACCESSED() that you should not use directly. See
        /// UDIPE_ASSUME_ACCESSED() for more information about general
        /// semantics.
        ///
        /// \internal
        ///
        /// See UDIPE_ASSUME_READ_ANYTHING() for more info about how this
        /// implementation differs from the closely related pointer
        /// implementation.
        #define UDIPE_ASSUME_ACCESSED_ANYTHING(value)  \
            __asm__ volatile ("" : "+r,m" ((value)) : : "memory")

        /// Make the compiler assume that the result `x` is used by something
        ///
        /// Benchmarks tend to do the same thing in a loop. But when optimizing
        /// compilers realize that the result of a loop iteration is unused,
        /// they love to optimize it out, or worse, optimize out parts of it
        /// (which is harder to detect in benchmark measurements). This macro
        /// lets you avoid this problem at the lowest cost that your compiler
        /// will allow.
        ///
        /// Some caveats to keep in mind:
        ///
        /// - While it is designed to be as lightweight as possible, this
        ///   barrier will still inhibit some important compiler optimizations
        ///   like loop autovectorization. Therefore using it on a primitive
        ///   which is meant to be used in the innermost loop of your
        ///   computations will likely degrade measured performance in an
        ///   unrealistic fashion. It is often better to measure the performance
        ///   of such functions as they are executed over small data batches
        ///   (such that the program working set fits in L1 cache) then average
        ///   out the result.
        /// - With the implementation used by GCC and clang, this barrier can
        ///   work on arbitrarily complex expressions, rather than variables
        ///   only. But this may result in poor codegen on clang at the time of
        ///   writing, and the portability of the resulting code to the
        ///   alternate implementation used by MSVC is unknown. It is therefore
        ///   safer to apply this optimization barrier to local variables only.
        /// - The implementation for pointers will cause the targeted data to be
        ///   spilled from registers to memory, but it may also affect other
        ///   data as a side-effect, especially global and thread-local
        ///   variables.
        /// - Due to limitations of the C type system, user-defined types such
        ///   as enums and structs will use a suboptimal implementation that may
        ///   have a lot more overhead than necessary.
        ///
        /// \param x should be the name of a variable, ideally one that is local
        ///          to the calling function.
        #define UDIPE_ASSUME_READ(x)  \
            _Generic((x),  \
                bool: ({ UDIPE_ASSUME_READ_GPR((x)); }),  \
                signed char: ({ UDIPE_ASSUME_READ_GPR((x)); }),  \
                unsigned char: ({ UDIPE_ASSUME_READ_GPR((x)); }),  \
                short: ({ UDIPE_ASSUME_READ_GPR((x)); }),  \
                unsigned short: ({ UDIPE_ASSUME_READ_GPR((x)); }),  \
                int: ({ UDIPE_ASSUME_READ_GPR((x)); }),  \
                unsigned: ({ UDIPE_ASSUME_READ_GPR((x)); }),  \
                long: ({ UDIPE_ASSUME_READ_GPR((x)); }),  \
                unsigned long: ({ UDIPE_ASSUME_READ_GPR((x)); }),  \
                long long: ({ UDIPE_ASSUME_READ_GPR((x)); }),  \
                unsigned long long: ({ UDIPE_ASSUME_READ_GPR((x)); }),  \
                float: ({ UDIPE_ASSUME_READ_FP((x)); }),  \
                double: ({ UDIPE_ASSUME_READ_FP((x)); }),  \
                void*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                bool*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                signed char*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                unsigned char*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                short*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                unsigned short*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                int*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                unsigned*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                long*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                unsigned long*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                long long*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                unsigned long long*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                float*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                double*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                const void*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                const bool*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                const signed char*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                const unsigned char*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                const short*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                const unsigned short*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                const int*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                const unsigned*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                const long*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                const unsigned long*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                const long long*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                const unsigned long long*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                const float*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                const double*: ({ UDIPE_ASSUME_READ_PTR((x)); }),  \
                default: ({ UDIPE_ASSUME_READ_ANYTHING((x)); })  \
            )

        /// Make the compiler assume that the result `x` is used by something,
        /// then replaced with a totally different value.
        ///
        /// This does everything that UDIPE_ASSUME_READ() does and additionally
        /// makes the compiler believe that the value of `x` changes to
        /// something completely different after it has been read.
        ///
        /// With respect to UDIPE_ASSUME_READ(), the main new caveat to keep in
        /// mind is that you must be allowed to change the value of `x` for this
        /// optimization barrier to work as expected. Applying this to a `const`
        /// value may not do anything more than UDIPE_ASSUME_READ().
        ///
        /// \param x should be the name of a variable, ideally one that is local
        ///          to the calling function. You should be allowed to modify
        ///          this variable i.e. it should not be `const`.
        #define UDIPE_ASSUME_ACCESSED(x)  \
            _Generic((x),  \
                bool: ({ UDIPE_ASSUME_ACCESSED_GPR((x)); }),  \
                signed char: ({ UDIPE_ASSUME_ACCESSED_GPR((x)); }),  \
                unsigned char: ({ UDIPE_ASSUME_ACCESSED_GPR((x)); }),  \
                short: ({ UDIPE_ASSUME_ACCESSED_GPR((x)); }),  \
                unsigned short: ({ UDIPE_ASSUME_ACCESSED_GPR((x)); }),  \
                int: ({ UDIPE_ASSUME_ACCESSED_GPR((x)); }),  \
                unsigned: ({ UDIPE_ASSUME_ACCESSED_GPR((x)); }),  \
                long: ({ UDIPE_ASSUME_ACCESSED_GPR((x)); }),  \
                unsigned long: ({ UDIPE_ASSUME_ACCESSED_GPR((x)); }),  \
                long long: ({ UDIPE_ASSUME_ACCESSED_GPR((x)); }),  \
                unsigned long long: ({ UDIPE_ASSUME_ACCESSED_GPR((x)); }),  \
                float: ({ UDIPE_ASSUME_ACCESSED_FP((x)); }),  \
                double: ({ UDIPE_ASSUME_ACCESSED_FP((x)); }),  \
                void*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                bool*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                signed char*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                unsigned char*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                short*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                unsigned short*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                int*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                unsigned*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                long*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                unsigned long*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                long long*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                unsigned long long*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                float*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                double*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                const void*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                const bool*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                const signed char*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                const unsigned char*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                const short*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                const unsigned short*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                const int*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                const unsigned*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                const long*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                const unsigned long*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                const long long*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                const unsigned long long*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                const float*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                const double*: ({ UDIPE_ASSUME_ACCESSED_PTR((x)); }),  \
                default: ({ UDIPE_ASSUME_ACCESSED_ANYTHING((x)); })  \
            )

    #elif defined(_MSC_VER)

        // MSVC does not have anything comparable to the GNU inline assembly
        // trick so we must use unoptimized functions as optimization barriers.
        // This will have a lot more overhead.

        #pragma optimize("", off)

            /// MSVC implementation of UDIPE_ASSUME_READ()
            ///
            /// This is a compiler-specific implementation detail of
            /// UDIPE_ASSUME_READ() that you should not use directly. See
            /// UDIPE_ASSUME_READ() for more information about general
            /// semantics.
            ///
            /// \internal
            ///
            /// Since MSVC inline assembly cannot be used for optimization
            /// barriers, we go for the less fancy trick of using an unoptimized
            /// function, which won't be inlined, as an optimization barrier.
            ///
            /// This is enough to make MSVC assume that the target value can be
            /// read in an arbitrary fashion, at the time of writing at least.
            static void udipe_assume_read_impl(const void* x) {}

            /// MSVC implementation of UDIPE_ASSUME_ACCESSED()
            ///
            /// This is a compiler-specific implementation detail of
            /// UDIPE_ASSUME_ACCESSED() that you should not use directly. See
            /// UDIPE_ASSUME_ACCESSED() for more information about general
            /// semantics.
            ///
            /// \internal
            ///
            /// See udipe_assume_read_impl() for more information about how this
            /// optimization barrier implementation works.
            static void udipe_assume_accessed_impl(void* x) {}

        #pragma optimize("", on)

        /// Make the compiler assume that the result `x` is used by something
        ///
        /// Benchmarks tend to do the same thing in a loop. But when optimizing
        /// compilers realize that the result of a loop iteration is unused,
        /// they love to optimize it out, or worse, optimize out parts of it
        /// (which is harder to detect in benchmark measurements). This macro
        /// lets you avoid this problem at the lowest cost that your compiler
        /// will allow.
        ///
        /// Some caveats to keep in mind:
        ///
        /// - While it is designed to be as lightweight as possible, this
        ///   barrier will still inhibit some important compiler optimizations
        ///   like loop autovectorization. Therefore using it on a primitive
        ///   which is meant to be used in the innermost loop of your
        ///   computations will likely degrade measured performance in an
        ///   unrealistic fashion. It is often better to measure the performance
        ///   of such functions as they are executed over small data batches
        ///   (such that the program working set fits in L1 cache) then average
        ///   out the result.
        /// - The kind of expressions that can be valid to use as `x` is bound
        ///   by C temporary extension rules, which are ill-defined. It is
        ///   therefore safer to apply this optimization barrier to variables
        ///   only, preferably local ones as this will reduce the optimization
        ///   barrier's cost.
        /// - The MSVC implementation will cause data targeted by pointers to be
        ///   spilled from registers to memory, but it may also affect other
        ///   data as a side-effect, especially global and thread-local
        ///   variables.
        ///
        /// \param x should be the name of a variable, ideally one that is local
        ///          to the calling function.
        #define UDIPE_ASSUME_READ(x)  udipe_assume_read_impl(&(x))

        /// Make the compiler assume that the result `x` is used by something,
        /// then replaced with a totally different value.
        ///
        /// This does everything that UDIPE_ASSUME_READ() does and additionally
        /// makes the compiler believe that the value of `x` changes to
        /// something completely different after it has been read.
        ///
        /// With respect to UDIPE_ASSUME_READ(), the main new caveat to keep in
        /// mind is that you must be allowed to change the value of `x` for this
        /// optimization barrier to work as expected. Applying this to a `const`
        /// value may not do anything more than UDIPE_ASSUME_READ().
        ///
        /// \param x should be the name of a variable, ideally one that is local
        ///          to the calling function. You should be allowed to modify
        ///          this variable i.e. it should not be `const`.
        #define UDIPE_ASSUME_ACCESSED(x)  udipe_assume_accessed_impl(&(x))

    #else
        #error "Sorry, we don't support your compiler yet. Please file a bug report about it!"
    #endif

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
