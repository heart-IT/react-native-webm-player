import React, { useState, useCallback, useEffect, useRef } from 'react'
import {
  StyleSheet,
  View,
  Text,
  TouchableOpacity,
  ScrollView,
  SafeAreaView,
  AppRegistry,
  StatusBar
} from 'react-native'
import {
  MediaPipeline,
  VideoView,
  AudioRoute,
  StreamHealth,
  installWebmPlayer,
  type AudioRouteChangeEvent,
  type AudioDeviceInfo,
  type HealthEvent,
  type TrackInfo,
  type PlayerMetrics
} from '@holepunchto/webm-player'

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
  const [currentRoute, setCurrentRoute] = useState<AudioRoute>(
    AudioRoute.Speaker
  )
  const [availableDevices, setAvailableDevices] = useState<AudioDeviceInfo[]>(
    []
  )
  const [healthStatus, setHealthStatus] = useState<StreamHealth>(
    StreamHealth.Healthy
  )
  const [healthDetail, setHealthDetail] = useState('')
  const [trackInfo, setTrackInfo] = useState<TrackInfo | null>(null)
  const [metrics, setMetrics] = useState<PlayerMetrics | null>(null)
  const [error, setError] = useState<string | null>(null)
  const metricsTimer = useRef<ReturnType<typeof setInterval> | null>(null)

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
      <StatusBar barStyle="light-content" />
      <ScrollView contentContainerStyle={styles.scroll}>
        <Text style={styles.title}>WebM Player</Text>

        {error && <Text style={styles.error}>{error}</Text>}

        {/* Health status bar */}
        {running && (
          <View
            style={[
              styles.healthBar,
              { backgroundColor: healthColor(healthStatus) }
            ]}
          >
            <Text style={styles.healthText}>
              {healthLabel(healthStatus)}
              {healthDetail ? ` — ${healthDetail}` : ''}
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
              <TouchableOpacity
                style={[styles.button, styles.flexButton]}
                onPress={doWarmUp}
              >
                <Text style={styles.buttonText}>Warm Up</Text>
              </TouchableOpacity>
              <TouchableOpacity
                style={[styles.button, styles.flexButton]}
                onPress={doStart}
              >
                <Text style={styles.buttonText}>Start</Text>
              </TouchableOpacity>
            </View>
          )}

          {running && (
            <TouchableOpacity
              style={[styles.button, styles.stopButton]}
              onPress={doStop}
            >
              <Text style={styles.buttonText}>Stop</Text>
            </TouchableOpacity>
          )}

          <Text style={styles.status}>
            Installed: {installed ? 'Yes' : 'No'} | Running:{' '}
            {running ? 'Yes' : 'No'}
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
                  style={[
                    styles.muteButton,
                    muted && styles.muteButtonActive
                  ]}
                  onPress={toggleMute}
                >
                  <Text style={styles.buttonText}>
                    {muted ? 'Unmute' : 'Mute'}
                  </Text>
                </TouchableOpacity>
                <View style={styles.gainContainer}>
                  <Text style={styles.gainLabel}>
                    Gain: {gain.toFixed(1)}
                  </Text>
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
              <Text style={styles.sectionTitle}>
                Route: {routeName(currentRoute)}
              </Text>
              <View style={styles.routeRow}>
                {(availableDevices.length > 0
                  ? [...new Set(availableDevices.map((d) => d.route))]
                  : [AudioRoute.Speaker]
                ).map((route) => (
                  <TouchableOpacity
                    key={route}
                    style={[
                      styles.routeButton,
                      currentRoute === route && styles.routeButtonActive
                    ]}
                    onPress={() => setRoute(route)}
                  >
                    <Text style={styles.routeText}>{routeName(route)}</Text>
                  </TouchableOpacity>
                ))}
              </View>
            </View>

            {/* Track Info */}
            {trackInfo && (
              <View style={styles.section}>
                <Text style={styles.sectionTitle}>Track Info</Text>
                <Text style={styles.metricText}>
                  Audio: {trackInfo.audioCodecId}
                </Text>
                <Text style={styles.metricText}>
                  Video: {trackInfo.videoCodecId} {trackInfo.videoWidth}x
                  {trackInfo.videoHeight}
                </Text>
              </View>
            )}

            {/* Metrics */}
            {metrics && (
              <View style={styles.section}>
                <Text style={styles.sectionTitle}>Metrics</Text>
                <View style={styles.metricsGrid}>
                  <MetricItem
                    label="Underruns"
                    value={metrics.quality.underruns}
                  />
                  <MetricItem
                    label="Drops (A)"
                    value={metrics.quality.framesDropped}
                  />
                  <MetricItem
                    label="Drops (V)"
                    value={metrics.video.framesDropped}
                  />
                  <MetricItem
                    label="FPS"
                    value={metrics.video.currentFps.toFixed(1)}
                  />
                  <MetricItem
                    label="A/V Sync"
                    value={`${(metrics.video.avSyncOffsetUs / 1000).toFixed(1)}ms`}
                  />
                  <MetricItem
                    label="Jitter"
                    value={`${(metrics.jitter.jitterUs / 1000).toFixed(1)}ms`}
                  />
                  <MetricItem
                    label="Buffer"
                    value={`${(metrics.jitter.bufferTargetUs / 1000).toFixed(0)}ms`}
                  />
                  <MetricItem
                    label="Drift"
                    value={`${metrics.drift.driftPpm.toFixed(0)}ppm`}
                  />
                  <MetricItem
                    label="Decode Err"
                    value={metrics.quality.decodeErrors}
                  />
                  <MetricItem
                    label="Gaps >100ms"
                    value={metrics.quality.gapsOver100ms}
                  />
                </View>
              </View>
            )}
          </>
        )}

        <View style={styles.section}>
          <Text style={styles.info}>
            This is a receive-only WebM player. Feed muxed WebM data (VP9 +
            Opus) via MediaPipeline.feedData() from your Hypercore or network
            source. Native demuxes and routes audio/video to HW decode
            pipelines. No mock data is generated — connect a real broadcast
            stream.
          </Text>
        </View>
      </ScrollView>
    </SafeAreaView>
  )
}

function MetricItem({
  label,
  value
}: {
  label: string
  value: string | number
}) {
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
  info: {
    color: '#666',
    fontSize: 13,
    lineHeight: 20,
    textAlign: 'center'
  }
})

AppRegistry.registerComponent('WebmPlayerExample', () => App)

export default App
