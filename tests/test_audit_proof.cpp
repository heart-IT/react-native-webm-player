// Proof tests for audit findings.
//
// Each test here asserts the behaviour the system is SUPPOSED to have, so it
// fails against the current code. A proof test that passes on first run proves
// nothing — see tests/test_stream_buffer.cpp's wraparound case, which passed
// under three sanitizers while testing nothing, until mutation testing exposed
// the aligned-chunk blind spot.
//
// Verdicts are recorded in docs/AUDIT.md.

#include "OpusChannelMapping.h"
#include "WebMStreamBuffer.h"

#include <vector>

#include "MediaLog.h"
#include "test_common.h"

using media::WebMStreamBuffer;

namespace {

struct SilenceLogs {
  SilenceLogs() {
    media::log::minLevel().store(media::log::Level::Error,
                                 std::memory_order_relaxed);
  }
};
SilenceLogs g_silence_logs;

constexpr size_t kCapacity = 8u << 20;

WebMStreamBuffer::Config testConfig() {
  WebMStreamBuffer::Config cfg;
  cfg.minCapacityBytes = 0;
  return cfg;
}

std::vector<uint8_t> patternOf(size_t n, uint8_t value) {
  return std::vector<uint8_t>(n, value);
}

size_t fill(WebMStreamBuffer& buf, size_t bytes) {
  const size_t kChunk = 64 * 1024;
  auto chunk = patternOf(kChunk, 0xAA);
  size_t written = 0;
  while (written < bytes) {
    size_t want = std::min(kChunk, bytes - written);
    size_t got = buf.write(chunk.data(), want, false);
    if (got == 0) break;
    written += got;
  }
  return written;
}

}  // namespace

// ---------------------------------------------------------------------------
// P0-2: a short write must not split a caller's chunk.
//
// WebMStreamBuffer feeds a container parser. The bytes are a stream, so writing
// only part of a chunk leaves a hole: the next write concatenates directly onto
// a truncated EBML element and the demuxer misparses from there.
//
// The predecessor's IngestRingBuffer dropped the whole chunk for exactly this
// reason, and the project's drop-policy table states it: "partial bytes already
// in the ring belong to in-progress parses; dropping them mid-frame would wedge
// the demuxer."
// ---------------------------------------------------------------------------

TEST(overflow_must_reject_a_chunk_whole_rather_than_splitting_it) {
  WebMStreamBuffer buf(1024, testConfig());
  const size_t kHeadroom = 10;
  ASSERT_EQ(fill(buf, kCapacity - kHeadroom), kCapacity - kHeadroom);

  auto chunk = patternOf(100, 0xBB);
  size_t wrote = buf.write(chunk.data(), chunk.size(), false);

  // All-or-nothing is the required contract. Currently returns 10.
  ASSERT_TRUE(wrote == 0 || wrote == chunk.size());
}

// Demonstrates the corruption the split produces, independent of the contract
// above: after a truncated write, the following chunk's bytes sit immediately
// after the fragment, so a reader cannot tell where one ended and the next began.
// The consequence of the split, measured directly: drain the whole ring and
// count how many of the rejected chunk's bytes are sitting in the stream. Either
// none of it was accepted, or all of it was — a fragment means the demuxer will
// read a truncated element.
TEST(a_partially_written_chunk_leaves_a_fragment_in_the_stream) {
  WebMStreamBuffer buf(1024, testConfig());
  const size_t kHeadroom = 10;
  ASSERT_EQ(fill(buf, kCapacity - kHeadroom), kCapacity - kHeadroom);

  auto chunk = patternOf(100, 0xA1);  // filler is 0xAA, so 0xA1 is unambiguous
  buf.write(chunk.data(), chunk.size(), false);

  size_t fragmentBytes = 0;
  std::vector<uint8_t> sink(64 * 1024);
  while (true) {
    int n = buf.read(sink.data(), sink.size(), 50);
    if (n <= 0) break;
    for (int i = 0; i < n; i++)
      if (sink[static_cast<size_t>(i)] == 0xA1) fragmentBytes++;
  }

  ASSERT_TRUE(fragmentBytes == 0 || fragmentBytes == chunk.size());
}

// ---------------------------------------------------------------------------
// P6-1: multichannel Opus could not be decoded at all.
//
// Found by running the example on a simulator: the 5.1 fixture logged
// "opus init failed (48000 Hz, 6 ch, err -1)" on every packet and produced
// silence. opus_decoder_create rejects channels > 2; multichannel is carried as
// several streams plus a mapping table in the OpusHead and needs the
// multistream decoder.
// ---------------------------------------------------------------------------

TEST(opus_head_family_one_yields_a_multistream_layout) {
  // Real 5.1 OpusHead from tests/fixtures/bbb_480p_vp9_opus_1second.webm.
  const uint8_t head[] = {
      'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
      1,           // version
      6,           // channels
      0x38, 0x01,  // pre-skip 312
      0x80, 0xBB, 0x00, 0x00,  // 48000
      0x00, 0x00,  // output gain
      1,           // mapping family
      4, 2,        // streams, coupled streams
      0, 4, 1, 2, 3, 5,  // channel mapping
  };
  auto m = media::parseOpusHead(head, sizeof(head));
  ASSERT_TRUE(m.valid);
  ASSERT_EQ(m.channels, 6);
  ASSERT_EQ(m.mappingFamily, 1);
  ASSERT_EQ(m.streams, 4);
  ASSERT_EQ(m.coupledStreams, 2);
  ASSERT_TRUE(m.needsMultistream());
}

TEST(opus_head_family_zero_stereo_stays_on_the_plain_decoder) {
  const uint8_t head[] = {
      'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
      1, 2, 0x38, 0x01, 0x80, 0xBB, 0x00, 0x00, 0x00, 0x00,
      0,  // mapping family 0
  };
  auto m = media::parseOpusHead(head, sizeof(head));
  ASSERT_TRUE(m.valid);
  ASSERT_EQ(m.channels, 2);
  ASSERT_EQ(m.streams, 1);
  ASSERT_EQ(m.coupledStreams, 1);
  ASSERT_FALSE(m.needsMultistream());
}

TEST(opus_head_rejects_truncated_and_foreign_headers) {
  const uint8_t notOpus[19] = {'N', 'o', 't', 'H', 'e', 'a', 'd', '!', 1, 2};
  ASSERT_FALSE(media::parseOpusHead(notOpus, sizeof(notOpus)).valid);

  // Family 1 declaring 6 channels but no room for the mapping table.
  const uint8_t truncated[] = {
      'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
      1, 6, 0, 0, 0x80, 0xBB, 0, 0, 0, 0, 1, 4, 2,
  };
  ASSERT_FALSE(media::parseOpusHead(truncated, sizeof(truncated)).valid);
  ASSERT_FALSE(media::parseOpusHead(nullptr, 0).valid);
}

TEST_MAIN("Audit Proof Tests")
