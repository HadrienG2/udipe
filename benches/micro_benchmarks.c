#include <udipe/benchmark.h>

int main(int argc, char *argv[]) {
    udipe_benchmark_t* benchmark = udipe_benchmark_initialize(argc, argv);
    udipe_micro_benchmarks(benchmark);
    udipe_benchmark_finalize(&benchmark);
    return 0;
}
