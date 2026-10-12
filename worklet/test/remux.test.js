const test = require('brittle')
const fs = require('bare-fs')
const os = require('bare-os')
const path = require('bare-path')
const Corestore = require('corestore')
const VCRCore = require('@holepunchto/vcr-core')
const { importStream } = require('@holepunchto/vcr-core/util')
const remuxWebm = require('../remux')

const kFixture = path.join(
  __dirname,
  '..',
  '..',
  'tests',
  'fixtures',
  'long.webm'
)
const kEbmlMagic = Buffer.from([0x1a, 0x45, 0xdf, 0xa3])

// A core recorded from the fixture, plus a second VCRCore on the same store
// opened by key, which is how a viewer sees it.
async function recordedCore(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'webm-remux-'))
  const store = new Corestore(dir)
  const writer = new VCRCore(store)
  await writer.ready()
  await importStream(writer, fs.createReadStream(kFixture))

  const viewer = new VCRCore(store, writer.key)
  await viewer.ready()

  t.teardown(async () => {
    await viewer.close()
    await writer.close()
    await store.close()
    fs.rmSync(dir, { recursive: true, force: true })
  })
  return viewer
}

test('remuxes a recorded core into WebM, header first, and reports the end', async (t) => {
  const core = await recordedCore(t)
  const input = core.reader()
  t.ok(await input.seek(0), 'seek to the start')

  const chunks = []
  const result = await remuxWebm(
    input,
    (chunk) => {
      chunks.push(chunk)
      return null
    },
    () => false
  )
  t.is(result, 'ended')

  const bytes = Buffer.concat(chunks)
  t.alike(bytes.subarray(0, 4), kEbmlMagic, 'first bytes are the EBML header')
  t.ok(bytes.includes('V_VP9'), 'video track is VP9')
  t.ok(bytes.includes('A_OPUS'), 'audio track is Opus')
  // The fixture is 1,582,400 bytes; a remux carries its payload through.
  t.ok(bytes.byteLength > 1_400_000, `muxed ${bytes.byteLength} bytes`)
})

test('stops between packets when asked and waits for backpressure', async (t) => {
  const core = await recordedCore(t)
  const input = core.reader()
  t.ok(await input.seek(0))

  let packets = 0
  let waits = 0
  const write = () => {
    waits++
    return new Promise((resolve) => setTimeout(resolve, 1))
  }
  const result = await remuxWebm(input, write, () => ++packets > 20)
  t.is(result, 'stopped')
  t.ok(waits > 0, 'every chunk waited for the consumer')
})
