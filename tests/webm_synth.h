// Hand-assembled minimal WebM byte sequences shared by the demux tests and
// the fuzzer seed. Every declared element size below is exact — the fuzzer
// asserts at startup that kMinimalWebmHeader alone reaches
// ParseState::Streaming, so a size that rots breaks the build loudly (R3-6).
#pragma once

#include <cstdint>
#include <cstddef>

namespace webm_synth {

// EBML header + Segment (unknown size) + Info + Tracks with one Opus audio
// track. The smallest byte sequence that gets the demuxer into Streaming.
static const uint8_t kMinimalWebmHeader[] = {
    // EBML Header, body = 31 bytes
    0x1A, 0x45, 0xDF, 0xA3,
    0x9F,
    0x42, 0x86, 0x81, 0x01,  // EBMLVersion = 1
    0x42, 0xF7, 0x81, 0x01,  // EBMLReadVersion = 1
    0x42, 0xF2, 0x81, 0x04,  // EBMLMaxIDLength = 4
    0x42, 0xF3, 0x81, 0x08,  // EBMLMaxSizeLength = 8
    0x42, 0x82, 0x84, 0x77, 0x65, 0x62, 0x6D,  // DocType = "webm"
    0x42, 0x87, 0x81, 0x04,  // DocTypeVersion = 4
    0x42, 0x85, 0x81, 0x02,  // DocTypeReadVersion = 2
    // Segment, unknown size (live stream)
    0x18, 0x53, 0x80, 0x67,
    0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    // Info, body = 7 bytes (no Duration: mkvparser requires a 4- or 8-byte
    // float there, and a live stream has none)
    0x15, 0x49, 0xA9, 0x66,
    0x87,
    0x2A, 0xD7, 0xB1, 0x83, 0x0F, 0x42, 0x40,  // TimecodeScale = 1000000
    // Tracks, body = 35 bytes
    0x16, 0x54, 0xAE, 0x6B,
    0xA3,
    // TrackEntry, body = 33 bytes
    0xAE, 0xA1,
    0xD7, 0x81, 0x01,        // TrackNumber = 1
    0x73, 0xC5, 0x81, 0x01,  // TrackUID = 1
    0x83, 0x81, 0x02,        // TrackType = audio (2)
    0x86, 0x86, 0x41, 0x5F, 0x4F, 0x50, 0x55, 0x53,  // CodecID = "A_OPUS"
    // Audio, body = 13 bytes
    0xE1, 0x8D,
    0xB5, 0x84, 0x47, 0x3B, 0x80, 0x00,  // SamplingFrequency = 48000.0f
    0x9F, 0x81, 0x02,        // Channels = 2
    0x62, 0x64, 0x81, 0x10,  // BitDepth = 16
};

// Offset of the SamplingFrequency float's first payload byte (0x47, the
// exponent byte of 48000.0f) inside kMinimalWebmHeader — for tests that
// corrupt it into a huge value (R3-20's repro).
inline constexpr size_t kSamplingRateExponentOffset =
    sizeof(kMinimalWebmHeader) - 11;

// One cluster (timecode 0, known size) holding a single Xiph-laced
// SimpleBlock with 3 frames of distinct sizes and payloads.
static const uint8_t kXiphLacedCluster[] = {
    // Cluster, body = 24 bytes
    0x1F, 0x43, 0xB6, 0x75,
    0x98,
    0xE7, 0x81, 0x00,        // Timecode = 0
    // SimpleBlock, body = 19 bytes
    0xA3, 0x93,
    0x81,                    // track 1
    0x00, 0x00,              // relative timecode 0
    0x82,                    // flags: keyframe | Xiph lacing
    0x02,                    // lace count - 1 (3 frames)
    0x03,                    // frame 1 size
    0x05,                    // frame 2 size (frame 3 size implied)
    0x01, 0x02, 0x03,                    // frame 1
    0x11, 0x12, 0x13, 0x14, 0x15,        // frame 2
    0x21, 0x22, 0x23, 0x24,              // frame 3
};

// Same cluster shape with a later timecode and different payloads, so a
// stream can carry two distinct clusters.
static const uint8_t kXiphLacedCluster2[] = {
    0x1F, 0x43, 0xB6, 0x75,
    0x98,
    0xE7, 0x81, 0x14,        // Timecode = 20 (ms at the default scale)
    0xA3, 0x93,
    0x81,
    0x00, 0x00,
    0x82,
    0x02,
    0x03,
    0x05,
    0x31, 0x32, 0x33,
    0x41, 0x42, 0x43, 0x44, 0x45,
    0x51, 0x52, 0x53, 0x54,
};

// A cluster header declaring a 3 MiB body — larger than the demuxer's 2 MiB
// parse window — followed by a Timecode and one complete SimpleBlock.
// mkvparser only surfaces a cluster once HasBlockEntries() has seen a real
// block, so the prefix carries one; the rest of the declared body never all
// fits, and a correct demuxer must surface a permanent error instead of
// wedging (R3-5).
static const uint8_t kOversizedClusterPrefix[] = {
    0x1F, 0x43, 0xB6, 0x75,
    0x10, 0x30, 0x00, 0x00,  // 4-byte vint: body size = 0x300000 (3 MiB)
    0xE7, 0x81, 0x28,        // Timecode = 40
    // SimpleBlock, body = 6 bytes
    0xA3, 0x86,
    0x81,                    // track 1
    0x00, 0x00,              // relative timecode 0
    0x80,                    // flags: keyframe, no lacing
    0xAA, 0xBB,              // payload
};

}  // namespace webm_synth
