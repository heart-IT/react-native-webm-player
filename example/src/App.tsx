import React, { useState, useCallback, useEffect, useRef } from 'react'
import {
  StyleSheet,
  View,
  Text,
  TouchableOpacity,
  ScrollView,
  SafeAreaView,
  AppRegistry,
  StatusBar,
  NativeModules,
  Platform
} from 'react-native'
import {
  MediaPipeline,
  VideoView,
  AudioRoute,
  StreamHealth,
  CatchupPolicy,
  PlaybackState,
  installWebmPlayer,
  type AudioRouteChangeEvent,
  type AudioDeviceInfo,
  type HealthEvent,
  type TrackInfo,
  type PlayerMetrics
} from '@heartit/webm-player'

const healthLabel = (status: StreamHealth): string => {
  switch (status) {
    case StreamHealth.Healthy:
      return 'Healthy'
    case StreamHealth.Buffering:
      return 'Buffering'
    case StreamHealth.Degraded:
      return 'Degraded'
    case StreamHealth.Stalled:
      return 'Stalled'
    case StreamHealth.Failed:
      return 'Failed'
    default:
      return 'Unknown'
  }
}

const healthColor = (status: StreamHealth): string => {
  switch (status) {
    case StreamHealth.Healthy:
      return '#2ecc71'
    case StreamHealth.Buffering:
      return '#f39c12'
    case StreamHealth.Degraded:
      return '#e67e22'
    case StreamHealth.Stalled:
      return '#e74c3c'
    case StreamHealth.Failed:
      return '#c0392b'
    default:
      return '#888'
  }
}

const playbackStateLabel = (state: PlaybackState): string => {
  switch (state) {
    case PlaybackState.Idle:
      return 'Idle'
    case PlaybackState.Buffering:
      return 'Buffering'
    case PlaybackState.Playing:
      return 'Playing'
    case PlaybackState.Paused:
      return 'Paused'
    case PlaybackState.Stalled:
      return 'Stalled'
    case PlaybackState.Failed:
      return 'Failed'
    default:
      return 'Unknown'
  }
}

const catchupPolicyLabel = (policy: CatchupPolicy): string => {
  switch (policy) {
    case CatchupPolicy.PlayThrough:
      return 'PlayThrough'
    case CatchupPolicy.Accelerate:
      return 'Accelerate'
    case CatchupPolicy.DropToLive:
      return 'DropToLive'
    default:
      return 'Unknown'
  }
}

const levelMeter = (dbfs: number, width: number = 20): string => {
  const clamped = Math.max(-60, Math.min(0, dbfs))
  const filled = Math.round(((clamped + 60) / 60) * width)
  return '[' + '#'.repeat(filled) + '-'.repeat(width - filled) + ']'
}

const routeName = (route: AudioRoute): string => {
  switch (route) {
    case AudioRoute.Earpiece:
      return 'Earpiece'
    case AudioRoute.Speaker:
      return 'Speaker'
    case AudioRoute.WiredHeadset:
      return 'Wired'
    case AudioRoute.BluetoothA2dp:
      return 'BT A2DP'
    case AudioRoute.BluetoothSco:
      return 'BT SCO'
    case AudioRoute.UsbDevice:
      return 'USB'
    default:
      return 'Unknown'
  }
}

function App(): React.ReactElement {
  const [installed, setInstalled] = useState(false)
  const [running, setRunning] = useState(false)
  const [muted, setMuted] = useState(false)
  const [gain, setGain] = useState(1.0)
  const [currentRoute, setCurrentRoute] = useState<AudioRoute>(AudioRoute.Speaker)
  const [availableDevices, setAvailableDevices] = useState<AudioDeviceInfo[]>([])
  const [healthStatus, setHealthStatus] = useState<StreamHealth>(StreamHealth.Healthy)
  const [healthDetail, setHealthDetail] = useState('')
  const [trackInfo, setTrackInfo] = useState<TrackInfo | null>(null)
  const [metrics, setMetrics] = useState<PlayerMetrics | null>(null)
  const [paused, setPaused] = useState(false)
  const [playbackRate, setPlaybackRate] = useState(1.0)
  const [catchupPolicy, setCatchupPolicy] = useState<CatchupPolicy>(CatchupPolicy.Accelerate)
  const [playbackState, setPlaybackState] = useState<PlaybackState>(PlaybackState.Idle)
  const [bufferRange, setBufferRange] = useState(0)
  const [clipPath, setClipPath] = useState<string | null>(null)
  const [clipCapturing, setClipCapturing] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const metricsTimer = useRef<ReturnType<typeof setInterval> | null>(null)

  const runV2Spike = useCallback(async () => {
    if (Platform.OS !== 'ios') {
      setError('V2 spike is iOS-only')
      return
    }
    const bridge = (NativeModules as { V2SpikeBridge?: { runV2Spike: () => Promise<{ result: string; error?: string }> } }).V2SpikeBridge
    if (!bridge?.runV2Spike) {
      setError('V2SpikeBridge not available — rebuild iOS pods')
      return
    }
    try {
      const r = await bridge.runV2Spike()
      setError(r.error ? `Spike ${r.result}: ${r.error}` : `Spike result: ${r.result}`)
    } catch (e) {
      setError(`Spike threw: ${String(e)}`)
    }
  }, [])

  const doInstall = useCallback(() => {
    try {
      installWebmPlayer()
      setInstalled(true)
      setError(null)
    } catch (e) {
      setError(String(e))
    }
  }, [])

  const doWarmUp = useCallback(() => {
    try {
      MediaPipeline.warmUp()
      setError(null)
    } catch (e) {
      setError(String(e))
    }
  }, [])

  const doStart = useCallback(() => {
    try {
      MediaPipeline.start()

      MediaPipeline.setAudioRouteCallback((event: AudioRouteChangeEvent) => {
        setCurrentRoute(event.route)
        setAvailableDevices(event.availableDevices)
      })

      MediaPipeline.setHealthCallback((event: HealthEvent) => {
        setHealthStatus(event.status)
        setHealthDetail(event.detail)
      })

      MediaPipeline.setKeyFrameNeededCallback(() => {
        // In a real app, this would signal the Hypercore source
        // to seek to the nearest keyframe.
      })

      setRunning(true)
      setError(null)
    } catch (e) {
      setError(String(e))
    }
  }, [])

  const doStop = useCallback(() => {
    try {
      MediaPipeline.setHealthCallback(null)
      MediaPipeline.setKeyFrameNeededCallback(null)
      MediaPipeline.stop()
      setRunning(false)
      setMetrics(null)
      setTrackInfo(null)
      setHealthStatus(StreamHealth.Healthy)
      setHealthDetail('')
    } catch (e) {
      setError(String(e))
    }
  }, [])

  const doPause = useCallback(() => {
    try {
      MediaPipeline.pause()
      setPaused(true)
    } catch (e) {
      setError(String(e))
    }
  }, [])

  const doResume = useCallback(() => {
    try {
      MediaPipeline.resume()
      setPaused(false)
    } catch (e) {
      setError(String(e))
    }
  }, [])

  const changePlaybackRate = useCallback((rate: number) => {
    try {
      MediaPipeline.setPlaybackRate(rate)
      setPlaybackRate(rate)
    } catch (e) {
      setError(String(e))
    }
  }, [])

  const changeCatchupPolicy = useCallback((policy: CatchupPolicy) => {
    try {
      MediaPipeline.setCatchupPolicy(policy)
      setCatchupPolicy(policy)
    } catch (e) {
      setError(String(e))
    }
  }, [])

  const doCapture = useCallback(() => {
    setClipCapturing(true)
    setClipPath(null)
    MediaPipeline.captureClip(10)
      .then((path) => {
        setClipPath(path)
        setClipCapturing(false)
      })
      .catch((e) => {
        setError(String(e))
        setClipCapturing(false)
      })
  }, [])

  const doRewind = useCallback(() => {
    try {
      MediaPipeline.seekTo(-10)
      setError(null)
    } catch (e) {
      setError(String(e))
    }
  }, [])

  const doGoLive = useCallback(() => {
    try {
      MediaPipeline.seekTo(0)
      setError(null)
    } catch (e) {
      setError(String(e))
    }
  }, [])

  const toggleMute = useCallback(() => {
    try {
      const next = !muted
      MediaPipeline.setMuted(next)
      setMuted(next)
    } catch (e) {
      setError(String(e))
    }
  }, [muted])

  const onGainChange = useCallback((value: number) => {
    try {
      MediaPipeline.setGain(value)
      setGain(value)
    } catch (e) {
      setError(String(e))
    }
  }, [])

  const setRoute = useCallback((route: AudioRoute) => {
    try {
      MediaPipeline.setAudioRoute(route)
    } catch (e) {
      setError(String(e))
    }
  }, [])

  useEffect(() => {
    if (running) {
      metricsTimer.current = setInterval(() => {
        try {
          const m = MediaPipeline.getMetrics()
          setMetrics(m)
          const info = MediaPipeline.getTrackInfo()
          if (info) setTrackInfo(info)
          setPlaybackState(MediaPipeline.getPlaybackState())
          setBufferRange(MediaPipeline.getBufferRangeSeconds())
        } catch {
          // ignore
        }
      }, 1000)
    } else if (metricsTimer.current) {
      clearInterval(metricsTimer.current)
      metricsTimer.current = null
    }
    return () => {
      if (metricsTimer.current) clearInterval(metricsTimer.current)
    }
  }, [running])

  return (
    <SafeAreaView style={styles.container}>
      <StatusBar barStyle='light-content' />
      <ScrollView contentContainerStyle={styles.scroll}>
        <Text style={styles.title}>WebM Player</Text>

        {error && <Text style={styles.error}>{error}</Text>}

        {/* Health status bar */}
        {running && (
          <View style={[styles.healthBar, { backgroundColor: healthColor(healthStatus) }]}>
            <Text style={styles.healthText}>
              {healthLabel(healthStatus)}
              {healthDetail ? ` — ${healthDetail}` : ''}
            </Text>
          </View>
        )}

        {/* V2 spike (iOS only — validates AVSampleBufferRenderSynchronizer architecture) */}
        {Platform.OS === 'ios' && (
          <View style={styles.section}>
            <Text style={styles.sectionTitle}>V2 Architecture Spike</Text>
            <TouchableOpacity style={styles.button} onPress={runV2Spike}>
              <Text style={styles.buttonText}>Run V2 Spike</Text>
            </TouchableOpacity>
            <Text style={styles.metricText}>
              Plays bbb_480p_vp9_opus_1second.webm via libwebm + libopus + VTDecompression →
              AVSampleBufferRenderSynchronizer. Independent of v1 pipeline.
            </Text>
          </View>
        )}

        {/* Lifecycle */}
        <View style={styles.section}>
          <Text style={styles.sectionTitle}>Lifecycle</Text>

          {!installed && (
            <TouchableOpacity style={styles.button} onPress={doInstall}>
              <Text style={styles.buttonText}>Install JSI</Text>
            </TouchableOpacity>
          )}

          {installed && !running && (
            <View style={styles.buttonRow}>
              <TouchableOpacity style={[styles.button, styles.flexButton]} onPress={doWarmUp}>
                <Text style={styles.buttonText}>Warm Up</Text>
              </TouchableOpacity>
              <TouchableOpacity style={[styles.button, styles.flexButton]} onPress={doStart}>
                <Text style={styles.buttonText}>Start</Text>
              </TouchableOpacity>
            </View>
          )}

          {running && (
            <TouchableOpacity style={[styles.button, styles.stopButton]} onPress={doStop}>
              <Text style={styles.buttonText}>Stop</Text>
            </TouchableOpacity>
          )}

          <Text style={styles.status}>
            Installed: {installed ? 'Yes' : 'No'} | Running: {running ? 'Yes' : 'No'} | State:{' '}
            {playbackStateLabel(playbackState)}
          </Text>
        </View>

        {running && (
          <>
            {/* Video */}
            <View style={styles.section}>
              <Text style={styles.sectionTitle}>Video</Text>
              <View style={styles.videoContainer}>
                <VideoView scaleMode={0} style={styles.video} />
              </View>
            </View>

            {/* Audio Controls */}
            <View style={styles.section}>
              <Text style={styles.sectionTitle}>Audio</Text>
              <View style={styles.controlRow}>
                <TouchableOpacity
                  style={[styles.muteButton, muted && styles.muteButtonActive]}
                  onPress={toggleMute}
                >
                  <Text style={styles.buttonText}>{muted ? 'Unmute' : 'Mute'}</Text>
                </TouchableOpacity>
                <View style={styles.gainContainer}>
                  <Text style={styles.gainLabel}>Gain: {gain.toFixed(1)}</Text>
                  <View style={styles.gainButtons}>
                    <TouchableOpacity
                      style={styles.gainButton}
                      onPress={() => onGainChange(Math.max(0, gain - 0.1))}
                    >
                      <Text style={styles.buttonText}>-</Text>
                    </TouchableOpacity>
                    <TouchableOpacity
                      style={styles.gainButton}
                      onPress={() => onGainChange(Math.min(2, gain + 0.1))}
                    >
                      <Text style={styles.buttonText}>+</Text>
                    </TouchableOpacity>
                  </View>
                </View>
              </View>
            </View>

            {/* Audio Route */}
            <View style={styles.section}>
              <Text style={styles.sectionTitle}>Route: {routeName(currentRoute)}</Text>
              <View style={styles.routeRow}>
                {(availableDevices.length > 0
                  ? [...new Set(availableDevices.map((d) => d.route))]
                  : [AudioRoute.Speaker]
                ).map((route) => (
                  <TouchableOpacity
                    key={route}
                    style={[styles.routeButton, currentRoute === route && styles.routeButtonActive]}
                    onPress={() => setRoute(route)}
                  >
                    <Text style={styles.routeText}>{routeName(route)}</Text>
                  </TouchableOpacity>
                ))}
              </View>
            </View>

            {/* Pause/Resume */}
            <View style={styles.section}>
              <Text style={styles.sectionTitle}>Playback Control</Text>
              <View style={styles.buttonRow}>
                <TouchableOpacity
                  style={[styles.button, styles.flexButton, paused && styles.muteButtonActive]}
                  onPress={paused ? doResume : doPause}
                >
                  <Text style={styles.buttonText}>{paused ? 'Resume' : 'Pause'}</Text>
                </TouchableOpacity>
              </View>

              {/* Playback Rate */}
              <Text style={styles.subLabel}>Rate: {playbackRate.toFixed(1)}x</Text>
              <View style={styles.buttonRow}>
                {[0.5, 1.0, 1.5, 2.0].map((rate) => (
                  <TouchableOpacity
                    key={rate}
                    style={[styles.rateButton, playbackRate === rate && styles.routeButtonActive]}
                    onPress={() => changePlaybackRate(rate)}
                  >
                    <Text style={styles.routeText}>{rate}x</Text>
                  </TouchableOpacity>
                ))}
              </View>
            </View>

            {/* DVR / Seek */}
            <View style={styles.section}>
              <Text style={styles.sectionTitle}>DVR / Seek</Text>
              <Text style={styles.metricText}>Buffer range: {bufferRange.toFixed(1)}s</Text>
              <View style={styles.buttonRow}>
                <TouchableOpacity style={[styles.button, styles.flexButton]} onPress={doRewind}>
                  <Text style={styles.buttonText}>Rewind 10s</Text>
                </TouchableOpacity>
                <TouchableOpacity style={[styles.button, styles.flexButton]} onPress={doGoLive}>
                  <Text style={styles.buttonText}>Go Live</Text>
                </TouchableOpacity>
              </View>
            </View>

            {/* Clip Capture */}
            <View style={styles.section}>
              <Text style={styles.sectionTitle}>Clip Capture</Text>
              <TouchableOpacity
                style={[styles.button, clipCapturing && styles.disabledButton]}
                onPress={doCapture}
                disabled={clipCapturing}
              >
                <Text style={styles.buttonText}>
                  {clipCapturing ? 'Capturing...' : 'Capture 10s'}
                </Text>
              </TouchableOpacity>
              {clipPath && (
                <Text style={styles.clipPath} numberOfLines={3}>
                  {clipPath}
                </Text>
              )}
            </View>

            {/* Catchup Policy */}
            <View style={styles.section}>
              <Text style={styles.sectionTitle}>
                Catchup Policy: {catchupPolicyLabel(catchupPolicy)}
              </Text>
              <View style={styles.routeRow}>
                {[
                  CatchupPolicy.PlayThrough,
                  CatchupPolicy.Accelerate,
                  CatchupPolicy.DropToLive
                ].map((policy) => (
                  <TouchableOpacity
                    key={policy}
                    style={[
                      styles.routeButton,
                      catchupPolicy === policy && styles.routeButtonActive
                    ]}
                    onPress={() => changeCatchupPolicy(policy)}
                  >
                    <Text style={styles.routeText}>{catchupPolicyLabel(policy)}</Text>
                  </TouchableOpacity>
                ))}
              </View>
            </View>

            {/* Track Info */}
            {trackInfo && (
              <View style={styles.section}>
                <Text style={styles.sectionTitle}>Track Info</Text>
                <Text style={styles.metricText}>Audio: {trackInfo.audioCodecId}</Text>
                <Text style={styles.metricText}>
                  Video: {trackInfo.videoCodecId} {trackInfo.videoWidth}x{trackInfo.videoHeight}
                </Text>
              </View>
            )}

            {/* Audio Levels */}
            {metrics && (
              <View style={styles.section}>
                <Text style={styles.sectionTitle}>Audio Levels</Text>
                <Text style={styles.meterText}>
                  Peak: {metrics.levels.peakDbfs.toFixed(1)} dBFS{' '}
                  {levelMeter(metrics.levels.peakDbfs)}
                </Text>
                <Text style={styles.meterText}>
                  RMS: {metrics.levels.rmsDbfs.toFixed(1)} dBFS {levelMeter(metrics.levels.rmsDbfs)}
                </Text>
                <Text style={styles.metricText}>Clips: {metrics.levels.clipCount}</Text>
              </View>
            )}

            {/* Stall Recovery */}
            {metrics && (
              <View style={styles.section}>
                <Text style={styles.sectionTitle}>Stall Recovery</Text>
                <View style={styles.metricsGrid}>
                  <MetricItem label='State' value={metrics.stall.state} />
                  <MetricItem label='Stalls' value={metrics.stall.stallCount} />
                  <MetricItem label='Recoveries' value={metrics.stall.recoveryCount} />
                  <MetricItem label='KF Requests' value={metrics.stall.keyFrameRequests} />
                  <MetricItem label='Total' value={`${metrics.stall.totalStallMs}ms`} />
                  <MetricItem label='Last' value={`${metrics.stall.lastStallMs}ms`} />
                  <MetricItem label='Longest' value={`${metrics.stall.longestStallMs}ms`} />
                </View>
              </View>
            )}

            {/* Metrics — Quality */}
            {metrics && (
              <View style={styles.section}>
                <Text style={styles.sectionTitle}>Quality Metrics</Text>
                <View style={styles.metricsGrid}>
                  <MetricItem label='Underruns' value={metrics.quality.underruns} />
                  <MetricItem label='Drops (A)' value={metrics.quality.framesDropped} />
                  <MetricItem label='Decode Err' value={metrics.quality.decodeErrors} />
                  <MetricItem label='Dec Resets' value={metrics.quality.decoderResets} />
                  <MetricItem label='PTS Disc' value={metrics.quality.ptsDiscontinuities} />
                  <MetricItem label='Gaps >50ms' value={metrics.quality.gapsOver50ms} />
                  <MetricItem label='Gaps >100ms' value={metrics.quality.gapsOver100ms} />
                  <MetricItem label='Gaps >500ms' value={metrics.quality.gapsOver500ms} />
                  <MetricItem label='Max Gap' value={`${metrics.quality.maxInterFrameGapMs}ms`} />
                  <MetricItem label='Received' value={metrics.quality.framesReceived} />
                  <MetricItem label='Drained' value={metrics.quality.framesDrained} />
                </View>
              </View>
            )}

            {/* Metrics — Video */}
            {metrics && (
              <View style={styles.section}>
                <Text style={styles.sectionTitle}>Video Metrics</Text>
                <View style={styles.metricsGrid}>
                  <MetricItem label='FPS' value={metrics.video.currentFps.toFixed(1)} />
                  <MetricItem
                    label='A/V Sync'
                    value={`${(metrics.video.avSyncOffsetUs / 1000).toFixed(1)}ms`}
                  />
                  <MetricItem label='Decoded' value={metrics.video.framesDecoded} />
                  <MetricItem label='Drops (V)' value={metrics.video.framesDropped} />
                  <MetricItem label='Late' value={metrics.video.lateFrames} />
                  <MetricItem label='Skipped' value={metrics.video.skippedFrames} />
                  <MetricItem label='Queue' value={metrics.video.queueDepth} />
                  <MetricItem
                    label='V Jitter'
                    value={`${(metrics.video.jitterUs / 1000).toFixed(1)}ms`}
                  />
                  <MetricItem label='V Drift' value={`${metrics.video.driftPpm.toFixed(0)}ppm`} />
                  <MetricItem label='V Dec Err' value={metrics.video.decodeErrors} />
                  <MetricItem label='V Dec Rst' value={metrics.video.decoderResets} />
                  <MetricItem label='Surf Lost' value={metrics.video.surfaceLostCount} />
                  <MetricItem label='Needs KF' value={metrics.video.needsKeyFrame ? 'Yes' : 'No'} />
                  <MetricItem
                    label='Size'
                    value={`${metrics.video.width}x${metrics.video.height}`}
                  />
                </View>
              </View>
            )}

            {/* Metrics — Jitter & Drift */}
            {metrics && (
              <View style={styles.section}>
                <Text style={styles.sectionTitle}>Jitter / Drift</Text>
                <View style={styles.metricsGrid}>
                  <MetricItem
                    label='Jitter'
                    value={`${(metrics.jitter.jitterUs / 1000).toFixed(1)}ms`}
                  />
                  <MetricItem
                    label='Buf Target'
                    value={`${(metrics.jitter.bufferTargetUs / 1000).toFixed(0)}ms`}
                  />
                  <MetricItem label='Drift' value={`${metrics.drift.driftPpm.toFixed(0)}ppm`} />
                  <MetricItem label='Drift Active' value={metrics.drift.active ? 'Yes' : 'No'} />
                  <MetricItem label='Ratio' value={metrics.drift.currentRatio.toFixed(6)} />
                  <MetricItem label='Catchup' value={metrics.drift.catchupRatio.toFixed(6)} />
                </View>
              </View>
            )}

            {/* Metrics — Pipeline & Session */}
            {metrics && (
              <View style={styles.section}>
                <Text style={styles.sectionTitle}>Pipeline / Session</Text>
                <View style={styles.metricsGrid}>
                  <MetricItem label='Stream St' value={metrics.pipeline.audioStreamState} />
                  <MetricItem label='Gain' value={metrics.pipeline.currentGain.toFixed(2)} />
                  <MetricItem
                    label='Buffered'
                    value={`${(metrics.pipeline.bufferedDurationUs / 1000).toFixed(0)}ms`}
                  />
                  <MetricItem
                    label='Decoded'
                    value={`${(metrics.pipeline.decodedDurationUs / 1000).toFixed(0)}ms`}
                  />
                  <MetricItem
                    label='Uptime'
                    value={`${(metrics.session.uptimeMs / 1000).toFixed(0)}s`}
                  />
                  <MetricItem label='Samples' value={metrics.session.samplesOutput} />
                  <MetricItem label='Rate' value={metrics.session.playbackRate.toFixed(2)} />
                </View>
              </View>
            )}

            {/* Metrics — Demux */}
            {metrics && (
              <View style={styles.section}>
                <Text style={styles.sectionTitle}>Demux</Text>
                <View style={styles.metricsGrid}>
                  <MetricItem label='Bytes Fed' value={metrics.demux.totalBytesFed} />
                  <MetricItem label='Feed Calls' value={metrics.demux.feedDataCalls} />
                  <MetricItem label='Audio Pkts' value={metrics.demux.audioPacketsEmitted} />
                  <MetricItem label='Video Pkts' value={metrics.demux.videoPacketsEmitted} />
                  <MetricItem label='Overflow' value={metrics.demux.overflowCount} />
                  <MetricItem label='Parse St' value={metrics.demux.parseState} />
                </View>
              </View>
            )}

            {/* Metrics — Audio Output & Health */}
            {metrics && (
              <View style={styles.section}>
                <Text style={styles.sectionTitle}>Audio Output / Health</Text>
                <View style={styles.metricsGrid}>
                  <MetricItem label='Restarts' value={metrics.audioOutput.restartCount} />
                  <MetricItem label='Last Err' value={metrics.audioOutput.lastError} />
                  <MetricItem label='Sample Rate' value={metrics.audioOutput.actualSampleRate} />
                  <MetricItem
                    label='Latency'
                    value={`${(metrics.audioOutput.latencyUs / 1000).toFixed(1)}ms`}
                  />
                  <MetricItem label='Mode' value={metrics.latency.mode} />
                  <MetricItem
                    label='Lowest Lat'
                    value={metrics.latency.isLowestLatency ? 'Yes' : 'No'}
                  />
                  <MetricItem label='BT Route' value={metrics.bluetooth.route} />
                  <MetricItem label='BT A2DP' value={metrics.bluetooth.isA2dp ? 'Yes' : 'No'} />
                  <MetricItem label='WD Tripped' value={metrics.health.watchdogTripCount} />
                  <MetricItem
                    label='Heartbeat'
                    value={`${metrics.health.timeSinceHeartbeatMs}ms`}
                  />
                  <MetricItem
                    label='Pool Enc'
                    value={metrics.pools.encodedUnderPressure ? 'PRESS' : 'OK'}
                  />
                  <MetricItem
                    label='Pool Dec'
                    value={metrics.pools.decodedUnderPressure ? 'PRESS' : 'OK'}
                  />
                </View>
              </View>
            )}
          </>
        )}

        <View style={styles.section}>
          <Text style={styles.info}>
            This is a receive-only WebM player. Feed muxed WebM data (VP9 + Opus) via
            MediaPipeline.feedData() from your Hypercore or network source. Native demuxes and
            routes audio/video to HW decode pipelines. No mock data is generated — connect a real
            broadcast stream.
          </Text>
        </View>
      </ScrollView>
    </SafeAreaView>
  )
}

function MetricItem({ label, value }: { label: string; value: string | number }) {
  return (
    <View style={styles.metricItem}>
      <Text style={styles.metricLabel}>{label}</Text>
      <Text style={styles.metricValue}>{value}</Text>
    </View>
  )
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: '#1a1a2e'
  },
  scroll: {
    padding: 20
  },
  title: {
    fontSize: 28,
    fontWeight: 'bold',
    color: '#e0e0e0',
    marginBottom: 20,
    textAlign: 'center'
  },
  healthBar: {
    borderRadius: 8,
    padding: 10,
    marginBottom: 16,
    alignItems: 'center'
  },
  healthText: {
    color: '#fff',
    fontSize: 14,
    fontWeight: '600'
  },
  section: {
    backgroundColor: '#16213e',
    borderRadius: 12,
    padding: 16,
    marginBottom: 16
  },
  sectionTitle: {
    fontSize: 16,
    fontWeight: '600',
    color: '#a0c4ff',
    marginBottom: 12
  },
  button: {
    backgroundColor: '#0f3460',
    borderRadius: 8,
    padding: 14,
    alignItems: 'center',
    marginBottom: 8
  },
  flexButton: {
    flex: 1
  },
  stopButton: {
    backgroundColor: '#c0392b'
  },
  buttonRow: {
    flexDirection: 'row',
    gap: 8
  },
  buttonText: {
    color: '#fff',
    fontSize: 16,
    fontWeight: '600'
  },
  status: {
    color: '#888',
    fontSize: 12,
    marginTop: 8,
    textAlign: 'center'
  },
  error: {
    color: '#e74c3c',
    fontSize: 14,
    marginBottom: 12,
    textAlign: 'center'
  },
  videoContainer: {
    aspectRatio: 16 / 9,
    backgroundColor: '#000',
    borderRadius: 8,
    overflow: 'hidden'
  },
  video: {
    flex: 1
  },
  controlRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12
  },
  muteButton: {
    backgroundColor: '#0a1628',
    borderRadius: 8,
    padding: 12,
    alignItems: 'center',
    minWidth: 80
  },
  muteButtonActive: {
    backgroundColor: '#c0392b'
  },
  gainContainer: {
    flex: 1,
    alignItems: 'center'
  },
  gainLabel: {
    color: '#888',
    fontSize: 12,
    marginBottom: 4
  },
  gainButtons: {
    flexDirection: 'row',
    gap: 8
  },
  gainButton: {
    backgroundColor: '#0a1628',
    borderRadius: 8,
    width: 40,
    height: 40,
    alignItems: 'center',
    justifyContent: 'center'
  },
  routeRow: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 8
  },
  routeButton: {
    flex: 1,
    minWidth: 70,
    backgroundColor: '#0a1628',
    borderRadius: 8,
    padding: 12,
    alignItems: 'center'
  },
  routeButtonActive: {
    backgroundColor: '#0f3460',
    borderWidth: 1,
    borderColor: '#a0c4ff'
  },
  routeText: {
    color: '#e0e0e0',
    fontSize: 14
  },
  metricText: {
    color: '#ccc',
    fontSize: 13,
    marginBottom: 4
  },
  metricsGrid: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 8
  },
  metricItem: {
    backgroundColor: '#0a1628',
    borderRadius: 6,
    padding: 8,
    minWidth: 80,
    alignItems: 'center'
  },
  metricLabel: {
    color: '#888',
    fontSize: 10,
    marginBottom: 2
  },
  metricValue: {
    color: '#e0e0e0',
    fontSize: 14,
    fontWeight: '600'
  },
  subLabel: {
    color: '#888',
    fontSize: 12,
    marginTop: 8,
    marginBottom: 4
  },
  rateButton: {
    flex: 1,
    backgroundColor: '#0a1628',
    borderRadius: 8,
    padding: 10,
    alignItems: 'center'
  },
  disabledButton: {
    backgroundColor: '#333',
    opacity: 0.6
  },
  clipPath: {
    color: '#2ecc71',
    fontSize: 11,
    marginTop: 8,
    fontFamily: 'monospace'
  },
  meterText: {
    color: '#e0e0e0',
    fontSize: 12,
    fontFamily: 'monospace',
    marginBottom: 4
  },
  info: {
    color: '#666',
    fontSize: 13,
    lineHeight: 20,
    textAlign: 'center'
  }
})

AppRegistry.registerComponent('WebmPlayerExample', () => App)

export default App
