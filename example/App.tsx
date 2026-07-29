import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
  SafeAreaView,
  ScrollView,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
} from 'react-native';
import {
  createWebmPlayer,
  WebmPlaybackState,
  WebmPlayerView,
  type WebmPlayer,
} from '@heartit/webm-player';
import { FIXTURE_BASE64, FIXTURE_BYTES } from './src/fixture';

// Roughly a real chunk from a P2P transport, fed at about real time so the
// buffer sees a live-ish arrival pattern rather than one giant write.
const CHUNK_BYTES = 8 * 1024;
const CHUNK_INTERVAL_MS = 15;

function decodeFixture(): Uint8Array {
  const binary = global.atob(FIXTURE_BASE64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

const STATE_NAMES: Record<number, string> = {
  [WebmPlaybackState.Idle]: 'Idle',
  [WebmPlaybackState.Buffering]: 'Buffering',
  [WebmPlaybackState.Playing]: 'Playing',
  [WebmPlaybackState.Paused]: 'Paused',
  [WebmPlaybackState.Failed]: 'Failed',
};

export default function App(): React.JSX.Element {
  const playerRef = useRef<WebmPlayer | null>(null);
  const [player, setPlayer] = useState<WebmPlayer | null>(null);
  const [log, setLog] = useState<string[]>([]);
  const [stats, setStats] = useState('not started');
  const feeding = useRef<ReturnType<typeof setInterval> | null>(null);

  const say = useCallback((line: string) => {
    setLog(prev =>
      [`${new Date().toISOString().slice(14, 23)}  ${line}`, ...prev].slice(
        0,
        12,
      ),
    );
  }, []);

  useEffect(() => {
    try {
      const p = createWebmPlayer();
      playerRef.current = p;
      setPlayer(p);
      say(`player created · route ${p.currentAudioRoute}`);
      p.setHealthCallback(e => say(`health: ${e.status} — ${e.detail}`));
      p.setRouteChangeCallback(r => say(`route changed: ${r}`));
    } catch (e) {
      say(`createWebmPlayer failed: ${String(e)}`);
    }
    return () => {
      if (feeding.current) clearInterval(feeding.current);
      playerRef.current?.stop();
    };
  }, [say]);

  // Poll metrics so a device run shows whether bytes are actually moving.
  useEffect(() => {
    const id = setInterval(() => {
      const p = playerRef.current;
      if (!p) return;
      try {
        const m = p.getMetrics();
        setStats(
          `${STATE_NAMES[p.playbackState] ?? '?'} · fed ${m.bytesFedTotal}B · ` +
            `audio ${m.audioPacketsDecoded} (recovered ${m.audioFramesRecovered}, ` +
            `underruns ${m.audioUnderruns}) · video ${m.videoPacketsDecoded} ` +
            `(dropped ${m.videoFramesDropped}) · ${m.videoWidth}x${m.videoHeight} · ` +
            `t=${m.currentTimeSeconds.toFixed(2)}s`,
        );
      } catch (e) {
        setStats(`getMetrics failed: ${String(e)}`);
      }
    }, 500);
    return () => clearInterval(id);
  }, []);

  const play = useCallback(() => {
    const p = playerRef.current;
    if (!p) return;
    if (feeding.current) clearInterval(feeding.current);

    p.start();
    say(`start() · feeding ${FIXTURE_BYTES}B in ${CHUNK_BYTES}B chunks`);

    const bytes = decodeFixture();
    let offset = 0;
    feeding.current = setInterval(() => {
      if (offset >= bytes.length) {
        clearInterval(feeding.current!);
        feeding.current = null;
        p.setEndOfStream();
        say('fixture fully fed · setEndOfStream()');
        return;
      }
      const end = Math.min(offset + CHUNK_BYTES, bytes.length);
      // slice() gives a fresh buffer, so .buffer is exactly this chunk.
      const accepted = p.feedData(bytes.slice(offset, end).buffer);
      if (!accepted) say(`feedData rejected at offset ${offset} (ring full)`);
      offset = end;
    }, CHUNK_INTERVAL_MS);
  }, [say]);

  // Auto-start once, so an unattended run (simulator smoke test, CI) exercises
  // the whole path without needing a tap. The buttons stay for device testing.
  const autoStarted = useRef(false);
  useEffect(() => {
    if (!player || autoStarted.current) return;
    autoStarted.current = true;
    const id = setTimeout(() => play(), 1000);
    return () => clearTimeout(id);
  }, [player, play]);

  const stop = useCallback(() => {
    if (feeding.current) {
      clearInterval(feeding.current);
      feeding.current = null;
    }
    playerRef.current?.stop();
    say('stop()');
  }, [say]);

  const reset = useCallback(() => {
    playerRef.current?.resetStream();
    say('resetStream()');
  }, [say]);

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

      <View style={styles.row}>
        <TouchableOpacity style={styles.button} onPress={play}>
          <Text style={styles.buttonText}>Play fixture</Text>
        </TouchableOpacity>
        <TouchableOpacity style={styles.button} onPress={reset}>
          <Text style={styles.buttonText}>Reset</Text>
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
