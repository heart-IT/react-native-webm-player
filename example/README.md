# Example app

Joins a live broadcast by key and plays it. Two terminals: one broadcasts from
this machine, the other runs the app.

```bash
# Terminal 1, from the repo root: broadcast the 60 s VP9+Opus fixture in a loop
# of one (it ends after 60 s, which also exercises the Ended path).
npm install
npm run bundle:worklet
sh scripts/dev-broadcast.sh
# ... prints "Broadcast key:" followed by the key

# Terminal 2
cd example
npm run ios        # or: npm run android
```

Paste the key into the field and tap Play. The line under the video shows the
player state, bytes received from the worklet, decoded packet counts, frame
size and media time; the log shows health transitions.

The example declares `NSLocalNetworkUsageDescription`, which iOS needs before
an app may reach a broadcaster on the same network (accept the prompt on first
launch), and `UIBackgroundModes: audio` so a backgrounded broadcast keeps
playing on iOS. On Android, playback keeps running as long as the process
does; keeping it alive past the cached-process window is the host app's job
(a foreground service or picture-in-picture), not this library's.

Both platforms need `react-native-bare-kit` as a **direct** dependency of the
app, which this example has. The library's `postinstall` and its gradle
`preBuild` link the worklet's native addons into it; with npm scripts disabled,
run `node scripts/link-bare-addons.js ios` (and `android`) from the package.
