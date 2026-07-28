#!/usr/bin/env node
// Verifies the published tarball is self-contained.
//
// The failure this exists to catch: a source file that ships via package.json
// `files` #includes a header that does NOT ship. `npm install` then succeeds,
// `pod install` succeeds, and the consumer's build dies at compile time with a
// missing-header error that nothing in CI ever saw.
//
// This is not hypothetical — the predecessor of this package shipped
// ios/JSIInstaller.mm (which includes "playback_v2/MediaPipelineModuleV2.h")
// while `files` omitted ios/playback_v2/**, making the published package
// uncompilable on iOS.
//
// Rule: every quoted #include in a packaged native source must resolve to a
// file that is ALSO packaged. Includes that resolve to nothing in the working
// tree are treated as external (system headers, framework umbrellas, artifacts
// generated at build time) and ignored — this check only fails on files that
// demonstrably exist here but were left out of the tarball.
//
// Run: npm run lint:pack

const { execFileSync } = require('child_process')
const fs = require('fs')
const path = require('path')

const ROOT = path.resolve(__dirname, '..')

const NATIVE_EXTS = new Set(['.h', '.hpp', '.cpp', '.cc', '.mm', '.m', '.inc'])

// Mirrors the header search roots the podspec and CMakeLists expose.
const INCLUDE_ROOTS = [
  '',
  'cpp',
  'cpp/third_party/libwebm',
  'ios',
  'nitrogen/generated/shared/c++',
  'nitrogen/generated/ios',
  'nitrogen/generated/android',
]

function packagedFiles() {
  const out = execFileSync(
    'npm',
    ['pack', '--dry-run', '--json', '--ignore-scripts'],
    { cwd: ROOT, encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 }
  )
  return JSON.parse(out)[0].files.map((f) => f.path)
}

function quotedIncludes(absFile) {
  const src = fs.readFileSync(absFile, 'utf8')
  const found = []
  const re = /^[ \t]*#[ \t]*include[ \t]+"([^"]+)"/gm
  let m
  while ((m = re.exec(src)) !== null) found.push(m[1])
  return found
}

// Resolve an include to a repo-relative path, or null if it isn't in this tree.
function resolveInclude(spec, fromFileRel) {
  const candidates = [
    path.join(path.dirname(fromFileRel), spec),
    ...INCLUDE_ROOTS.map((r) => path.join(r, spec)),
  ]
  for (const rel of candidates) {
    const normalized = path.normalize(rel)
    if (normalized.startsWith('..')) continue
    if (fs.existsSync(path.join(ROOT, normalized))) return normalized
  }
  return null
}

function main() {
  const packaged = packagedFiles()
  const packagedSet = new Set(packaged)
  const missing = []
  let scanned = 0
  let checked = 0

  for (const rel of packaged) {
    if (!NATIVE_EXTS.has(path.extname(rel))) continue
    const abs = path.join(ROOT, rel)
    if (!fs.existsSync(abs)) continue
    scanned++

    for (const spec of quotedIncludes(abs)) {
      const resolved = resolveInclude(spec, rel)
      if (resolved === null) continue // external / build-time generated
      checked++
      if (!packagedSet.has(resolved)) {
        missing.push(
          `${rel} includes "${spec}" -> ${resolved}, which exists locally but is NOT in the tarball.`
        )
      }
    }
  }

  if (missing.length) {
    console.error('lint:pack errors — published package would not compile:')
    for (const e of missing) console.error('  ' + e)
    console.error(
      `\n${missing.length} unshipped header(s). Add the missing path(s) to package.json "files".`
    )
    process.exit(1)
  }

  console.log(
    `lint:pack ok — ${packaged.length} packaged files, ${scanned} native sources scanned, ${checked} local includes all shipped.`
  )
}

main()
