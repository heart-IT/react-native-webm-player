#!/usr/bin/env bash
# Mutation Calibration Script
# Applies domain-specific mutations one at a time, runs the test suite,
# and scores how many are caught (killed) vs survive undetected (blind spots).
#
# Usage: cd tests/sanitizer && ./run_mutations.sh [--quick]
#   --quick: skip TSan build (only ASan + temporal stress)
#
# Exit code: 0 if all mutations killed, 1 if any survived

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CPP_ROOT="$PROJECT_ROOT/cpp"
BUILD_DIR="$SCRIPT_DIR/build_mutation"

QUICK=false
[[ "${1:-}" == "--quick" ]] && QUICK=true

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

KILLED=0
SURVIVED=0
SKIPPED=0
TOTAL=0
SURVIVORS=()

log() { printf "  %-50s" "$1"; }
pass() { printf "${GREEN}KILLED${NC} (%s)\n" "$1"; KILLED=$((KILLED + 1)); }
fail() { printf "${RED}SURVIVED${NC}\n" "$1"; SURVIVED=$((SURVIVED + 1)); SURVIVORS+=("$2"); }
skip() { printf "${YELLOW}SKIPPED${NC} (%s)\n" "$1"; SKIPPED=$((SKIPPED + 1)); }

# Build once with ASan (for temporal stress + lsan tests)
build_asan() {
    mkdir -p "$BUILD_DIR/asan"
    cd "$BUILD_DIR/asan"
    cmake -DSANITIZER=address "$SCRIPT_DIR" -DCMAKE_BUILD_TYPE=Debug > /dev/null 2>&1
    make -j test_temporal_stress test_lsan 2>&1 | tail -1
    cd "$SCRIPT_DIR"
}

# Build once without sanitizer (for RT safety interposition tests)
build_none() {
    mkdir -p "$BUILD_DIR/none"
    cd "$BUILD_DIR/none"
    cmake -DSANITIZER=none "$SCRIPT_DIR" -DCMAKE_BUILD_TYPE=Debug > /dev/null 2>&1
    make -j test_rt_safety 2>&1 | tail -1
    cd "$SCRIPT_DIR"
}

# Build once with TSan
build_tsan() {
    mkdir -p "$BUILD_DIR/tsan"
    cd "$BUILD_DIR/tsan"
    cmake -DSANITIZER=thread "$SCRIPT_DIR" -DCMAKE_BUILD_TYPE=Debug > /dev/null 2>&1
    make -j test_tsan 2>&1 | tail -1
    cd "$SCRIPT_DIR"
}

# Run tests, return 0 if ANY test fails (= mutation killed)
run_tests() {
    local killed=false
    local reason=""

    # Temporal stress (fast, catches logic mutations)
    if ! "$BUILD_DIR/asan/test_temporal_stress" > /dev/null 2>&1; then
        killed=true
        reason="temporal-stress"
    fi

    # ASan tests (catches bounds mutations)
    if ! "$BUILD_DIR/asan/test_lsan" > /dev/null 2>&1; then
        killed=true
        reason="${reason:+$reason+}asan"
    fi

    # TSan tests (catches ordering/lock mutations)
    if ! $QUICK; then
        if ! "$BUILD_DIR/tsan/test_tsan" > /dev/null 2>&1; then
            killed=true
            reason="${reason:+$reason+}tsan"
        fi
    fi

    # RT safety (catches alloc/lock mutations in readSamples)
    if [[ -x "$BUILD_DIR/none/test_rt_safety" ]]; then
        if ! "$BUILD_DIR/none/test_rt_safety" > /dev/null 2>&1; then
            killed=true
            reason="${reason:+$reason+}rt-safety"
        fi
    fi

    if $killed; then
        echo "$reason"
        return 0
    else
        return 1
    fi
}

# Apply a find/replace mutation, rebuild, test, revert
# $3 = string to find (literal), $4 = string to replace with (literal), $5 = description
run_mutation() {
    local id="$1"
    local file="$2"
    local find_str="$3"
    local replace_str="$4"
    local description="$5"

    TOTAL=$((TOTAL + 1))
    log "$id: $description"

    local full_path="$PROJECT_ROOT/$file"

    # Backup
    cp "$full_path" "$full_path.bak"

    # Apply mutation via Python (reliable across macOS/Linux, handles special chars)
    python3 -c "
import sys
with open(sys.argv[1], 'r') as f: content = f.read()
new = content.replace(sys.argv[2], sys.argv[3], 1)
if new == content: sys.exit(1)
with open(sys.argv[1], 'w') as f: f.write(new)
" "$full_path" "$find_str" "$replace_str" 2>/dev/null

    # Verify file actually changed
    if diff -q "$full_path" "$full_path.bak" > /dev/null 2>&1; then
        mv "$full_path.bak" "$full_path"
        skip "pattern didn't match"
        return
    fi

    # Rebuild (suppress output)
    local build_ok=true
    (cd "$BUILD_DIR/asan" && make -j test_temporal_stress test_lsan 2>&1 | tail -1) > /dev/null 2>&1 || build_ok=false
    (cd "$BUILD_DIR/none" && make -j test_rt_safety 2>&1 | tail -1) > /dev/null 2>&1 || build_ok=false
    if ! $QUICK; then
        (cd "$BUILD_DIR/tsan" && make -j test_tsan 2>&1 | tail -1) > /dev/null 2>&1 || build_ok=false
    fi

    if ! $build_ok; then
        # Compile failure = mutation killed (static_assert, type error, etc.)
        mv "$full_path.bak" "$full_path"
        pass "compile-error"
        return
    fi

    # Run tests
    if reason=$(run_tests); then
        mv "$full_path.bak" "$full_path"
        pass "$reason"
    else
        mv "$full_path.bak" "$full_path"
        fail "" "$id: $description"
    fi

    # Rebuild clean (restore) — suppress output
    (cd "$BUILD_DIR/asan" && make -j test_temporal_stress test_lsan > /dev/null 2>&1) || true
    (cd "$BUILD_DIR/none" && make -j test_rt_safety > /dev/null 2>&1) || true
    if ! $QUICK; then
        (cd "$BUILD_DIR/tsan" && make -j test_tsan > /dev/null 2>&1) || true
    fi
}

# ============================================================
# Main
# ============================================================

printf "\n${YELLOW}Mutation Calibration${NC}\n"
printf "====================\n\n"

printf "Building test suite...\n"
build_asan
build_none
if ! $QUICK; then
    build_tsan
fi
printf "Build complete.\n\n"

# --- Category 1: Memory Ordering Weakening ---
printf "Category 1: Memory Ordering Weakening\n"

run_mutation "M01" "cpp/playback/AudioDecodeChannel.h" \
    "state_.store(StreamState::Buffering, std::memory_order_release);" \
    "state_.store(StreamState::Buffering, std::memory_order_relaxed);" \
    "activate() state release→relaxed"

run_mutation "M05" "cpp/playback/AudioDecodeChannel.h" \
    "encodedClearRequested_.store(true, std::memory_order_release);" \
    "encodedClearRequested_.store(true, std::memory_order_relaxed);" \
    "encodedClearRequested release→relaxed"

# --- Category 5: Logic Errors ---
printf "\nCategory 5: Logic Errors\n"

run_mutation "M17" "cpp/common/AVSyncCoordinator.h" \
    "kDeadZoneUs = 15000" \
    "kDeadZoneUs = 0" \
    "A/V sync dead zone → 0"

run_mutation "M18" "cpp/common/MediaConfig.h" \
    "kMaxConsecutivePLC = 8" \
    "kMaxConsecutivePLC = 0" \
    "PLC limit → 0 (kill PLC)"

run_mutation "M20" "cpp/playback/DriftCompensator.h" \
    "kRateDenom = 99991" \
    "kRateDenom = 99990" \
    "prime denominator → composite"

# --- Category 6: Video Pipeline ---
printf "\nCategory 6: Video Pipeline\n"

run_mutation "V-M01" "cpp/video/VideoFrameQueue.h" \
    "if (needsKeyFrame_ && !isKeyFrame) {" \
    "if (false && needsKeyFrame_ && !isKeyFrame) {" \
    "disable keyframe gating"

run_mutation "V-M02" "cpp/video/VideoFrameQueue.h" \
    "if (encodedQueue_.size() >= video_config::kDecodeQueueDepth) {" \
    "if (false) {" \
    "disable queue depth cap"

run_mutation "V-M03" "cpp/video/VideoConfig.h" \
    "kLateFrameThresholdUs = 16667" \
    "kLateFrameThresholdUs = 0" \
    "late-frame threshold → 0 (everything is late)"

run_mutation "V-M04" "cpp/video/VideoFrameQueue.h" \
    "encodedQueue_.clear();
    }" \
    "// encodedQueue_.clear();
    }" \
    "requestKeyFrame() skips queue clear"

# --- Category 7: Lock Removal ---
printf "\nCategory 7: Lock Removal\n"

run_mutation "M07" "cpp/playback/AudioDecodeChannel.h" \
    "std::unique_lock<std::mutex> lk(decoderMtx_, std::try_to_lock);" \
    "std::unique_lock<std::mutex> lk(decoderMtx_);" \
    "processPendingDecode try_lock→lock"

run_mutation "M08" "cpp/playback/AudioDecodeChannel.h" \
    "    void activate() noexcept {
        std::lock_guard<std::mutex> lk(decoderMtx_);" \
    "    void activate() noexcept {
        // std::lock_guard<std::mutex> lk(decoderMtx_);" \
    "activate() remove lock_guard"

run_mutation "M09" "cpp/playback/AudioDecodeChannel.h" \
    "        std::lock_guard<std::mutex> lk(decoderMtx_);
        decoder_ = std::move(decoder);" \
    "        // std::lock_guard<std::mutex> lk(decoderMtx_);
        decoder_ = std::move(decoder);" \
    "setDecoder() remove lock_guard"

# --- Category 8: RT Safety Violations ---
# M13 (std::mutex) and M14 (MEDIA_LOG_D) cannot be detected at runtime on macOS:
#   M13: os_unfair_lock has no public interposition hook
#   M14: os_log uses compiler builtins (__builtin_os_log_format)
# These require static analysis or Linux CI with different lock/log implementations.
printf "\nCategory 8: RT Safety Violations\n"

run_mutation "M15" "cpp/playback/AudioDecodeChannel.h" \
    "if (!isActive()) return 0;" \
    "{ std::vector<float> _v15(960); _v15[0] = 1.0f; } if (!isActive()) return 0;" \
    "inject heap vector in readSamples()"

run_mutation "M16" "cpp/playback/AudioDecodeChannel.h" \
    "if (!isActive()) return 0;" \
    "{ float* _lk16 = new float[960]; _lk16[0] = 1.0f; delete[] _lk16; } if (!isActive()) return 0;" \
    "inject heap new[] in readSamples()"

# --- Summary ---
printf "\n====================\n"
printf "Mutations tested: %d\n" "$TOTAL"
printf "Killed:           ${GREEN}%d${NC}\n" "$KILLED"
printf "Survived:         ${RED}%d${NC}\n" "$SURVIVED"
printf "Skipped:          ${YELLOW}%d${NC}\n" "$SKIPPED"

if [[ $TOTAL -gt 0 ]]; then
    TESTABLE=$((TOTAL - SKIPPED))
    if [[ $TESTABLE -gt 0 ]]; then
        PCT=$((KILLED * 100 / TESTABLE))
        printf "Effectiveness:    %d%%\n" "$PCT"
    fi
fi

if [[ ${#SURVIVORS[@]} -gt 0 ]]; then
    printf "\n${RED}BLIND SPOTS:${NC}\n"
    for s in "${SURVIVORS[@]}"; do
        printf "  - %s\n" "$s"
    done
fi

printf "\nNote: M03/M04 (FrameQueue ordering) require multi-line edits.\n"
printf "M13 (os_unfair_lock) and M14 (os_log builtins) are not runtime-detectable\n"
printf "on macOS — require static analysis or Linux CI.\n"

exit $SURVIVED
