# Contributing

Contributions are welcome — large or small. Please read the [code of conduct](./CODE_OF_CONDUCT.md) before interacting.

## Setup

This is a Yarn-workspaces monorepo:

- Library — repo root
- Example app — `example/`

Check [`.nvmrc`](./.nvmrc) for the Node version, then install:

```sh
yarn
```

> Yarn workspaces only — `npm` will not work without manual migration.

The example app is wired to the local library, so library JS changes are picked up without rebuilds; native changes require a rebuild of the example.

Open native sources in:

- **Xcode:** `example/ios/WebmPlayerExample.xcworkspace` → `Pods > Development Pods > webm-player`
- **Android Studio:** `example/android` → `webm-player` under Android

## Running the example

```sh
yarn example start              # Metro
yarn example ios                # iOS device/sim
yarn example android            # Android device/emu
```

Confirm the New Architecture is active — Metro logs should show:

```
Running "WebmPlayerExample" with {"fabric":true,"initialProps":{"concurrentRoot":true},"rootTag":1}
```

## Checks

```sh
yarn typecheck
yarn lint
yarn lint:size                  # file-size budget enforcement
yarn format                     # fix formatting
```

### File-size budgets

`yarn lint:size` enforces a **600-line hard ceiling** and **400-line warning** per source file (vendored and generated code excluded). If your change pushes a file past 400 lines, the PR description must explain why no extraction was possible.

The principle: line budgets are a forcing function. Fixes accumulating in one file are a refactor signal, not a comfort zone. Bug fixes don't grow files past budget — extract first (in its own PR), fix in the new file.

A single class should have a single responsibility. If a class has more than one `public:` block, that class is doing more than one thing — split it. Headers stay in `.h`; non-template implementations belong in `.cpp` or `.inl`.

### Adding a new JSI method

The native API is wired through a single dispatch table on both platforms. To add a method:

1. Declare in `src/MediaPipeline.ts` (TS-side type).
2. Add the binding in the appropriate `cpp/common/bindings/*.inl` — current files: `LifecycleBindings`, `AudioControlBindings`, `AudioRoutingBindings`, `CallbackBindings`, `ClipBindings`, `QueryBindings`, `TranscriptBindings`.
3. Native handler lives in `cpp/common/PipelineOrchestrator.h` or the relevant subsystem.
4. iOS + Android share the binding — no platform-specific JSI plumbing needed unless the call must touch a platform API.
5. If the method changes `HealthEvent.metrics`, update `cpp/pipeline/MetricsCollector.h` and the README metrics table.

## Scripts

| Script                                 | Purpose                |
| -------------------------------------- | ---------------------- |
| `yarn`                                 | Install deps           |
| `yarn prepare`                         | Build TS → `lib/`      |
| `yarn build`                           | Clean + rebuild        |
| `yarn typecheck`                       | Type check             |
| `yarn lint`                            | Lint (`lunte`)         |
| `yarn format`                          | Prettier               |
| `yarn test`                            | Format check + lint    |
| `yarn clean`                           | Remove build artifacts |
| `yarn example start \| ios \| android` | Run example app        |

## Native C++ development

Shared C++ in `cpp/` is where most pipeline logic lives. Run locally without building the full app:

```sh
brew install opus speexdsp      # Prerequisites

yarn test:asan                  # AddressSanitizer + LeakSanitizer
yarn test:tsan                  # ThreadSanitizer
yarn test:ubsan                 # UndefinedBehaviorSanitizer
```

Or build individual binaries:

```sh
cd tests/sanitizer
cmake -B build -DSANITIZER=address -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/test_lsan
./build/test_ubsan
./build/test_route_handler
./build/test_e2e_demux
./build/test_temporal_stress
./build/test_health_watchdog
```

See [`tests/sanitizer/README.md`](tests/sanitizer/README.md) for coverage and how to add new tests.

## Pull requests

> First time? See [How to Contribute to an Open Source Project on GitHub](https://app.egghead.io/playlists/how-to-contribute-to-an-open-source-project-on-github).

- Small, focused PRs — one change each.
- Linters and tests pass.
- Docs reviewed.
- Follow the PR template.
- For API or implementation changes, open an issue to discuss first.
