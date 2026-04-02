#!/bin/bash
# Download a pre-converted whisper.cpp GGML model for on-device transcription.
# Usage: ./scripts/download-whisper-model.sh [model-name]
#
# Models (smallest → largest):
#   tiny.en     (~75MB, English-only, fastest)
#   tiny        (~75MB, multilingual)
#   base.en     (~142MB, English-only)
#   base        (~142MB, multilingual)
#
# Default: tiny.en (recommended for mobile — smallest with good English accuracy)
#
# Output:
#   Android: android/src/main/assets/ggml-<model>.bin
#   iOS:     ios/whisper/models/ggml-<model>.bin

set -euo pipefail

MODEL="${1:-tiny.en}"
FILENAME="ggml-${MODEL}.bin"
URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/${FILENAME}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

ANDROID_DIR="${PROJECT_DIR}/android/src/main/assets"
IOS_DIR="${PROJECT_DIR}/ios/whisper/models"

mkdir -p "${ANDROID_DIR}" "${IOS_DIR}"

echo "Downloading ${FILENAME} from Hugging Face..."
echo "URL: ${URL}"
echo ""

# Download to a temp file first
TMPFILE=$(mktemp)
trap "rm -f ${TMPFILE}" EXIT

curl -L --progress-bar -o "${TMPFILE}" "${URL}"

SIZE=$(wc -c < "${TMPFILE}" | tr -d ' ')
echo ""
echo "Downloaded: ${SIZE} bytes"

# Copy to both platform directories
cp "${TMPFILE}" "${ANDROID_DIR}/${FILENAME}"
cp "${TMPFILE}" "${IOS_DIR}/${FILENAME}"

echo ""
echo "Installed to:"
echo "  Android: android/src/main/assets/${FILENAME}"
echo "  iOS:     ios/whisper/models/${FILENAME}"
echo ""
echo "Usage in JS:"
echo "  // Android"
echo "  MediaPipeline.setTranscriptionEnabled(true, '${FILENAME}')"
echo "  // iOS"
echo "  MediaPipeline.setTranscriptionEnabled(true, bundlePath + '/${FILENAME}')"
