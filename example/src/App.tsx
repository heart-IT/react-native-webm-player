import React, { useState, useCallback } from 'react'
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
  VideoPipeline,
  VideoView,
  AudioRoute,
  installWebmPlayer,
  type AudioRouteChangeEvent
} from '@holepunchto/webm-player'

function App(): React.ReactElement {
  const [installed, setInstalled] = useState(false)
  const [running, setRunning] = useState(false)
  const [participantId, setParticipantId] = useState(-1)
  const [currentRoute, setCurrentRoute] = useState<AudioRoute>(
    AudioRoute.Speaker
  )
  const [error, setError] = useState<string | null>(null)

  const doInstall = useCallback(() => {
    try {
      installWebmPlayer()
      setInstalled(true)
      setError(null)
    } catch (e) {
      setError(String(e))
    }
  }, [])

  const doStart = useCallback(() => {
    try {
      MediaPipeline.warmUp()
      MediaPipeline.start()
      VideoPipeline.start()

      const pid = MediaPipeline.addParticipant()
      setParticipantId(pid)

      MediaPipeline.setAudioRouteCallback((event: AudioRouteChangeEvent) => {
        setCurrentRoute(event.route)
      })

      setRunning(true)
      setError(null)
    } catch (e) {
      setError(String(e))
    }
  }, [])

  const doStop = useCallback(() => {
    try {
      if (participantId >= 0) {
        MediaPipeline.removeParticipant(participantId)
        VideoPipeline.removeParticipant(participantId)
      }
      VideoPipeline.stop()
      MediaPipeline.stop()
      setRunning(false)
      setParticipantId(-1)
    } catch (e) {
      setError(String(e))
    }
  }, [participantId])

  const setRoute = useCallback((route: AudioRoute) => {
    try {
      MediaPipeline.setAudioRoute(route)
    } catch (e) {
      setError(String(e))
    }
  }, [])

  const routeName = (route: AudioRoute): string => {
    switch (route) {
      case AudioRoute.Speaker:
        return 'Speaker'
      case AudioRoute.Earpiece:
        return 'Earpiece'
      case AudioRoute.WiredHeadset:
        return 'Wired'
      case AudioRoute.BluetoothSco:
        return 'BT SCO'
      case AudioRoute.BluetoothA2dp:
        return 'BT A2DP'
      case AudioRoute.UsbDevice:
        return 'USB'
      default:
        return 'Unknown'
    }
  }

  return (
    <SafeAreaView style={styles.container}>
      <StatusBar barStyle="light-content" />
      <ScrollView contentContainerStyle={styles.scroll}>
        <Text style={styles.title}>WebM Player</Text>

        {error && <Text style={styles.error}>{error}</Text>}

        <View style={styles.section}>
          <Text style={styles.sectionTitle}>Lifecycle</Text>

          {!installed && (
            <TouchableOpacity style={styles.button} onPress={doInstall}>
              <Text style={styles.buttonText}>Install JSI</Text>
            </TouchableOpacity>
          )}

          {installed && !running && (
            <TouchableOpacity style={styles.button} onPress={doStart}>
              <Text style={styles.buttonText}>Start Player</Text>
            </TouchableOpacity>
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
            {participantId >= 0 ? ` | PID: ${participantId}` : ''}
          </Text>
        </View>

        {running && participantId >= 0 && (
          <>
            <View style={styles.section}>
              <Text style={styles.sectionTitle}>Video</Text>
              <View style={styles.videoContainer}>
                <VideoView
                  participantId={participantId}
                  scaleMode={0}
                  style={styles.video}
                />
              </View>
            </View>

            <View style={styles.section}>
              <Text style={styles.sectionTitle}>
                Audio Route: {routeName(currentRoute)}
              </Text>
              <View style={styles.routeRow}>
                <TouchableOpacity
                  style={[
                    styles.routeButton,
                    currentRoute === AudioRoute.Speaker &&
                      styles.routeButtonActive
                  ]}
                  onPress={() => setRoute(AudioRoute.Speaker)}
                >
                  <Text style={styles.routeText}>Speaker</Text>
                </TouchableOpacity>
                <TouchableOpacity
                  style={[
                    styles.routeButton,
                    currentRoute === AudioRoute.Earpiece &&
                      styles.routeButtonActive
                  ]}
                  onPress={() => setRoute(AudioRoute.Earpiece)}
                >
                  <Text style={styles.routeText}>Earpiece</Text>
                </TouchableOpacity>
              </View>
            </View>
          </>
        )}

        <View style={styles.section}>
          <Text style={styles.info}>
            Push WebM data via MediaPipeline.pushAudioFrame() and
            VideoPipeline.pushVideoFrame() after demuxing.
          </Text>
        </View>
      </ScrollView>
    </SafeAreaView>
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
  stopButton: {
    backgroundColor: '#c0392b'
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
  routeRow: {
    flexDirection: 'row',
    gap: 8
  },
  routeButton: {
    flex: 1,
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
  info: {
    color: '#666',
    fontSize: 13,
    lineHeight: 20,
    textAlign: 'center'
  }
})

AppRegistry.registerComponent('WebmPlayerExample', () => App)

export default App
