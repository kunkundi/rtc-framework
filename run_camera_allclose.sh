#!/bin/bash
set -u

GRACE_SECONDS="${GRACE_SECONDS:-2}"
TERM_SECONDS="${TERM_SECONDS:-2}"

APP_PATTERNS=(
  '(^|/)(v4l2_camera_test)([[:space:]]|$)'
  '(^|/)(xcCamera)([[:space:]]|$)'
  '(^|/)(gst-launch-1\.0)([[:space:]]|$)'
  '(^|/)(v4l2-ctl)([[:space:]]|$)'
)

TRIGGER_PATTERNS=(
  '/camera_driver/.*/run_fsync_continuously'
  '/camera_driver/.*/run_fsync_trigger'
  '(^|/|[[:space:]])gpioset([[:space:]]|$)'
)

usage() {
  cat <<'EOF'
Usage: run_camera_allclose.sh

Gracefully stops common local camera capture processes, then escalates if they
do not exit in time.

Targets:
  - v4l2_camera_test
  - xcCamera
  - gst-launch-1.0
  - v4l2-ctl
  - run_fsync_continuously
  - run_fsync_trigger
  - gpioset (used by FSYNC loops)

Environment:
  GRACE_SECONDS  Wait time after SIGINT, default 2
  TERM_SECONDS   Wait time after SIGTERM, default 2
EOF
}

collect_pids() {
  local -n patterns_ref=$1
  local pattern
  local -A uniq=()

  for pattern in "${patterns_ref[@]}"; do
    while read -r pid; do
      if [[ -n "${pid}" ]]; then
        uniq["${pid}"]=1
      fi
    done < <(pgrep -f "${pattern}" || true)
  done

  local pid
  for pid in "${!uniq[@]}"; do
    echo "${pid}"
  done | sort -n
}

print_processes() {
  local pid
  for pid in "$@"; do
    ps -p "${pid}" -o pid=,comm=,args= 2>/dev/null || true
  done
}

load_group_pids() {
  local group_name=$1
  local -n out_ref=$2

  out_ref=()
  while read -r pid; do
    if [[ -n "${pid}" ]]; then
      out_ref+=("${pid}")
    fi
  done < <(collect_pids "${group_name}")
}

signal_group() {
  local group_name=$1
  local signal_name=$2
  local label=$3
  local pids=()

  load_group_pids "${group_name}" pids
  if [[ ${#pids[@]} -eq 0 ]]; then
    echo "${label}: no matching processes"
    return 0
  fi

  echo "${label}: sending SIG${signal_name} to:"
  print_processes "${pids[@]}"
  kill -"${signal_name}" "${pids[@]}" 2>/dev/null || true
}

wait_group_gone() {
  local group_name=$1
  local timeout_seconds=$2
  local label=$3
  local pids=()
  local rounds=$((timeout_seconds * 10))
  local i

  for ((i = 0; i < rounds; ++i)); do
    load_group_pids "${group_name}" pids
    if [[ ${#pids[@]} -eq 0 ]]; then
      return 0
    fi
    sleep 0.1
  done

  echo "${label}: still running after ${timeout_seconds}s:"
  print_processes "${pids[@]}"
  return 1
}

stop_group() {
  local group_name=$1
  local label=$2

  signal_group "${group_name}" INT "${label}"
  if wait_group_gone "${group_name}" "${GRACE_SECONDS}" "${label}"; then
    return 0
  fi

  signal_group "${group_name}" TERM "${label}"
  if wait_group_gone "${group_name}" "${TERM_SECONDS}" "${label}"; then
    return 0
  fi

  signal_group "${group_name}" KILL "${label}"
  wait_group_gone "${group_name}" 1 "${label}" || true
}

report_video_holders() {
  local found_any=0
  local holders_any=0
  local dev

  while read -r dev; do
    if [[ -z "${dev}" ]]; then
      continue
    fi
    found_any=1
    if command -v lsof >/dev/null 2>&1; then
      if lsof "${dev}" >/tmp/run_camera_allclose_lsof.$$ 2>/dev/null; then
        holders_any=1
        echo "Open holder remains on ${dev}:"
        cat /tmp/run_camera_allclose_lsof.$$
      fi
      rm -f /tmp/run_camera_allclose_lsof.$$
    elif command -v fuser >/dev/null 2>&1; then
      if fuser -v "${dev}" >/tmp/run_camera_allclose_fuser.$$ 2>/dev/null; then
        holders_any=1
        echo "Open holder remains on ${dev}:"
        cat /tmp/run_camera_allclose_fuser.$$
      fi
      rm -f /tmp/run_camera_allclose_fuser.$$
    fi
  done < <(compgen -G "/dev/video*" || true)

  if [[ ${found_any} -eq 0 ]]; then
    echo "No /dev/video* nodes found."
    return 0
  fi

  if [[ ${holders_any} -eq 0 ]]; then
    echo "No active /dev/video* holders remain."
  fi
}

main() {
  if [[ $# -gt 0 ]]; then
    case "$1" in
      -h|--help)
        usage
        return 0
        ;;
      *)
        echo "Unknown argument: $1" >&2
        usage >&2
        return 1
        ;;
    esac
  fi

  echo "Stopping camera capture applications..."
  stop_group APP_PATTERNS "camera-apps"

  echo "Stopping camera trigger helpers..."
  stop_group TRIGGER_PATTERNS "camera-trigger"

  report_video_holders
}

main "$@"
