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
make -j$(nproc) || (clear && make)
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
./tests/unit_tests || (clear && UDIPE_LOG=trace ./tests/unit_tests) || exit 1
ctest -j$(nproc) -E '^unit_tests$' || (clear && UDIPE_LOG=trace ctest -V -E '^unit_tests$') || exit 1
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
make -j$(nproc) || (clear && make)
set -e
cd ..

# Run all the benchmarks
cd build-bench/benches
for entry in $(ls); do
    if [[ -f ${entry} && -x ${entry} ]]; then
        ./${entry} || UDIPE_LOG=trace ./${entry}
    fi
done
cd ../..
