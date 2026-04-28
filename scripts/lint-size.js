#!/usr/bin/env node
// Enforces file size discipline.
//
//   - 400 lines: warning. Approaching the cap is a refactor signal.
//   - 600 lines: hard ceiling. New offenders fail CI.
//   - .lintsize-baseline.json: existing offenders. Each entry pins the file's
//     current size — the file must shrink below 600 to be removed from the
//     baseline, and may NEVER grow past its pinned size while it remains.
//
// Run: yarn lint:size
// Update baseline (after extracting): yarn lint:size --update-baseline

const fs = require('fs')
const path = require('path')

const ROOT = path.resolve(__dirname, '..')
const BASELINE_PATH = path.join(ROOT, '.lintsize-baseline.json')

const WARN = 400
const HARD = 600

const SOURCE_DIRS = ['cpp', 'ios', 'android/src', 'src']
const EXCLUDE_DIRS = new Set([
  'build',
  '.cxx',
  'node_modules',
  'lib',
  'third_party',
  '_deps',
  '.git'
])
const SOURCE_EXTS = new Set(['.h', '.cpp', '.mm', '.m', '.kt', '.java', '.tsx', '.ts'])

function walk(dir, out = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    if (EXCLUDE_DIRS.has(entry.name)) continue
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

function loadBaseline() {
  if (!fs.existsSync(BASELINE_PATH)) return {}
  return JSON.parse(fs.readFileSync(BASELINE_PATH, 'utf8'))
}

function saveBaseline(b) {
  const sorted = Object.fromEntries(Object.entries(b).sort(([a], [c]) => a.localeCompare(c)))
  fs.writeFileSync(BASELINE_PATH, JSON.stringify(sorted, null, 2) + '\n')
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
  const updateBaseline = process.argv.includes('--update-baseline')
  const files = collect()
  const baseline = loadBaseline()

  const errors = []
  const warnings = []
  const stale = [] // baselined files that have shrunk below HARD
  const newBaseline = {}

  for (const abs of files) {
    const rel = path.relative(ROOT, abs)
    const lines = lineCount(abs)

    if (rel in baseline) {
      const pinned = baseline[rel]
      if (lines <= HARD) {
        stale.push(`${rel}: now ${lines} lines (was pinned at ${pinned}); remove from baseline.`)
        if (!updateBaseline) newBaseline[rel] = pinned
      } else if (lines > pinned) {
        errors.push(
          `${rel}: ${lines} lines (baseline ${pinned}). Baselined files must shrink, not grow. Extract before adding.`
        )
        newBaseline[rel] = pinned
      } else {
        if (lines < pinned && !updateBaseline) {
          warnings.push(
            `${rel}: ${lines} lines (baseline ${pinned}, --update-baseline to tighten).`
          )
        }
        newBaseline[rel] = updateBaseline ? lines : pinned
      }
      continue
    }

    if (lines > HARD) {
      if (updateBaseline) {
        newBaseline[rel] = lines
        warnings.push(`${rel}: ${lines} lines added to baseline.`)
      } else {
        errors.push(
          `${rel}: ${lines} lines exceeds hard limit of ${HARD}. New files must be under ${HARD}.`
        )
      }
    } else if (lines > WARN) {
      warnings.push(`${rel}: ${lines} lines (warn at ${WARN}).`)
    }
  }

  if (warnings.length) {
    console.warn('lint:size warnings:')
    for (const w of warnings) console.warn('  ' + w)
    console.warn('')
  }

  if (stale.length) {
    console.warn('lint:size — baseline entries that can be retired:')
    for (const s of stale) console.warn('  ' + s)
    console.warn('  Run: yarn lint:size --update-baseline')
    console.warn('')
  }

  if (errors.length) {
    console.error('lint:size errors:')
    for (const e of errors) console.error('  ' + e)
    console.error(`\n${errors.length} error(s). See file size discipline rules.`)
    process.exit(1)
  }

  if (updateBaseline) saveBaseline(newBaseline)

  console.log(
    `lint:size ok — ${files.length} files, ${warnings.length} warnings, ${Object.keys(newBaseline).length} baselined.`
  )
}

main()
