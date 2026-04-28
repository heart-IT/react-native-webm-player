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
yarn format                     # fix formatting
```

## Scripts

| Script | Purpose |
|--------|---------|
| `yarn` | Install deps |
| `yarn prepare` | Build TS → `lib/` |
| `yarn build` | Clean + rebuild |
| `yarn typecheck` | Type check |
| `yarn lint` | Lint (`lunte`) |
| `yarn format` | Prettier |
| `yarn test` | Format check + lint |
| `yarn clean` | Remove build artifacts |
| `yarn example start \| ios \| android` | Run example app |

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
