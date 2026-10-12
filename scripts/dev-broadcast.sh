#!/bin/sh
# Broadcasts the 60 s VP9+Opus fixture from this machine and prints its key.
#
# `vcr record` imports stdin as-is and `--throttle` paces it to real time, so
# the broadcast stays VP9 (vcr's own capture path would default to AV1). Join
# it from the example app with the printed key. Needs a pipe on stdin: vcr
# treats a regular file on stdin as "listen on TCP".
set -eu
cd "$(dirname "$0")/.."
# THROTTLE="" broadcasts the whole clip as fast as the swarm takes it, which
# is how a viewer's backpressure chain gets tested.
THROTTLE=${THROTTLE---throttle}
# FIXTURE=<path> broadcasts another VP9+Opus WebM, e.g. a longer one for soaks.
FIXTURE=${FIXTURE:-tests/fixtures/long.webm}
cat "$FIXTURE" |
  exec node node_modules/bare/bin/bare node_modules/@holepunchto/vcr-core/bin.js record $THROTTLE --no-stats "$@"
