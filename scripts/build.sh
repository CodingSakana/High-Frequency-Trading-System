#!/bin/bash

echo ""
cd ~/CodingFiles/High-Frequency-Trading-System
mkdir -p build-release && cd build-release

cmake -DPERF_TEST=OFF -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

cd ..
mkdir -p build-debug && cd build-debug

cmake -DPERF_TEST=OFF -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)

cd ..
mkdir -p build-perf-test && cd build-perf-test

cmake -DPERF_TEST=ON -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)