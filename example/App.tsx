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
import { LONG_BASE64, LONG_BYTES, LONG_SECONDS } from './src/fixtureLong';

const FEED_INTERVAL_MS = 50;
// Upper bound per feedData call, so catching up after a late tick still arrives
// as several realistic chunks rather than one large write.
const MAX_CHUNK_BYTES = 16 * 1024;

type Clip = {
  name: string;
  base64: string;
  bytes: number;
  seconds: number;
};

// 1s 5.1 Opus — the only clip that exercises the multistream decode path.
const SHORT_CLIP: Clip = {
  name: '1s 5.1',
  base64: FIXTURE_BASE64,
  bytes: FIXTURE_BYTES,
  seconds: 1,
};

// 60s stereo with a burnt-in timecode, for sustained playback and drift.
const LONG_CLIP: Clip = {
  name: '60s',
  base64: LONG_BASE64,
  bytes: LONG_BYTES,
  seconds: LONG_SECONDS,
};

function decodeBase64(b64: string): Uint8Array {
  const binary = global.atob(b64);
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
  const startedAt = useRef<number>(0);
  const clipSeconds = useRef<number>(0);

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
        const elapsed = startedAt.current
          ? (Date.now() - startedAt.current) / 1000
          : 0;
        // Drift only means something while there is still content to play.
        // Media time stops at the end of the clip and wall time does not, so
        // comparing them past that point reports the clip's own length as
        // drift: at wall 100s on a 60s clip it reads -40s, which only says it
        // ended 40s ago. Clamp the reference to the clip duration.
        const wall = clipSeconds.current
          ? Math.min(elapsed, clipSeconds.current)
          : elapsed;
        const drift = elapsed > 0 ? m.currentTimeSeconds - wall : 0;
        const ended = clipSeconds.current > 0 && elapsed > clipSeconds.current;
        setStats(
          `${STATE_NAMES[p.playbackState] ?? '?'} · fed ${m.bytesFedTotal}B · ` +
            `audio ${m.audioPacketsDecoded} (rec ${m.audioFramesRecovered}, ` +
            `under ${m.audioUnderruns}) · video ${m.videoPacketsDecoded} ` +
            `(drop ${m.videoFramesDropped}) · ${m.videoWidth}x${m.videoHeight}\n` +
            `t=${m.currentTimeSeconds.toFixed(2)}s · wall=${wall.toFixed(1)}s · ` +
            `drift=${drift >= 0 ? '+' : ''}${drift.toFixed(2)}s` +
            (ended ? ` · ended (+${(elapsed - wall).toFixed(0)}s ago)` : ''),
        );
      } catch (e) {
        setStats(`getMetrics failed: ${String(e)}`);
      }
    }, 500);
    return () => clearInterval(id);
  }, []);

  const play = useCallback(
    (clip: Clip) => {
      const p = playerRef.current;
      if (!p) return;
      if (feeding.current) clearInterval(feeding.current);

      p.start();
      const bytesPerSecond = clip.bytes / clip.seconds;
      say(
        `start() · ${clip.name} · ${clip.bytes}B at ~${Math.round(bytesPerSecond)}B/s`,
      );

      const bytes = decodeBase64(clip.base64);
      // Deliberately after decoding, so base64 time is not charged to drift.
      // What remains in the gap is real pipeline latency: bytes sit in the ring
      // until the demuxer and decoders catch up.
      startedAt.current = Date.now();
      clipSeconds.current = clip.seconds;
      let offset = 0;
      feeding.current = setInterval(() => {
        // Target derived from elapsed time, not tick count. setInterval fires
        // late under JS load, and a per-tick byte budget never makes that up —
        // the feed then runs slower than real time and the player starves,
        // which reads as playback drift when it is really a starved input.
        const elapsed = (Date.now() - startedAt.current) / 1000;
        const target = Math.min(
          bytes.length,
          Math.floor(bytesPerSecond * elapsed),
        );

        while (offset < target) {
          const end = Math.min(offset + MAX_CHUNK_BYTES, target);
          // slice() gives a fresh buffer, so .buffer is exactly this chunk.
          if (!p.feedData(bytes.slice(offset, end).buffer)) {
            say(`feedData rejected at ${offset} (ring full)`);
            break;
          }
          offset = end;
        }

        if (offset >= bytes.length) {
          clearInterval(feeding.current!);
          feeding.current = null;
          p.setEndOfStream();
          say('fully fed · setEndOfStream()');
        }
      }, FEED_INTERVAL_MS);
    },
    [say],
  );

  // Auto-start once, so an unattended run (simulator smoke test, CI) exercises
  // the whole path without needing a tap. The buttons stay for device testing.
  const autoStarted = useRef(false);
  useEffect(() => {
    if (!player || autoStarted.current) return;
    autoStarted.current = true;
    const id = setTimeout(() => play(LONG_CLIP), 1000);
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
        <TouchableOpacity style={styles.button} onPress={() => play(LONG_CLIP)}>
          <Text style={styles.buttonText}>Play 60s</Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={styles.button}
          onPress={() => play(SHORT_CLIP)}
        >
          <Text style={styles.buttonText}>1s 5.1</Text>
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
