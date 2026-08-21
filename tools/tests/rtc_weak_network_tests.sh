#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOL="$(cd "$SCRIPT_DIR/.." && pwd)/rtc_weak_network.sh"
TEST_STATE_DIR="$(mktemp -d)"
TEST_OUTPUT="$(mktemp)"

cleanup() {
  local status=$?
  rm -rf -- "$TEST_STATE_DIR"
  rm -f -- "$TEST_OUTPUT"
  exit "$status"
}
trap cleanup EXIT

bash -n "$TOOL"

"$TOOL" diagnose >"$TEST_OUTPUT" 2>&1
grep -q "可用后端：iptables" "$TEST_OUTPUT"

"$TOOL" profile weak --interface lo --target 192.0.2.10 \
  --backend iptables --duration 30 --state-dir "$TEST_STATE_DIR" \
  --dry-run >"$TEST_OUTPUT" 2>&1
grep -q -- "--hashlimit-above 150kb/s" "$TEST_OUTPUT"
grep -q -- "--probability 0.0500000000" "$TEST_OUTPUT"
grep -q "不能模拟 RTT/抖动" "$TEST_OUTPUT"

if "$TOOL" profile weak --interface lo --target 192.0.2.10 \
  --backend iptables --step-duration 1 --dry-run >"$TEST_OUTPUT" 2>&1; then
  echo "profile 静默接受了仅适用于 cycle 的 --step-duration" >&2
  exit 1
fi
grep -q -- "--step-duration 仅适用于 cycle" "$TEST_OUTPUT"

if "$TOOL" cycle --interface lo --target 192.0.2.10 \
  --backend iptables --duration 1 --dry-run >"$TEST_OUTPUT" 2>&1; then
  echo "cycle 静默接受了不会生效的 --duration" >&2
  exit 1
fi
grep -q "cycle 不接受 --duration" "$TEST_OUTPUT"

"$TOOL" profile weak --interface lo --target 192.0.2.10 \
  --backend iptables --egress-only --duration 30 \
  --state-dir "$TEST_STATE_DIR" --dry-run >"$TEST_OUTPUT" 2>&1
grep -q "不能模拟 RTT/抖动" "$TEST_OUTPUT"
grep -q "download=unchanged" "$TEST_OUTPUT"

if "$TOOL" apply --interface lo --target 999.0.0.1 --rate 1mbit \
  --dry-run >"$TEST_OUTPUT" 2>&1; then
  echo "非法 IPv4 地址没有被拒绝" >&2
  exit 1
fi

# 在独立网络命名空间中实际创建并清理 iptables 规则。
unshare -Urn bash -c '
  set -euo pipefail
  export XTABLES_LOCKFILE="$1/xtables.lock"
  ip link add weak0 type dummy
  ip link set weak0 up
  ip address add 192.0.2.1/24 dev weak0
  "$2" apply --interface weak0 --target 192.0.2.10 --rate 800kbit \
    --loss 3 --backend iptables --duration 0 --state-dir "$1"
  iptables -C OUTPUT -j RTCWEAK_OUT
  iptables -C INPUT -j RTCWEAK_IN
  dd if=/dev/zero bs=1200 count=2000 2>/dev/null |
    nc -u -w 1 192.0.2.10 5000 || true
  dropped="$(iptables -n -v -x -L RTCWEAK_OUT |
    awk '\''$3 == "DROP" { packets += $1 } END { print packets + 0 }'\'')"
  if ((dropped == 0)); then
    echo "限速/丢包规则没有产生丢包计数" >&2
    exit 1
  fi
  "$2" clear --interface weak0 --state-dir "$1"
  if iptables -n -L RTCWEAK_OUT >/dev/null 2>&1; then
    exit 1
  fi

  "$2" apply --interface weak0 --target 192.0.2.10 --rate 1mbit \
    --loss 1 --backend iptables --duration 1 --state-dir "$1"
  sleep 2
  if iptables -n -L RTCWEAK_OUT >/dev/null 2>&1; then
    echo "定时自动恢复没有删除规则" >&2
    exit 1
  fi
' _ "$TEST_STATE_DIR" "$TOOL"

echo "rtc_weak_network_tests: PASS"
