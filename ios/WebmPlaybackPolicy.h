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

// Consecutive dataless sessions before the player reports Failed.
constexpr int kMaxDatalessSessions = 5;

// Delay before the next rejoin. A session that delivered data resets the
// count; the delay grows only across consecutive sessions that delivered
// nothing.
inline double WebmRejoinDelaySeconds(int datalessSessions) {
  constexpr double kDelaysSec[] = {0.5, 1.0, 2.0, 4.0};
  constexpr int kLast = sizeof(kDelaysSec) / sizeof(kDelaysSec[0]) - 1;
  return kDelaysSec[datalessSessions < kLast ? datalessSessions : kLast];
}
