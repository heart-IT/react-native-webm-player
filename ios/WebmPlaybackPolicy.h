// Playback policy numbers, shared in spirit with Android's constants.
#pragma once

#include <cstddef>

// How far behind the live edge the synchronizer's clock starts. The demux
// thread paces on the decoders' queues, so this margin accumulates in the ring
// as compressed bytes; without that pacing the decoders' queues overflowed at
// 2 s and 513 frames were dropped on the iPhone. The iPhone's path to the
// broadcaster delivered in bursts with gaps over 2 s; emulating 3 s gaps on the
// simulator gave four underruns a minute at 1.5 s and none at 4 s. The
// worklet's lag threshold must stay well above this delay plus the queues.
constexpr double kLiveDelaySec = 4.0;

// What the ring holds: the live delay above plus whatever backpressure lets
// accumulate before the worklet declares lag (10 s, worklet/feeder.js). 14 s of
// a 1.2 Mbps stream fits; the fixture is 211 kbps. The pump's 5 s summary logs
// the occupancy as "ring %".
constexpr size_t kRingCapacityBytes = 2u * 1024 * 1024;

// Rejoins in a row that produced no data before the player reports Failed:
// the fifth such rejoin's session ending without a byte fails the player, as
// Android's REJOIN_BACKOFF_MS.size does.
constexpr int kMaxDatalessRejoins = 5;

// Delay before the next rejoin, indexed by the rejoins already made since the
// last session that delivered data (0 after play() or after a session that
// delivered): 0.5, 1, 2, 4, 4 s, Android's REJOIN_BACKOFF_MS. The index is
// read before the count is incremented, so the first wait is 0.5 s.
inline double WebmRejoinDelaySeconds(int datalessRejoins) {
  constexpr double kDelaysSec[] = {0.5, 1.0, 2.0, 4.0};
  constexpr int kLast = sizeof(kDelaysSec) / sizeof(kDelaysSec[0]) - 1;
  return kDelaysSec[datalessRejoins < kLast ? datalessRejoins : kLast];
}
