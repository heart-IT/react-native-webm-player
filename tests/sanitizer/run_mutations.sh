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

printf "\nNote: M03/M04 (FrameQueue ordering), M07-M09 (lock removal),\n"
printf "M10-M12 (bounds removal), M13-M16 (RT violations), M19 (invert catchup)\n"
printf "require multi-line edits — run these manually or extend this script.\n"

exit $SURVIVED
