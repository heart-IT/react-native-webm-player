/* global Bare, BareKit */
// Worklet entry. Joins the broadcast at Bare.argv[0], remuxes it to WebM and
// streams it to the host over the worklet IPC as length-prefixed frames
// (framed-stream, 4-byte little-endian length). Payload byte 0 is the type:
// 0 = muxed WebM bytes, 1 = a UTF-8 status: `connected`, `lag`, `stalled`,
// `reconfigured`, `ended`, `error <message>`. The host rejoins on `lag`,
// `stalled`, `reconfigured`, `error` and on an IPC that ends without `ended`.
const Corestore = require('corestore')
const Hyperswarm = require('hyperswarm')
const VCRCore = require('@holepunchto/vcr-core')
const FramedStream = require('framed-stream')
const remuxWebm = require('./remux')

const kFrameData = 0
const kFrameStatus = 1
// vcr-core's seek target for "the next fragment the broadcaster appends".
const kLiveEdge = -1
// How far the reader may trail the log before the host is told to rejoin.
// Backpressure from a stalled player shows up here: the reader stops
// advancing while the broadcaster keeps appending. Playback itself runs
// kLiveDelaySec plus the decoders' queues behind, so this stays well above.
const kMaxLagMs = 10000
// A link that dies quietly leaves the reader waiting for a fragment that never
// comes: the log is not seen growing, so there is no lag, no error and no end
// (iPhone 11, 2026-10-12: Wi-Fi off, "stream stalled" and nothing after it).
// Nothing written for this long is reported as `stalled`; the host rejoins.
const kMaxSilenceMs = kMaxLagMs
const kSilenceCheckMs = 1000
// Lag is measured against the log's own append rate over this window;
// fragment durations are not usable for it (a video-only fragment has none).
const kRateWindowMs = 5000
// A join may learn the log's length seconds before it learns the current
// tail; seeking again while this far behind lands on the real live edge
// before any byte is muxed.
const kMaxJoinBehindFragments = 50
// Interval of the `net` status while no peer is connected.
const kNetReportMs = 5000
// How much of the log a viewer keeps locally: vcr-core clears older fragments
// as new ones download, so a long broadcast cannot fill the cache directory.
const kKeepMs = 2 * 60 * 1000

const [key, storageDir] = Bare.argv
const out = new FramedStream(BareKit.IPC)

let swarm = null
let store = null
let core = null
let exiting = false

// A rejection nobody handles aborts the whole host app, not just this worklet.
Bare.on('uncaughtException', fail)
Bare.on('unhandledRejection', fail)

main().then(exit, fail)

async function main() {
  store = new Corestore(storageDir)
  core = new VCRCore(store, key, { keep: kKeepMs })
  await core.ready()

  swarm = new Hyperswarm()
  swarm.on('connection', (socket) => core.replicate(socket))
  const connected = new Promise((resolve) =>
    swarm.once('connection', (socket) => resolve(socket))
  )
  swarm.join(core.discoveryKey, { server: true, client: true })
  status('joining')
  // While no peer is connected, say what the swarm sees: the host logs it, so
  // a device run shows whether the DHT came up, peers were found, and how
  // many connection attempts each has had.
  const netReport = setInterval(
    () => status(`net ${netSummary()}`),
    kNetReportMs
  )
  let socket
  try {
    socket = await connected
  } finally {
    clearInterval(netReport)
  }
  // The remote address says which path the swarm chose: a LAN address is the
  // direct link, anything else went through NAT or a relay.
  const raw = socket.rawStream
  status(
    `connected ${raw ? `${raw.remoteHost}:${raw.remotePort}` : 'via unknown path'}`
  )

  // Every broadcaster session opens with a fragment that re-declares the
  // streams, and vcr-core reports it through onchange. One seen while
  // seeking is the session being joined; one seen afterwards is a new
  // session, whose codec parameters may differ from the muxer's, so this
  // session ends and the host rejoins with a fresh header.
  let joined = false
  let reconfigured = false
  const input = core.reader({
    onchange: () => {
      if (joined) reconfigured = true
    },
  })
  // The live edge is only known once the peer has sent the log's length.
  // vcr-core's own wait for it is five seconds, which a slow join exceeds,
  // and it then seeks to fragment 1: the start of the broadcast. A peer with
  // an empty log answers without a length; then its first append brings one.
  while (core.length === 0) {
    if (!(await core.core.update({ wait: true }))) {
      await new Promise((resolve) => core.core.once('append', resolve))
    }
  }
  // A log whose last session has ended, or has not grown for minutes, is a
  // finished broadcast: say so rather than wait at an edge nothing reaches.
  const latest = (await core.info()).at(-1)
  if (latest && !latest.isLive) {
    status('ended')
    return
  }
  do {
    if (!(await input.seek(kLiveEdge)))
      throw new Error('seek to live edge failed')
  } while (fragmentsBehind(input) > kMaxJoinBehindFragments)
  joined = true

  lastDataAt = Date.now()
  const silenceWatch = setInterval(() => {
    const silentMs = Date.now() - lastDataAt
    if (silentMs < kMaxSilenceMs) return
    clearInterval(silenceWatch)
    status(
      `stalled no data for ${silentMs} ms at ${input.offset} of ${core.length}`
    )
  }, kSilenceCheckMs)
  let result
  try {
    result = await remuxWebm(
      input,
      write,
      () => reconfigured || lagMs(input) > kMaxLagMs
    )
  } finally {
    clearInterval(silenceWatch)
  }
  if (result === 'ended') status('ended')
  else if (reconfigured) status('reconfigured')
  else status(`lag ${lagDetail(input)}`)
}

function netSummary() {
  const dht = swarm.dht
  const peers = [...swarm.peers.values()]
    .map(
      (p) =>
        `${p.attempts}${p.tried ? 't' : ''}${p.proven ? 'p' : ''}${p.banned ? 'b' : ''}`
    )
    .join(',')
  return (
    `dht ${dht.bootstrapped ? 'up' : 'down'} ${dht.host}:${dht.port}` +
    `${dht.firewalled ? ' firewalled' : ''} peers=${swarm.peers.size}` +
    `${peers ? ` [${peers}]` : ''} connecting=${swarm.connecting} connections=${swarm.connections.size}`
  )
}

function fragmentsBehind(input) {
  return core.length - 1 - input.offset
}

// Samples of the log length over the rate window, newest last.
const lengthSamples = []

function lagMs(input) {
  const now = Date.now()
  lengthSamples.push({ at: now, length: core.length })
  while (lengthSamples.length > 1 && now - lengthSamples[0].at > kRateWindowMs)
    lengthSamples.shift()
  const oldest = lengthSamples[0]
  const appended = core.length - oldest.length
  // A log that is not growing cannot be fallen behind: the reader is waiting
  // for the broadcaster, not the other way round.
  if (appended <= 0) return 0
  return (fragmentsBehind(input) * (now - oldest.at)) / appended
}

// What the watchdog saw, for the host's health detail: the one trace of the
// worklet's view that survives into a device log.
function lagDetail(input) {
  return `${Math.round(lagMs(input))} ms: ${fragmentsBehind(input)} fragments behind at ${input.offset} of ${core.length}`
}

let lastDataAt = 0

function write(chunk) {
  lastDataAt = Date.now()
  if (out.write(frame(kFrameData, chunk))) return null
  return new Promise((resolve) => out.once('drain', resolve))
}

function status(text) {
  out.write(frame(kFrameStatus, Buffer.from(text, 'utf8')))
}

function frame(type, body) {
  const buffer = Buffer.allocUnsafe(1 + body.byteLength)
  buffer[0] = type
  buffer.set(body, 1)
  return buffer
}

function fail(err) {
  if (exiting) return
  status('error ' + (err && err.stack ? err.stack : String(err)))
  exit()
}

async function exit() {
  if (exiting) return
  exiting = true
  if (swarm) await swarm.destroy()
  if (core) await core.close()
  if (store) await store.close()
  // Everything queued on the IPC must reach the host before the worklet goes
  // away: the status frame is what tells it whether to rejoin.
  await new Promise((resolve) => out.end(resolve))
  Bare.exit(0)
}
