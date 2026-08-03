#!/usr/bin/env bash

set -euo pipefail

BUILD_DIRECTORY=${1:-build}
CTEST_EXCLUDE='mongoc/fixtures/fake_kms_provider_server/start|mongoc/CMake/bare-bson-import|mongoc/CMake/bare-mongoc-import|mongoc/CMake/bson-import-1.0|mongoc/CMake/bson-import-range-upper|mongoc/CMake/bson-import-range-lower|mongoc/CMake/bson-import-major-range|mongoc/CMake/bson-import-opt-components'

ctest --test-dir "${BUILD_DIRECTORY}" -E "${CTEST_EXCLUDE}" --output-on-failure
