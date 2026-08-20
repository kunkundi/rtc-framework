#!/usr/bin/env bash
set -euo pipefail

readonly PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

export SDL_AUDIO_DEVICE_SAMPLE_FRAMES=960
export PULSE_LATENCY_MSEC=40

cd "$PROJECT_ROOT"
exec xmake r rtc_edge "$@"
