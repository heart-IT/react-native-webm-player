import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
  SafeAreaView,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from 'react-native';
import {
  createWebmPlayer,
  WebmPlaybackState,
  WebmPlayerView,
  type WebmPlayer,
} from '@heartit/webm-player';
import { DEV_KEY } from './src/devKey';

const STATE_NAMES: Record<number, string> = {
  [WebmPlaybackState.Idle]: 'Idle',
  [WebmPlaybackState.Buffering]: 'Buffering',
  [WebmPlaybackState.Playing]: 'Playing',
  [WebmPlaybackState.Failed]: 'Failed',
};

const METRICS_INTERVAL_MS = 500;
// Every tenth metrics sample also goes to the console, for unattended runs.
const METRICS_LOG_EVERY = 10;
const LOG_LINES = 12;

export default function App(): React.JSX.Element {
  const playerRef = useRef<WebmPlayer | null>(null);
  const [player, setPlayer] = useState<WebmPlayer | null>(null);
  const [key, setKey] = useState(DEV_KEY);
  const [log, setLog] = useState<string[]>([]);
  const [stats, setStats] = useState('not started');

  const say = useCallback((line: string) => {
    // Also to the console, so a device run can be read from logcat / Metro.
    console.log(line);
    setLog(prev =>
      [`${new Date().toISOString().slice(14, 23)}  ${line}`, ...prev].slice(
        0,
        LOG_LINES,
      ),
    );
  }, []);

  useEffect(() => {
    try {
      const p = createWebmPlayer();
      playerRef.current = p;
      setPlayer(p);
      say('player created');
      p.setHealthCallback(e => say(`health: ${e.status} — ${e.detail}`));
    } catch (e) {
      say(`createWebmPlayer failed: ${String(e)}`);
    }
    return () => {
      playerRef.current?.stop();
    };
  }, [say]);

  // Poll metrics so a device run shows whether bytes are actually moving.
  useEffect(() => {
    let samples = 0;
    const id = setInterval(() => {
      const p = playerRef.current;
      if (!p) return;
      try {
        const m = p.getMetrics();
        const line =
          `${STATE_NAMES[p.playbackState] ?? '?'} · received ${m.bytesReceived}B · ` +
          `audio ${m.audioPacketsDecoded} · video ${m.videoPacketsDecoded} ` +
          `(drop ${m.videoFramesDropped}) · ${m.videoWidth}x${m.videoHeight} · ` +
          `t=${m.currentTimeSeconds.toFixed(2)}s`;
        setStats(line);
        if (samples++ % METRICS_LOG_EVERY === 0)
          console.log(`metrics: ${line}`);
      } catch (e) {
        setStats(`getMetrics failed: ${String(e)}`);
      }
    }, METRICS_INTERVAL_MS);
    return () => clearInterval(id);
  }, []);

  const play = useCallback(
    (k: string) => {
      const p = playerRef.current;
      if (!p || !k) return;
      p.play(k);
      say(`play(${k.slice(0, 8)}…)`);
    },
    [say],
  );

  const stop = useCallback(() => {
    playerRef.current?.stop();
    say('stop()');
  }, [say]);

  // Unattended runs: a dev key plays as soon as the player exists.
  const autoPlayed = useRef(false);
  useEffect(() => {
    if (!player || !DEV_KEY || autoPlayed.current) return;
    autoPlayed.current = true;
    play(DEV_KEY);
  }, [player, play]);

  return (
    <SafeAreaView style={styles.root}>
      <View style={styles.video}>
        {player ? (
          <WebmPlayerView
            player={player}
            scaleMode="contain"
            style={StyleSheet.absoluteFill}
          />
        ) : (
          <Text style={styles.dim}>no player</Text>
        )}
      </View>

      <Text style={styles.stats}>{stats}</Text>

      <TextInput
        style={styles.input}
        value={key}
        onChangeText={setKey}
        placeholder="broadcast key"
        placeholderTextColor="#666"
        autoCapitalize="none"
        autoCorrect={false}
      />

      <View style={styles.row}>
        <TouchableOpacity
          style={styles.button}
          onPress={() => play(key.trim())}
        >
          <Text style={styles.buttonText}>Play</Text>
        </TouchableOpacity>
        <TouchableOpacity style={styles.button} onPress={stop}>
          <Text style={styles.buttonText}>Stop</Text>
        </TouchableOpacity>
      </View>

      <ScrollView style={styles.log}>
        {log.map((line, i) => (
          <Text key={i} style={styles.logLine}>
            {line}
          </Text>
        ))}
      </ScrollView>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#111' },
  video: {
    height: 240,
    margin: 12,
    backgroundColor: '#000',
    justifyContent: 'center',
    alignItems: 'center',
  },
  dim: { color: '#666' },
  stats: {
    color: '#7fd',
    fontSize: 11,
    paddingHorizontal: 12,
    paddingBottom: 8,
  },
  input: {
    color: '#eee',
    backgroundColor: '#222',
    marginHorizontal: 12,
    marginBottom: 4,
    padding: 10,
    borderRadius: 6,
    fontFamily: 'Courier',
    fontSize: 12,
  },
  row: { flexDirection: 'row', paddingHorizontal: 8 },
  button: {
    flex: 1,
    backgroundColor: '#2a2a2a',
    margin: 4,
    padding: 12,
    borderRadius: 6,
    alignItems: 'center',
  },
  buttonText: { color: '#eee', fontWeight: '600' },
  log: { flex: 1, margin: 12 },
  logLine: { color: '#9a9', fontSize: 11, fontFamily: 'Courier' },
});
