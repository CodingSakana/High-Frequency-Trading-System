#!/bin/bash
set -euo pipefail

PROJECT_ROOT=~/CodingFiles/High-Frequency-Trading-System

build() {
  local name=$1
  local perf_flag=$2
  local build_type=$3

  echo -e "\n=== Building ${name} (PERF_TEST=${perf_flag}, BUILD_TYPE=${build_type}) ==="
  rm -rf "${PROJECT_ROOT}/${name}"
  mkdir -p "${PROJECT_ROOT}/${name}"
  pushd "${PROJECT_ROOT}/${name}" > /dev/null

  cmake -G Ninja -DPERF_TEST=${perf_flag} -DCMAKE_BUILD_TYPE=${build_type} ..
  ninja -j$(nproc)

  popd > /dev/null
}

build build-release    OFF Release
build build-debug      OFF Debug
build build-perf-test  ON  Release

echo -e "\nAll builds completed!"
