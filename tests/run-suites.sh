#!/bin/sh
# The single list of test binaries. package.json's test:asan/tsan/ubsan and
# .github/workflows/ci.yml both run suites through this script — the lists had
# drifted before (CI silently skipped test_demux_hostile and test_demux_lacing,
# the suites that caught R3-23), so no caller may enumerate binaries itself.
set -eu

BUILD_DIR="${1:?usage: run-suites.sh <build-dir>}"

for suite in \
  test_stream_buffer \
  test_e2e_demux \
  test_audit_proof \
  test_ring_registry \
  test_ring_lifetime \
  test_demux_hostile \
  test_demux_lacing \
  fuzz_demuxer; do
  echo "=== ${suite} ==="
  "${BUILD_DIR}/${suite}"
done
