#!/bin/bash
set -euo pipefail
#
# This is intended as a simple everyday development script that sets up builds
# and docs if they don't exist yet, updates the builds, runs tests, and finally
# runs benchmarks.
#
# Every operation that can fail in a hard-to-understand fashion is re-run upon
# failure in a mode that is less efficient but easier to debug: parallel things
# become sequential, udipe things are re-run at TRACE log level...

# We'll set up two bulds:
# - A Debug build for testing
# - A RelWithDebInfo build for benchmarking
mkdir -p build-{test,bench}

# If something goes wrong, we'll insert this visual separator before trying
# again in a manner that should produce more readable output
function separator() {
    printf "\n\n=== ERROR: Let's try that again more slowly... ===\n\n"
}

# Set up or update the testing build
cd build-test
if [[ ! -e CMakeCache.txt ]]; then
    cmake -DCMAKE_BUILD_TYPE=Debug  \
          -DUDIPE_BUILD_DOXYGEN=ON  \
          -DUDIPE_BUILD_DOXYGEN_INTERNAL=ON  \
          -DUDIPE_BUILD_EXAMPLES=ON  \
          -DUDIPE_BUILD_TESTS=ON  \
          -DUDIPE_WERROR=ON  \
          ..
fi
set +e
cmake --build . -j$(nproc) || (separator && cmake --build . -j1 --verbose) || exit 1
set -e
cd ..

# Link the mdbook docs to doxygen
cd doc/src/
rm -f doxygen
ln -s ../../build-test/html doxygen
cd ../..

# Run the tests. We single out unit tests because these are hard to make sense
# of without full colored logs
cd build-test
set +e
./tests/unit_tests || (separator && UDIPE_LOG=trace ./tests/unit_tests) || exit 1
ctest -j$(nproc) -E '^unit_tests$' || (separator && UDIPE_LOG=trace ctest -V -E '^unit_tests$') || exit 1
set -e
cd ..

# Set up or update the benchmarking build
cd build-bench
if [[ ! -e CMakeCache.txt ]]; then
    cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo  \
          -DUDIPE_BUILD_BENCHMARKS=ON  \
          -DUDIPE_WERROR=ON  \
          ..
fi
set +e
cmake --build . -j$(nproc) || (separator && cmake --build . -j1 --verbose) || exit 1
set -e
cd ..

# Run all the benchmarks
cd build-bench/benches
for entry in $(ls); do
    if [[ -f ${entry} && -x ${entry} ]]; then
        set +e
        ./${entry} || (separator && UDIPE_LOG=trace ./${entry}) || exit 1
        set -e
    fi
done
cd ../..
