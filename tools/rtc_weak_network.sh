#!/usr/bin/env bash

set -euo pipefail

readonly TC_ROOT_HANDLE="1919:"
readonly TC_NETEM_HANDLE="191a:"
readonly TC_IFB_HANDLE="1920:"
readonly IPTABLES_OUT_CHAIN="RTCWEAK_OUT"
readonly IPTABLES_IN_CHAIN="RTCWEAK_IN"

STATE_DIR="${RTC_WEAK_NETWORK_STATE_DIR:-/run/rtc-weak-network}"
COMMAND=""
INTERFACE=""
TARGET=""
REMOTE_PORTS=""
BACKEND="auto"
PROFILE_NAME="custom"
UPLOAD_RATE=""
DOWNLOAD_RATE=""
RTT_MS=0
JITTER_MS=0
LOSS_PERCENT=0
CORRELATION_PERCENT=0
QUEUE_LIMIT=1000
DURATION_SEC=120
STEP_DURATION_SEC=30
DURATION_SPECIFIED=0
STEP_DURATION_SPECIFIED=0
TOKEN=""
DRY_RUN=0
EGRESS_ONLY=0
IFB_DEVICE=""

CLEANUP_ON_ERROR=0
CLEANUP_BACKEND=""
CLEANUP_INTERFACE=""
CLEANUP_IFB=""
CYCLE_CLEANUP=0

usage() {
  cat <<'EOF'
用法：
  sudo tools/rtc_weak_network.sh profile <场景> --target <IPv4> [选项]
  sudo tools/rtc_weak_network.sh apply --target <IPv4> --rate <速率> [选项]
  sudo tools/rtc_weak_network.sh cycle --target <IPv4> [选项]
  sudo tools/rtc_weak_network.sh status [--interface <网卡>]
  sudo tools/rtc_weak_network.sh clear [--interface <网卡>]
  tools/rtc_weak_network.sh diagnose

场景：good、limited、weak、extreme、burst-loss

主要选项：
  --interface, -i <网卡>       默认使用 IPv4 默认路由网卡
  --target <IPv4>              远端 RTC 对端或 TURN 服务器地址
  --ports <p1,p2>              只处理指定远端 UDP 端口，最多 15 个
  --backend <auto|tc|iptables> 默认自动选择
  --rate <速率>                同时设置上下行，如 1200kbit、3mbit
  --upload-rate <速率>         单独设置上行
  --download-rate <速率>       单独设置下行
  --rtt-ms <毫秒>              tc 后端模拟的双向 RTT
  --jitter-ms <毫秒>           tc 后端模拟的双向抖动
  --loss <百分比>              每个方向的随机丢包率
  --correlation <百分比>       tc 后端的丢包相关性
  --duration <秒>              自动恢复时间，0 表示不自动恢复，默认 120
  --step-duration <秒>         cycle 每个场景的持续时间，默认 30
  --egress-only                只限制 Orin 发出的 UDP 流量
  --dry-run                    只打印命令，不修改系统
  --state-dir <目录>           覆盖状态目录，主要用于隔离测试

示例：
  sudo tools/rtc_weak_network.sh profile weak --target 10.15.73.245
  sudo tools/rtc_weak_network.sh cycle --target 10.15.73.245 --step-duration 45
  sudo tools/rtc_weak_network.sh clear --interface eth2
EOF
}

info() {
  printf '[rtc-weak-network] %s\n' "$*"
}

warn() {
  printf '[rtc-weak-network] 警告：%s\n' "$*" >&2
}

die() {
  printf '[rtc-weak-network] 错误：%s\n' "$*" >&2
  exit 1
}

print_command() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
}

run() {
  print_command "$@"
  if ((DRY_RUN)); then
    return 0
  fi
  "$@"
}

run_ignore() {
  print_command "$@"
  if ((DRY_RUN)); then
    return 0
  fi
  "$@" >/dev/null 2>&1 || true
}

detect_default_interface() {
  ip -4 route show default 2>/dev/null |
    awk '/default/ { for (i = 1; i <= NF; ++i) if ($i == "dev") { print $(i + 1); exit } }'
}

validate_interface() {
  [[ "$INTERFACE" =~ ^[A-Za-z0-9_.:-]+$ ]] ||
    die "网卡名称不合法：$INTERFACE"
  ip link show dev "$INTERFACE" >/dev/null 2>&1 ||
    die "找不到网卡：$INTERFACE"
}

validate_ipv4() {
  local value="$1"
  local a b c d extra
  IFS=. read -r a b c d extra <<<"$value"
  [[ -z "${extra:-}" && -n "${a:-}" && -n "${b:-}" &&
     -n "${c:-}" && -n "${d:-}" ]] || return 1
  local octet
  for octet in "$a" "$b" "$c" "$d"; do
    [[ "$octet" =~ ^[0-9]+$ ]] || return 1
    ((10#$octet >= 0 && 10#$octet <= 255)) || return 1
  done
}

validate_nonnegative_integer() {
  local name="$1"
  local value="$2"
  [[ "$value" =~ ^[0-9]+$ ]] || die "$name 必须是非负整数：$value"
}

validate_positive_integer() {
  local name="$1"
  local value="$2"
  [[ "$value" =~ ^[0-9]+$ ]] && ((value > 0)) ||
    die "$name 必须是正整数：$value"
}

validate_percent() {
  local name="$1"
  local value="$2"
  [[ "$value" =~ ^[0-9]+([.][0-9]+)?$ ]] ||
    die "$name 必须是 0 到 100 的数字：$value"
  awk -v value="$value" 'BEGIN { exit !(value >= 0 && value <= 100) }' ||
    die "$name 必须处于 0 到 100：$value"
}

validate_rate() {
  local rate="$1"
  [[ "$rate" =~ ^[1-9][0-9]*(kbit|mbit|gbit)$ ]] ||
    die "速率格式不合法：$rate，示例为 1200kbit 或 3mbit"
}

validate_ports() {
  [[ -z "$REMOTE_PORTS" ]] && return 0
  local port_count=0
  local port
  local old_ifs="$IFS"
  IFS=,
  for port in $REMOTE_PORTS; do
    [[ "$port" =~ ^[0-9]+$ ]] && ((port >= 1 && port <= 65535)) ||
      die "UDP 端口不合法：$port"
    ((port_count += 1))
  done
  IFS="$old_ifs"
  ((port_count <= 15)) || die "--ports 最多支持 15 个端口"
}

kernel_config_value() {
  local option="$1"
  if [[ -r /proc/config.gz ]]; then
    zgrep -E "^${option}=" /proc/config.gz 2>/dev/null |
      head -n 1 | cut -d= -f2
    return 0
  fi
  local config_file="/boot/config-$(uname -r)"
  if [[ -r "$config_file" ]]; then
    grep -E "^${option}=" "$config_file" 2>/dev/null |
      head -n 1 | cut -d= -f2
  fi
  return 0
}

kernel_feature_enabled() {
  local value
  value="$(kernel_config_value "$1")"
  [[ "$value" == "y" || "$value" == "m" ]]
}

tc_backend_available() {
  command -v tc >/dev/null 2>&1 || return 1
  kernel_feature_enabled CONFIG_NET_SCH_PRIO || return 1
  kernel_feature_enabled CONFIG_NET_SCH_NETEM || return 1
  kernel_feature_enabled CONFIG_NET_CLS_FLOWER || return 1
  if ((!EGRESS_ONLY)); then
    kernel_feature_enabled CONFIG_IFB || return 1
    kernel_feature_enabled CONFIG_NET_SCH_INGRESS || return 1
    kernel_feature_enabled CONFIG_NET_ACT_MIRRED || return 1
  fi
}

iptables_backend_available() {
  command -v iptables >/dev/null 2>&1 || return 1
  iptables -m hashlimit -h >/dev/null 2>&1 || return 1
  iptables -m statistic -h >/dev/null 2>&1 || return 1
}

select_backend() {
  case "$BACKEND" in
    auto)
      if tc_backend_available; then
        BACKEND="tc"
      elif iptables_backend_available; then
        BACKEND="iptables"
      else
        die "没有可用的 tc/netem 或 iptables 弱网后端"
      fi
      ;;
    tc)
      tc_backend_available ||
        die "当前内核没有启用 PRIO/NETEM/IFB，不能使用 tc 后端"
      ;;
    iptables)
      iptables_backend_available ||
        die "当前系统缺少 iptables hashlimit/statistic 支持"
      ;;
    *)
      die "未知后端：$BACKEND"
      ;;
  esac
}

require_root() {
  if ((DRY_RUN)); then
    return 0
  fi
  ((EUID == 0)) || die "该操作需要 root 权限，请使用 sudo"
}

apply_profile() {
  PROFILE_NAME="$1"
  case "$PROFILE_NAME" in
    good)
      UPLOAD_RATE="12mbit"
      DOWNLOAD_RATE="12mbit"
      RTT_MS=20
      JITTER_MS=2
      LOSS_PERCENT=0
      CORRELATION_PERCENT=0
      ;;
    limited)
      UPLOAD_RATE="3mbit"
      DOWNLOAD_RATE="3mbit"
      RTT_MS=80
      JITTER_MS=15
      LOSS_PERCENT=1
      CORRELATION_PERCENT=10
      ;;
    weak)
      UPLOAD_RATE="1200kbit"
      DOWNLOAD_RATE="1200kbit"
      RTT_MS=160
      JITTER_MS=40
      LOSS_PERCENT=5
      CORRELATION_PERCENT=25
      ;;
    extreme)
      UPLOAD_RATE="400kbit"
      DOWNLOAD_RATE="400kbit"
      RTT_MS=300
      JITTER_MS=100
      LOSS_PERCENT=12
      CORRELATION_PERCENT=35
      ;;
    burst-loss)
      UPLOAD_RATE="1mbit"
      DOWNLOAD_RATE="1mbit"
      RTT_MS=180
      JITTER_MS=60
      LOSS_PERCENT=5
      CORRELATION_PERCENT=60
      ;;
    *)
      die "未知弱网场景：$PROFILE_NAME"
      ;;
  esac
}

parse_common_options() {
  while (($# > 0)); do
    case "$1" in
      --interface|-i)
        (($# >= 2)) || die "$1 缺少参数"
        INTERFACE="$2"
        shift 2
        ;;
      --target)
        (($# >= 2)) || die "$1 缺少参数"
        TARGET="$2"
        shift 2
        ;;
      --ports)
        (($# >= 2)) || die "$1 缺少参数"
        REMOTE_PORTS="$2"
        shift 2
        ;;
      --backend)
        (($# >= 2)) || die "$1 缺少参数"
        BACKEND="$2"
        shift 2
        ;;
      --rate)
        (($# >= 2)) || die "$1 缺少参数"
        UPLOAD_RATE="$2"
        DOWNLOAD_RATE="$2"
        shift 2
        ;;
      --upload-rate)
        (($# >= 2)) || die "$1 缺少参数"
        UPLOAD_RATE="$2"
        shift 2
        ;;
      --download-rate)
        (($# >= 2)) || die "$1 缺少参数"
        DOWNLOAD_RATE="$2"
        shift 2
        ;;
      --rtt-ms)
        (($# >= 2)) || die "$1 缺少参数"
        RTT_MS="$2"
        shift 2
        ;;
      --jitter-ms)
        (($# >= 2)) || die "$1 缺少参数"
        JITTER_MS="$2"
        shift 2
        ;;
      --loss)
        (($# >= 2)) || die "$1 缺少参数"
        LOSS_PERCENT="$2"
        shift 2
        ;;
      --correlation)
        (($# >= 2)) || die "$1 缺少参数"
        CORRELATION_PERCENT="$2"
        shift 2
        ;;
      --duration)
        (($# >= 2)) || die "$1 缺少参数"
        DURATION_SEC="$2"
        DURATION_SPECIFIED=1
        shift 2
        ;;
      --step-duration)
        (($# >= 2)) || die "$1 缺少参数"
        STEP_DURATION_SEC="$2"
        STEP_DURATION_SPECIFIED=1
        shift 2
        ;;
      --queue-limit)
        (($# >= 2)) || die "$1 缺少参数"
        QUEUE_LIMIT="$2"
        shift 2
        ;;
      --state-dir)
        (($# >= 2)) || die "$1 缺少参数"
        STATE_DIR="$2"
        shift 2
        ;;
      --token)
        (($# >= 2)) || die "$1 缺少参数"
        TOKEN="$2"
        shift 2
        ;;
      --egress-only)
        EGRESS_ONLY=1
        shift
        ;;
      --dry-run)
        DRY_RUN=1
        shift
        ;;
      --help|-h)
        usage
        exit 0
        ;;
      *)
        die "未知参数：$1"
        ;;
    esac
  done
}

validate_timing_options() {
  case "$COMMAND" in
    profile|apply)
      ((STEP_DURATION_SPECIFIED == 0)) ||
        die "--step-duration 仅适用于 cycle；profile/apply 请使用 --duration"
      ;;
    cycle)
      ((DURATION_SPECIFIED == 0)) ||
        die "cycle 不接受 --duration；请使用 --step-duration"
      ;;
  esac
}

prepare_common_values() {
  if [[ -z "$INTERFACE" ]]; then
    INTERFACE="$(detect_default_interface)"
  fi
  [[ -n "$INTERFACE" ]] || die "无法自动检测默认 IPv4 网卡"
  validate_interface
  validate_nonnegative_integer "--duration" "$DURATION_SEC"
  validate_positive_integer "--step-duration" "$STEP_DURATION_SEC"
  validate_positive_integer "--queue-limit" "$QUEUE_LIMIT"
}

validate_apply_values() {
  [[ -n "$TARGET" ]] || die "必须通过 --target 指定远端 RTC IPv4 地址"
  validate_ipv4 "$TARGET" || die "IPv4 地址不合法：$TARGET"
  [[ -n "$UPLOAD_RATE" && -n "$DOWNLOAD_RATE" ]] ||
    die "必须通过 --rate 或上下行速率参数指定带宽"
  validate_rate "$UPLOAD_RATE"
  validate_rate "$DOWNLOAD_RATE"
  validate_nonnegative_integer "--rtt-ms" "$RTT_MS"
  validate_nonnegative_integer "--jitter-ms" "$JITTER_MS"
  validate_percent "--loss" "$LOSS_PERCENT"
  validate_percent "--correlation" "$CORRELATION_PERCENT"
  validate_ports
}

state_file_path() {
  printf '%s/%s.state\n' "$STATE_DIR" "$INTERFACE"
}

read_state_value() {
  local state_file="$1"
  local key="$2"
  awk -F= -v key="$key" '$1 == key { sub(/^[^=]*=/, ""); print; exit }' \
    "$state_file"
}

write_state() {
  local state_file
  state_file="$(state_file_path)"
  if ((DRY_RUN)); then
    info "试运行不会写入状态文件：$state_file"
    return 0
  fi
  install -d -m 0755 "$STATE_DIR"
  umask 077
  {
    printf 'token=%s\n' "$TOKEN"
    printf 'backend=%s\n' "$BACKEND"
    printf 'interface=%s\n' "$INTERFACE"
    printf 'ifb=%s\n' "$IFB_DEVICE"
    printf 'target=%s\n' "$TARGET"
    printf 'ports=%s\n' "$REMOTE_PORTS"
    printf 'profile=%s\n' "$PROFILE_NAME"
    printf 'upload_rate=%s\n' "$UPLOAD_RATE"
    printf 'download_rate=%s\n' "$DOWNLOAD_RATE"
    printf 'rtt_ms=%s\n' "$RTT_MS"
    printf 'jitter_ms=%s\n' "$JITTER_MS"
    printf 'loss_percent=%s\n' "$LOSS_PERCENT"
    printf 'correlation_percent=%s\n' "$CORRELATION_PERCENT"
    printf 'egress_only=%s\n' "$EGRESS_ONLY"
  } >"$state_file"
}

tc_root_is_owned() {
  tc qdisc show dev "$1" 2>/dev/null |
    grep -q "^qdisc prio ${TC_ROOT_HANDLE} root"
}

cleanup_tc_backend() {
  local interface="$1"
  local ifb_device="$2"
  if ((DRY_RUN)) || tc_root_is_owned "$interface"; then
    run_ignore tc qdisc del dev "$interface" root
  fi
  if [[ -n "$ifb_device" ]]; then
    run_ignore tc qdisc del dev "$interface" ingress
    run_ignore tc qdisc del dev "$ifb_device" root
    run_ignore ip link set dev "$ifb_device" down
    run_ignore ip link del dev "$ifb_device" type ifb
  fi
}

iptables_chain_exists() {
  iptables -w 5 -n -L "$1" >/dev/null 2>&1
}

cleanup_iptables_backend() {
  run_ignore iptables -w 5 -D OUTPUT -j "$IPTABLES_OUT_CHAIN"
  run_ignore iptables -w 5 -D INPUT -j "$IPTABLES_IN_CHAIN"
  run_ignore iptables -w 5 -F "$IPTABLES_OUT_CHAIN"
  run_ignore iptables -w 5 -F "$IPTABLES_IN_CHAIN"
  run_ignore iptables -w 5 -X "$IPTABLES_OUT_CHAIN"
  run_ignore iptables -w 5 -X "$IPTABLES_IN_CHAIN"
}

cleanup_backend() {
  local backend="$1"
  local interface="$2"
  local ifb_device="$3"
  case "$backend" in
    tc) cleanup_tc_backend "$interface" "$ifb_device" ;;
    iptables) cleanup_iptables_backend ;;
  esac
}

cleanup_from_state_file() {
  local state_file="$1"
  [[ -f "$state_file" ]] || return 0
  local state_backend state_interface state_ifb
  state_backend="$(read_state_value "$state_file" backend)"
  state_interface="$(read_state_value "$state_file" interface)"
  state_ifb="$(read_state_value "$state_file" ifb)"
  [[ -n "$state_interface" ]] || state_interface="$INTERFACE"
  cleanup_backend "$state_backend" "$state_interface" "$state_ifb"
  if ((!DRY_RUN)); then
    rm -f -- "$state_file"
  fi
}

on_exit() {
  local status=$?
  if ((CLEANUP_ON_ERROR)) && ((status != 0)); then
    CLEANUP_ON_ERROR=0
    set +e
    cleanup_backend "$CLEANUP_BACKEND" "$CLEANUP_INTERFACE" "$CLEANUP_IFB"
  fi
  if ((CYCLE_CLEANUP)) && ((status != 0)); then
    CYCLE_CLEANUP=0
    set +e
    local state_file
    state_file="$(state_file_path)"
    cleanup_from_state_file "$state_file"
  fi
  exit "$status"
}

trap on_exit EXIT

load_tc_modules() {
  if [[ "$(kernel_config_value CONFIG_NET_SCH_PRIO)" == "m" ]]; then
    run modprobe sch_prio
  fi
  if [[ "$(kernel_config_value CONFIG_NET_SCH_NETEM)" == "m" ]]; then
    run modprobe sch_netem
  fi
  if [[ "$(kernel_config_value CONFIG_NET_CLS_FLOWER)" == "m" ]]; then
    run modprobe cls_flower
  fi
  if ((!EGRESS_ONLY)); then
    if [[ "$(kernel_config_value CONFIG_IFB)" == "m" ]]; then
      run modprobe ifb
    fi
    if [[ "$(kernel_config_value CONFIG_NET_SCH_INGRESS)" == "m" ]]; then
      run modprobe sch_ingress
    fi
    if [[ "$(kernel_config_value CONFIG_NET_ACT_MIRRED)" == "m" ]]; then
      run modprobe act_mirred
    fi
  fi
}

ensure_default_tc_root() {
  if ((DRY_RUN)); then
    return 0
  fi
  local root_kind
  root_kind="$(tc qdisc show dev "$INTERFACE" |
    awk '{ for (i = 1; i <= NF; ++i) if ($i == "root") { print $2; exit } }')"
  case "$root_kind" in
    mq|noqueue|pfifo_fast|fq_codel|"") ;;
    *)
      die "网卡已有自定义根 qdisc（$root_kind），拒绝覆盖"
      ;;
  esac
  if tc qdisc show dev "$INTERFACE" | grep -qE '^qdisc (ingress|clsact) '; then
    die "网卡已有 ingress/clsact 规则，拒绝覆盖"
  fi
}

build_tc_netem_command() {
  local device="$1"
  local parent_kind="$2"
  local parent_value="$3"
  local handle="$4"
  local rate="$5"
  local one_way_delay=$(((RTT_MS + 1) / 2))
  local one_way_jitter=$(((JITTER_MS + 1) / 2))
  local -a command=(tc qdisc add dev "$device")
  if [[ "$parent_kind" == "root" ]]; then
    command+=(root)
  else
    command+=(parent "$parent_value")
  fi
  command+=(handle "$handle" netem limit "$QUEUE_LIMIT")
  if ((one_way_delay > 0 || one_way_jitter > 0)); then
    command+=(delay "${one_way_delay}ms")
    if ((one_way_jitter > 0)); then
      command+=("${one_way_jitter}ms" distribution normal)
    fi
  fi
  if awk -v loss="$LOSS_PERCENT" 'BEGIN { exit !(loss > 0) }'; then
    command+=(loss random "${LOSS_PERCENT}%")
    if awk -v correlation="$CORRELATION_PERCENT" \
      'BEGIN { exit !(correlation > 0) }'; then
      command+=("${CORRELATION_PERCENT}%")
    fi
  fi
  command+=(rate "$rate")
  run "${command[@]}"
}

add_tc_filter() {
  local parent="$1"
  local direction="$2"
  local classid="$3"
  local port_key="dst_port"
  local ip_key="dst_ip"
  if [[ "$direction" == "incoming" ]]; then
    port_key="src_port"
    ip_key="src_ip"
  fi

  if [[ -z "$REMOTE_PORTS" ]]; then
    run tc filter add dev "$INTERFACE" parent "$parent" protocol ip prio 10 \
      flower ip_proto udp "$ip_key" "$TARGET" classid "$classid"
    return 0
  fi

  local port
  local old_ifs="$IFS"
  IFS=,
  for port in $REMOTE_PORTS; do
    run tc filter add dev "$INTERFACE" parent "$parent" protocol ip prio 10 \
      flower ip_proto udp "$ip_key" "$TARGET" "$port_key" "$port" \
      classid "$classid"
  done
  IFS="$old_ifs"
}

apply_tc_backend() {
  load_tc_modules
  ensure_default_tc_root
  IFB_DEVICE="ifb_${INTERFACE:0:10}"
  if ((!DRY_RUN)) && ip link show dev "$IFB_DEVICE" >/dev/null 2>&1; then
    die "IFB 设备已存在，拒绝覆盖：$IFB_DEVICE"
  fi

  run tc qdisc replace dev "$INTERFACE" root handle "$TC_ROOT_HANDLE" \
    prio bands 3 priomap 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
  build_tc_netem_command "$INTERFACE" parent "${TC_ROOT_HANDLE}3" \
    "$TC_NETEM_HANDLE" "$UPLOAD_RATE"
  add_tc_filter "$TC_ROOT_HANDLE" outgoing "${TC_ROOT_HANDLE}3"

  if ((EGRESS_ONLY)); then
    IFB_DEVICE=""
    return 0
  fi

  run ip link add "$IFB_DEVICE" type ifb
  run ip link set dev "$IFB_DEVICE" up
  run tc qdisc add dev "$INTERFACE" handle ffff: ingress
  if [[ -z "$REMOTE_PORTS" ]]; then
    run tc filter add dev "$INTERFACE" parent ffff: protocol ip prio 10 \
      flower ip_proto udp src_ip "$TARGET" \
      action mirred egress redirect dev "$IFB_DEVICE"
  else
    local port
    local old_ifs="$IFS"
    IFS=,
    for port in $REMOTE_PORTS; do
      run tc filter add dev "$INTERFACE" parent ffff: protocol ip prio 10 \
        flower ip_proto udp src_ip "$TARGET" src_port "$port" \
        action mirred egress redirect dev "$IFB_DEVICE"
    done
    IFS="$old_ifs"
  fi
  build_tc_netem_command "$IFB_DEVICE" root "" "$TC_IFB_HANDLE" \
    "$DOWNLOAD_RATE"
}

rate_to_hashlimit() {
  local rate="$1"
  local number="${rate%%[a-z]*}"
  local unit="${rate#$number}"
  local kilobits
  case "$unit" in
    kbit) kilobits="$number" ;;
    mbit) kilobits=$((number * 1000)) ;;
    gbit) kilobits=$((number * 1000 * 1000)) ;;
    *) die "不能转换速率：$rate" ;;
  esac
  printf '%skb/s\n' "$(((kilobits + 7) / 8))"
}

rate_to_hashlimit_burst() {
  local rate="$1"
  local hashlimit_rate
  hashlimit_rate="$(rate_to_hashlimit "$rate")"
  local kilobytes="${hashlimit_rate%%kb/s}"
  printf '%s\n' "$((kilobytes * 1024))"
}

loss_probability() {
  awk -v loss="$LOSS_PERCENT" 'BEGIN { printf "%.10f", loss / 100.0 }'
}

append_iptables_direction_rules() {
  local chain="$1"
  local direction="$2"
  local rate="$3"
  local hashlimit_name="$4"
  local -a selector=(-p udp)
  local port_key="--dports"
  local hashlimit_mode="dstip"
  if [[ "$direction" == "outgoing" ]]; then
    selector+=(-o "$INTERFACE" -d "$TARGET")
  else
    selector+=(-i "$INTERFACE" -s "$TARGET")
    port_key="--sports"
    hashlimit_mode="srcip"
  fi
  if [[ -n "$REMOTE_PORTS" ]]; then
    selector+=(-m multiport "$port_key" "$REMOTE_PORTS")
  fi

  run iptables -w 5 -A "$chain" "${selector[@]}" \
    -m hashlimit --hashlimit-above "$(rate_to_hashlimit "$rate")" \
    --hashlimit-mode "$hashlimit_mode" --hashlimit-name "$hashlimit_name" \
    --hashlimit-burst "$(rate_to_hashlimit_burst "$rate")" -j DROP

  if awk -v loss="$LOSS_PERCENT" 'BEGIN { exit !(loss > 0) }'; then
    run iptables -w 5 -A "$chain" "${selector[@]}" \
      -m statistic --mode random --probability "$(loss_probability)" -j DROP
  fi
}

apply_iptables_backend() {
  if ((!DRY_RUN)); then
    iptables_chain_exists "$IPTABLES_OUT_CHAIN" &&
      die "iptables 链已存在，拒绝覆盖：$IPTABLES_OUT_CHAIN"
    iptables_chain_exists "$IPTABLES_IN_CHAIN" &&
      die "iptables 链已存在，拒绝覆盖：$IPTABLES_IN_CHAIN"
  fi

  if [[ "$(kernel_config_value CONFIG_NETFILTER_XT_MATCH_HASHLIMIT)" == "m" ]]; then
    run modprobe xt_hashlimit
  fi
  if [[ "$(kernel_config_value CONFIG_NETFILTER_XT_MATCH_STATISTIC)" == "m" ]]; then
    run modprobe xt_statistic
  fi
  run iptables -w 5 -N "$IPTABLES_OUT_CHAIN"
  append_iptables_direction_rules "$IPTABLES_OUT_CHAIN" outgoing \
    "$UPLOAD_RATE" rtcwout
  run iptables -w 5 -I OUTPUT 1 -j "$IPTABLES_OUT_CHAIN"

  if ((RTT_MS > 0 || JITTER_MS > 0)); then
    warn "iptables 后端不能模拟 RTT/抖动，本次只应用限速 policing 和丢包"
  fi
  if awk -v correlation="$CORRELATION_PERCENT" \
    'BEGIN { exit !(correlation > 0) }'; then
    warn "iptables 后端不支持相关丢包，本次使用独立随机丢包"
  fi

  if ((EGRESS_ONLY)); then
    return 0
  fi
  run iptables -w 5 -N "$IPTABLES_IN_CHAIN"
  append_iptables_direction_rules "$IPTABLES_IN_CHAIN" incoming \
    "$DOWNLOAD_RATE" rtcwin
  run iptables -w 5 -I INPUT 1 -j "$IPTABLES_IN_CHAIN"

}

schedule_rollback() {
  ((DURATION_SEC > 0)) || return 0
  if ((DRY_RUN)); then
    info "将在 ${DURATION_SEC} 秒后按令牌自动恢复：$TOKEN"
    return 0
  fi
  local script_path
  script_path="$(readlink -f "$0")"
  nohup bash -c '
    sleep "$1"
    exec "$2" rollback --interface "$3" --token "$4" --state-dir "$5"
  ' _ "$DURATION_SEC" "$script_path" "$INTERFACE" "$TOKEN" "$STATE_DIR" \
    >/dev/null 2>&1 &
  info "已安排 ${DURATION_SEC} 秒后的自动恢复"
}

apply_current_settings() {
  prepare_common_values
  validate_apply_values
  select_backend
  require_root

  local state_file
  state_file="$(state_file_path)"
  if [[ -f "$state_file" ]]; then
    info "清理网卡上的上一组弱网规则"
    cleanup_from_state_file "$state_file"
  fi

  TOKEN="$(date +%s%N)-$$"
  CLEANUP_ON_ERROR=1
  CLEANUP_BACKEND="$BACKEND"
  CLEANUP_INTERFACE="$INTERFACE"
  CLEANUP_IFB="ifb_${INTERFACE:0:10}"

  case "$BACKEND" in
    tc) apply_tc_backend ;;
    iptables) apply_iptables_backend ;;
  esac

  CLEANUP_IFB="$IFB_DEVICE"
  write_state
  CLEANUP_ON_ERROR=0
  local download_summary="$DOWNLOAD_RATE"
  if ((EGRESS_ONLY)); then
    download_summary="unchanged"
  fi
  info "已应用 profile=$PROFILE_NAME backend=$BACKEND target=$TARGET upload=$UPLOAD_RATE download=$download_summary loss=${LOSS_PERCENT}%"
  schedule_rollback
}

show_status() {
  prepare_common_values
  require_root
  local state_file
  state_file="$(state_file_path)"
  if [[ ! -f "$state_file" ]]; then
    info "网卡 $INTERFACE 没有由本工具管理的弱网规则"
    return 0
  fi
  info "当前状态文件：$state_file"
  sed 's/^/  /' "$state_file"
  local state_backend state_ifb
  state_backend="$(read_state_value "$state_file" backend)"
  state_ifb="$(read_state_value "$state_file" ifb)"
  if [[ "$state_backend" == "tc" ]]; then
    tc -s qdisc show dev "$INTERFACE"
    tc -s filter show dev "$INTERFACE" parent "$TC_ROOT_HANDLE" || true
    tc -s filter show dev "$INTERFACE" parent ffff: || true
    [[ -z "$state_ifb" ]] || tc -s qdisc show dev "$state_ifb"
  else
    iptables -w 5 -n -v -L "$IPTABLES_OUT_CHAIN" --line-numbers || true
    iptables -w 5 -n -v -L "$IPTABLES_IN_CHAIN" --line-numbers || true
  fi
}

clear_current_settings() {
  prepare_common_values
  require_root
  local state_file
  state_file="$(state_file_path)"
  if [[ ! -f "$state_file" ]]; then
    info "网卡 $INTERFACE 没有需要恢复的弱网规则"
    return 0
  fi
  cleanup_from_state_file "$state_file"
  info "已恢复网卡 $INTERFACE 的弱网规则"
}

rollback_if_current() {
  prepare_common_values
  require_root
  [[ -n "$TOKEN" ]] || die "rollback 缺少令牌"
  local state_file current_token
  state_file="$(state_file_path)"
  [[ -f "$state_file" ]] || return 0
  current_token="$(read_state_value "$state_file" token)"
  [[ "$current_token" == "$TOKEN" ]] || return 0
  cleanup_from_state_file "$state_file"
}

diagnose() {
  local option value
  info "内核：$(uname -r)"
  for option in CONFIG_NET_SCH_PRIO CONFIG_NET_SCH_NETEM \
    CONFIG_NET_CLS_FLOWER CONFIG_NET_ACT_MIRRED CONFIG_IFB \
    CONFIG_NET_SCH_INGRESS CONFIG_NETFILTER_XT_MATCH_HASHLIMIT \
    CONFIG_NETFILTER_XT_MATCH_STATISTIC; do
    value="$(kernel_config_value "$option")"
    [[ -n "$value" ]] || value="未启用或未知"
    printf '  %-42s %s\n' "$option" "$value"
  done
  if tc_backend_available; then
    info "可用后端：tc（支持限速、RTT、抖动和相关丢包）"
  elif iptables_backend_available; then
    info "可用后端：iptables（支持限速 policing 和随机丢包）"
    warn "当前内核不能使用 netem，RTT、抖动和相关丢包不会生效"
  else
    die "未找到可用弱网后端"
  fi
}

run_cycle() {
  prepare_common_values
  [[ -n "$TARGET" ]] || die "cycle 必须通过 --target 指定远端地址"
  validate_ipv4 "$TARGET" || die "IPv4 地址不合法：$TARGET"
  validate_ports
  select_backend
  require_root
  CYCLE_CLEANUP=1
  local profile
  local -a profiles=(good limited weak extreme weak limited good)
  for profile in "${profiles[@]}"; do
    apply_profile "$profile"
    DURATION_SEC=$((STEP_DURATION_SEC + 30))
    apply_current_settings
    info "场景 $profile 持续 ${STEP_DURATION_SEC} 秒"
    if ((DRY_RUN)); then
      continue
    fi
    sleep "$STEP_DURATION_SEC"
  done
  DURATION_SEC=0
  clear_current_settings
  CYCLE_CLEANUP=0
}

main() {
  (($# > 0)) || {
    usage
    exit 1
  }
  COMMAND="$1"
  shift

  case "$COMMAND" in
    profile)
      (($# > 0)) || die "profile 缺少场景名称"
      apply_profile "$1"
      shift
      parse_common_options "$@"
      validate_timing_options
      apply_current_settings
      ;;
    apply)
      parse_common_options "$@"
      validate_timing_options
      apply_current_settings
      ;;
    cycle)
      parse_common_options "$@"
      validate_timing_options
      run_cycle
      ;;
    status)
      parse_common_options "$@"
      show_status
      ;;
    clear)
      parse_common_options "$@"
      clear_current_settings
      ;;
    rollback)
      parse_common_options "$@"
      rollback_if_current
      ;;
    diagnose)
      parse_common_options "$@"
      diagnose
      ;;
    --help|-h|help)
      usage
      ;;
    *)
      die "未知命令：$COMMAND"
      ;;
  esac
}

main "$@"
