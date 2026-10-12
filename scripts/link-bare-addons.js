#!/usr/bin/env node
// Links the worklet's native addons (bare-ffmpeg, udx-native, ...) into
// react-native-bare-kit's addon directories, which its pod and gradle module
// already package into the app. Run by this package's postinstall (so a
// reinstalled node_modules gets them back) and by the gradle preBuild. The app
// needs no bare-link step of its own; an app that also runs bare-link for a
// worklet of its own writes the same files to the same place.
const path = require('path')
const link = require('bare-link')

const platform = process.argv[2]
const out = {
  ios: path.join('ios', 'addons'),
  android: path.join('android', 'src', 'main', 'addons'),
}[platform]

if (!out) {
  console.error('usage: link-bare-addons.js <ios|android>')
  process.exit(2)
}

// Apple frameworks are assembled with Xcode tooling; a Linux install (CI, an
// Android-only machine) has nothing to put them in anyway.
if (platform === 'ios' && process.platform !== 'darwin') {
  console.log('[WebmPlayer] skipping iOS addon link: not macOS')
  process.exit(0)
}

const root = path.join(__dirname, '..')
const bareKit = path.dirname(
  require.resolve('react-native-bare-kit/package.json', { paths: [root] })
)

async function main() {
  const entry = path.join(root, 'worklet', 'feeder.js')
  const dest = path.join(bareKit, out)
  // bare-link yields every intermediate it writes; only the final artifacts
  // land in `dest`.
  const artifacts = new Set()
  for await (const linked of link(entry, { preset: platform, out: dest })) {
    const file = String(linked)
    if (file.startsWith(dest))
      artifacts.add(path.relative(dest, file).split(path.sep)[0])
  }
  console.log(
    `[WebmPlayer] linked ${artifacts.size} worklet addons into ${dest}`
  )
}

main().catch((err) => {
  console.error(err)
  process.exit(1)
})
