#!/usr/bin/env node
// Enforces file size discipline.
//
//   - 400 lines: warning. Approaching the cap is a refactor signal.
//   - 600 lines: hard ceiling, CI fails.
//
// Vendored deps and nitrogen-generated code are excluded; tests are included.
//
// Run: npm run lint:size

const fs = require('fs')
const path = require('path')

const ROOT = path.resolve(__dirname, '..')

const WARN = 400
const HARD = 600

const SOURCE_DIRS = ['cpp', 'ios', 'android/src', 'src', 'tests']
const EXCLUDE_DIRS = new Set([
  '.cxx',
  'node_modules',
  'lib',
  'third_party',
  'nitrogen',
  '_deps',
  'fixtures',
  '.git',
])

// Any build tree, however suffixed: build/, build-asan/, build-tsan/, ...
// CMake drops a 900-line generated compiler-probe source in each one.
const EXCLUDE_DIR_PREFIXES = ['build']

function isExcludedDir(name) {
  return (
    EXCLUDE_DIRS.has(name) ||
    EXCLUDE_DIR_PREFIXES.some((p) => name.startsWith(p))
  )
}
const SOURCE_EXTS = new Set([
  '.h',
  '.hpp',
  '.cpp',
  '.mm',
  '.m',
  '.kt',
  '.java',
  '.swift',
  '.tsx',
  '.ts',
])

function walk(dir, out = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    if (entry.isDirectory() && isExcludedDir(entry.name)) continue
    const full = path.join(dir, entry.name)
    if (entry.isDirectory()) walk(full, out)
    else if (SOURCE_EXTS.has(path.extname(entry.name))) out.push(full)
  }
  return out
}

function lineCount(file) {
  // Count newlines + 1 for the last line if it doesn't end with \n.
  const buf = fs.readFileSync(file)
  let lines = 0
  for (let i = 0; i < buf.length; i++) if (buf[i] === 0x0a) lines++
  if (buf.length && buf[buf.length - 1] !== 0x0a) lines++
  return lines
}

function collect() {
  const files = []
  for (const dir of SOURCE_DIRS) {
    const full = path.join(ROOT, dir)
    if (fs.existsSync(full)) walk(full, files)
  }
  return files
}

function main() {
  const files = collect()
  const errors = []
  const warnings = []

  for (const abs of files) {
    const rel = path.relative(ROOT, abs)
    const lines = lineCount(abs)
    if (lines > HARD) {
      errors.push(`${rel}: ${lines} lines exceeds hard limit of ${HARD}.`)
    } else if (lines > WARN) {
      warnings.push(`${rel}: ${lines} lines (warn at ${WARN}).`)
    }
  }

  if (warnings.length) {
    console.warn('lint:size warnings:')
    for (const w of warnings) console.warn('  ' + w)
    console.warn('')
  }

  if (errors.length) {
    console.error('lint:size errors:')
    for (const e of errors) console.error('  ' + e)
    console.error(
      `\n${errors.length} error(s). See file size discipline rules.`
    )
    process.exit(1)
  }

  console.log(
    `lint:size ok — ${files.length} files, ${warnings.length} warnings.`
  )
}

main()
