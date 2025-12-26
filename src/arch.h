#pragma once

//! \file
//! \brief Hardware-specific definitions
//!
//! This code module contains preprocessor defines that encode compile-time
//! knowledge about supported CPU architectures.

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/// Upper bound on the CPU's memory access granularity in bytes
///
/// This is the alignment that is set on struct members that are shared between
/// threads in order to avoid false sharing issues.
///
/// The current definition is known to work for x86_64, aarch64 and powerpc64.
/// It should be extended with ifdefs whenever the need arises as more CPU
/// architectures become supported.
///
/// \internal
///
/// This is 128B and not 64B as you might expect because according to the Intel
/// optimization manual, some modern x86_64 CPUs fetch data at the granularity
/// of pairs of cache lines, effectively doubling the false sharing granularity.
/// with respect to the cache line size that is normally used.
///
/// However, not all x86_64 CPUs implement such pairwise cache line fetching, so
/// when you aim for best spatial cache locality, 64B remains the maximal data
/// structure size that you should aim for on x86_64.
#define FALSE_SHARING_GRANULARITY ((size_t)128)

/// Lower bound on the CPU cache line size, in bytes
///
/// This is the size that any data structure which is not manipulated in array
/// batches should strive to stay under for optimal access performance.
///
/// This number is only used for testing at the time of writing, so it's fine
/// (although obviously not ideal) if the estimate is off.
///
/// The current definition is known to work for x86_64, and should be extended
/// with ifdefs whenever the need arises as more CPU architectures become
/// supported.
#define CACHE_LINE_SIZE ((size_t)64)
static_assert(FALSE_SHARING_GRANULARITY % CACHE_LINE_SIZE == 0,
              "The CPU should access data at the granularity of cache lines");

/// Expected size of the smallest memory page available, in bytes
///
/// This is used to set the size of the flexible array inside of
/// mmap()-allocated storage buffers that are meant to fit in one memory page.
///
/// For this use case, it is okay if the value of the constant is wrong (we just
/// allocate more pages than we should which is not the end of the world), so
/// we tolerate an incorrect estimate on unknown CPU architectures.
///
/// The current definition is x86_64 specific, but coincidentally happens to
/// work for several other popular CPU architectures. Extend if with ifdefs as
/// required once more CPU architectures with other page sizes become supported.
#define EXPECTED_MIN_PAGE_SIZE ((size_t)4096)

#if defined(__x86_64__) || defined(_M_X64)
    /// Macro-compatible truth that the host CPU arch is x86_64
    ///
    /// This is needed because GCC, clang and MSVC don't agree on how x86_64
    /// should be detected at compile time.
    #define X86_64 1
#endif

/// Lower bound on the memory page alignment, in bytes
///
/// This is used to improve compiler optimizations around allocate() by telling
/// the compiler how aligned allocations are guaranteed to be.
///
/// Unlike \ref EXPECTED_MIN_PAGE_SIZE, this definition is a **guaranteed**
/// lower bound, and failure to meet it will result in undefined behavior. Which
/// is why on CPU architectures where the page size isn't known, a very
/// pessimistic guess is taken.
#ifdef X86_64
    #define MIN_PAGE_ALIGNMENT ((size_t)4096)
#else
    #warning "Compiling on an unknown CPU architectures, will take a " \
             "pessimistic lower bound for MIN_PAGE_ALIGNMENT."
    #define MIN_PAGE_ALIGNMENT alignof(max_align_t)
#endif

// x86-specific functionality
#ifdef X86_64
    /// TSC timestamp in clock ticks
    ///
    /// This is the timing unit of the RDTSC and RDTSCP x86 instructions.
    ///
    /// To relate this to real time units like nanoseconds, you must calibrate
    /// TSC clock ticks against the operating system clock.
    typedef uint64_t x86_instant;

    /// Duration in TSC clock ticks
    ///
    /// This is a working quantity that is used when computing durations from
    /// pairs of \ref x86_instant.
    ///
    /// The TSC itself does not go back in time when both readouts have been
    /// taken on a single CPU core. But after subtracting the TSC offset to get
    /// an unbiased duration estimator we can sometimes get negative quantities
    /// when timing very short durations, depending on which side of the TSC
    /// offset confidence interval we end up.
    typedef int64_t x86_duration_ticks;

    /// CPU identifier on x86 systems
    ///
    /// Used to detect CPU migrations in TSC-based timing.
    typedef uint32_t x86_cpu_id;

    /// (timestamp, CPU ID) pair from the RDTSCP instruction
    ///
    /// This can be used for high-precision timing in benchmarks. Just call
    /// `x86_timer_start()` at the beginning of each timed code region,
    /// `x86_timer_end()` at the end of each timed code region, keep the
    /// resulting timestamps around, and once you're done measuring analyse the
    /// resulting data to deduce execution timings.
    typedef struct x86_timestamp_s {
        /// Number of TSC ticks since the last CPU reset
        ///
        /// The difference of such readings between the start and the end of a
        /// benchmark lets you know how many times the TSC ticked during the
        /// execution of the benchmark.
        ///
        /// To relate these TSC ticks to physically meaningful time units like
        /// nanoseconds, you must calibrate the TSC against the operating system
        /// clock during benchmark harness initialization.
        x86_instant ticks;

        /// OS identifier of the CPU on which the TSC was measured
        ///
        /// If this value changes between the start and the end of a timed run,
        /// or between the end of a timed run and the start of the next timed
        /// run, it means that the program was interrupted and migrated to a
        /// different logical CPU by the operating system (aka CPU migration).
        /// When this happens, you should do the following:
        ///
        /// - If this happened between x86_timer_start() and x86_timer_end(),
        ///   then you should always discard the associated duration
        ///   measurement. Subtracting timestamps from the TSC of different CPU
        ///   cores will result in imprecise measurements as different TSCs are
        ///   not kept in perfect sync with each other.
        /// - Whether the migration happened during a timed run or between two
        ///   timed runs, you should discard at least the next duration
        ///   measurement, and possibly some of the subsequent ones depending on
        ///   the nature of the operation that is being benchmarked. Indeed CPU
        ///   migrations result in a loss of all forms of CPU backend warmups
        ///   (cache warmup, branch predictor warmup, turbo ramp-up, wide SIMD
        ///   activation...), and therefore the next timed run will not have the
        ///   same performance characteristics as the previous ones, which
        ///   operated over a warmed-up CPU backend state.
        ///
        /// Unfortunately, a change of core_id is only a sufficient condition
        /// for CPU migration, not a necessary condition. Indeed, for
        /// sufficiently long-running benchmarks, the OS could migrate the
        /// program to a different CPU and back during the two timing calls. To
        /// avoid this undetectable outcome, you should measure the typical time
        /// between CPU migrations at benchmark harness initialization time,
        /// then adjust benchmark run durations accordingly if possible.
        ///
        /// If you are measuring a workload that lasts long enough or performs
        /// enough syscalls that avoiding CPU migration between
        /// x86_timer_start() and x86_timer_end() is not possible, then you
        /// should reconsider using the TSC as your timing source and instead
        /// prefer the operating system's high-resolution clock (like
        /// `CLOCK_MONOTONIC_RAW` on Linux). Indeed, the OS clock provides
        /// stronger guarantees of inter-core synchronization than the TSC on
        /// its own, and is thus safer to use in the presence of CPU migrations.
        ///
        /// For example, the OS high resolution clock usually guarantees
        /// monotonicity across cores (i.e. absence of timestamps going
        /// backwards in time), whereas the TSC does not guarantee this and the
        /// TSCs of different cores may drift away from each other by more than
        /// 1 tick on a system with sufficiently long uptime since boot-time TSC
        /// synchronization occured
        x86_cpu_id cpu_id;
    } x86_timestamp_t;

    /// \ref x86_timestamp_t that was measured by x86_timer_start()
    ///
    /// This is just a convenience typedef to make intended usage of
    /// x86_timer_start() and x86_timer_end() clearer.
    typedef x86_timestamp_t x86_timestamp_start;

    /// \ref x86_timestamp_t that was measured by x86_timer_end()
    ///
    /// This is just a convenience typedef to make intended usage of
    /// x86_timer_start() and x86_timer_end() clearer.
    typedef x86_timestamp_t x86_timestamp_end;

    /// Start of an RDTSCP-based timed benchmark run
    ///
    /// This attempts to minimize interactions between the code that is being
    /// timed and the benchmark harness code that precedes it by taking the
    /// following precautions:
    ///
    /// - A serializing instruction is executed before the RDTSCP call. This
    ///   ensures that instructions from code preceding the timed region will
    ///   interact as little as possible with instructions within the timed
    ///   region. Because some interaction remains unavoidable, you should
    ///   minimize the amount of code that executes between two benchmark runs.
    /// - In the recommended `strict = false` configuration, an LFENCE is
    ///   executed after the RDTSCP call. This ensures that instructions from
    ///   within the timed region can begin executing before the clock timestamp
    ///   has been acquired. It does allow for instructions to be fetched from
    ///   memory before timer readout, including via branch prediction, which
    ///   some may consider as a form of hardware cheating. But in the author's
    ///   opinion that's actually fair game because in all realistic execution
    ///   scenarios we care about the performance of code that has already been
    ///   fetched from memory, not code that is not fetched from memory yet.
    /// - If you nevertheless want to minimize the amount of hardware cheating,
    ///   then set `strict = true` to force a full serializing instruction
    ///   barrier between the initial TSC readout and the timed code region.
    ///   Beware that this will not prevent other phenomena that can be
    ///   considered as forms of hardware cheating (cache warmup, branch
    ///   predictor warmup...), and that this will increase clock measurement
    ///   overhead and thus require longer benchmark runs.
    ///
    /// \param strict must be a compile-time boolean constant that the compiler
    ///               recognizes as such, like bare `true` and `false` keywords.
    ///               It tells whether the timer implementation should attempt
    ///               to maximally prevent hardware cheating at the expense of
    ///               increased overhead and reduced realism.
    ///
    /// \returns a start timestamp that should be paired with an end timestamp
    ///          measured using x86_timer_end().
    static inline x86_timestamp_start x86_timer_start(bool strict) {
        uint32_t eax_out, ecx_out, edx_out;
        #ifdef __GNUC__
            // In both modes we use CPUID before RDTSC to maximally shield the
            // timed region from preceding benchmark harness instructions.
            if (strict) {
                // In strict mode, we additionally use CPUID after RDTSC to
                // ensure that timed instructions cannot escape the timed
                // region. This requires us to save the output of RDTSC away
                // from eax, ecx and edx beforehand, because this RDTSC output
                // will be overwritten by CPUID.
                __asm__ volatile ("cpuid\n\t"
                                  "rdtscp\n\t"
                                  "mov %%eax, %0\n\t"
                                  "mov %%ecx, %1\n\t"
                                  "mov %%edx, %2\n\t"
                                  "cpuid"
                                  : "=r"(eax_out), "=r"(ecx_out), "=r"(edx_out)
                                  :
                                  : "eax", "ebx", "ecx", "edx");
            } else {
                // In relaxed mode we only use LFENCE, which is weaker as CPUID
                // as it allows instruction fetch to occur before TSC readout.
                // That's fine and more realistic in most circumstances.
                __asm__ volatile ("cpuid; rdtscp; lfence"
                                  : "=a"(eax_out), "=c"(ecx_out), "=d"(edx_out)
                                  :
                                  : "ebx");
            }
        #elif defined(_MSC_VER)
            if (strict) {
                __asm {
                    cpuid;
                    rdtscp;
                    mov eax_out eax;
                    mov ecx_out ecx;
                    mov edx_out edx;
                    cpuid;
                }
            } else {
                __asm {
                    cpuid;
                    rdtscp;
                    lfence;
                    mov eax_out eax;
                    mov ecx_out ecx;
                    mov edx_out edx;
                }
            }
        #else
            #error "Sorry, we don't support your compiler yet. Please file a bug report about it!"
        #endif
        return (x86_timestamp_start){
            .ticks = (x86_instant)edx_out << 32 | eax_out,
            .cpu_id = ecx_out,
        };
    }

    /// End of an RDTSCP-based timed benchmark run
    ///
    /// Like x86_timer_start(), this attempts to minimize interactions between
    /// the code that is being timed and the benchmark harness code that comes
    /// after it. But due to the one-way nature of some x86 memory and execution
    /// barriers, the logic surrounding the RDTSCP call is a little different
    /// than in x86_timer_start().
    ///
    /// - On its own, RDTSCP acts as an LFENCE. It waits for previous
    ///   instructions to have executed and for loads to have fetched data
    ///   before measuring the TSC value. But it does not wait for buffered
    ///   stores to be committed to caches/memory or for previous instructions
    ///   to have fully retired.
    ///     - In the author's opinion, this is fair game, as in all realistic
    ///       execution scenarios we care about the performance of code
    ///       execution with store buffering enabled, not with store buffering
    ///       artificially inhibited. Therefore in the recommended `strict =
    ///       false` configuration we treat this LFENCE barrier as sufficient.
    ///     - If you care about minimizing hardware cheating to the fullest
    ///       extent that x86 enables, set `strict = true`. This will force a
    ///       full serializing instruction barrier at the expense of increased
    ///       overhead and reduced measurement realism.
    /// - A serializing instruction is used after RDTSCP to ensure that to the
    ///   fullest extent allowed by x86, no code after the timing call can
    ///   interfere with the timing of the code that is being benchmarked.
    ///
    /// \param strict must be a compile-time boolean constant that the compiler
    ///               recognizes as such, like bare `true` and `false` keywords.
    ///               It tells whether the timer implementation should attempt
    ///               to maximally prevent hardware cheating at the expense of
    ///               increased overhead and reduced realism.
    ///
    /// \returns an end timestamp that should be paired with the start timestamp
    ///          previously measured using x86_timer_start() for the purpose of
    ///          duration and CPU migration analysis. To minimize benchmark
    ///          environment drift during the measurement period, it is
    ///          recommended not to perform this analysis right away, but
    ///          instead do nothing but accumulate timestamps in a loop until
    ///          you're done with time measurements then perform all analysis at
    ///          the end, discarding "bad" data points as necessary.
    static inline x86_timestamp_end x86_timer_end(bool strict) {
        uint32_t eax_out, ecx_out, edx_out;
        #ifdef __GNUC__
            // In both modes we use CPUID after RDTSC to maximally shield the
            // timed region from subsequent benchmark harness instructions. This
            // requires us to save eax, ecx and edx beforehand as the RDTSC
            // output will be overwritten by CPUID.
            if (strict) {
                // In strict mode, we additionally use CPUID before RDTSC to
                // ensure that pending buffered stores are committed to caches
                // and memory before the end of the timed region.
                __asm__ volatile ("cpuid\n\t"
                                  "rdtscp\n\t"
                                  "mov %%eax, %0\n\t"
                                  "mov %%ecx, %1\n\t"
                                  "mov %%edx, %2\n\t"
                                  "cpuid"
                                  : "=r"(eax_out), "=r"(ecx_out), "=r"(edx_out)
                                  :
                                  : "eax", "ebx", "ecx", "edx");
            } else {
                // In relaxed mode, we let the implicit LFENCE at the start of
                // RDTSCP do its job, it is good enough for most purposes.
                __asm__ volatile ("rdtscp\n\t"
                                  "mov %%eax, %0\n\t"
                                  "mov %%ecx, %1\n\t"
                                  "mov %%edx, %2\n\t"
                                  "cpuid"
                                  : "=r"(eax_out), "=r"(ecx_out), "=r"(edx_out)
                                  :
                                  : "eax", "ebx", "ecx", "edx");
            }
        #elif defined(_MSC_VER)
            if (strict) {
                __asm {
                    cpuid;
                    rdtscp;
                    mov eax_out eax;
                    mov ecx_out ecx;
                    mov edx_out edx;
                    cpuid;
                }
            } else {
                __asm {
                    rdtscp;
                    mov eax_out eax;
                    mov ecx_out ecx;
                    mov edx_out edx;
                    cpuid;
                }
            }
        #else
            #error "Sorry, we don't support your compiler yet. Please file a bug report about it!"
        #endif
        return (x86_timestamp_start){
            .ticks = (x86_instant)edx_out << 32 | eax_out,
            .cpu_id = ecx_out,
        };
    }
#endif  // x86-specific functionality
