#!/usr/bin/env bash
# Virtual core test helper for TrusterAcuator
# - Single-file tool placed in tools/
# - If TA binary not found, exits with clear compile instructions
# - Interactive: 输入两个 0..100 的百分比 (left right)，或输入 q 退出

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# Candidate binary names to check (workspace-relative and PATH)
CANDIDATES=("$ROOT_DIR/truster_actuator_demo" "$ROOT_DIR/TrusterAcuator" "$ROOT_DIR/truster_actuator" "$ROOT_DIR/truster")
TA_BIN=""
for c in "${CANDIDATES[@]}"; do
  if [ -x "$c" ]; then
    TA_BIN="$c"
    break
  fi
done
if [ -z "$TA_BIN" ]; then
  if command -v truster_actuator_demo >/dev/null 2>&1; then
    TA_BIN="$(command -v truster_actuator_demo)"
  elif command -v TrusterAcuator >/dev/null 2>&1; then
    TA_BIN="$(command -v TrusterAcuator)"
  fi
fi

if [ -z "$TA_BIN" ]; then
  cat <<EOF >&2
ERROR: 没有在仓库或 PATH 中找到 TrusterAcuator 可执行文件。
请在仓库根目录编译它，示例命令（在 /root/usv_better）:

  g++ -std=c++17 -O2 -Wall TrusterAcuator.cc -o truster_actuator_demo

或根据项目的构建系统调整编译命令。编译完成后将可执行文件放在仓库根目录或 PATH 中，脚本即会自动使用它。

此脚本不会修改现有核心代码；如需直接控制 PWM，可使用 tools/thruster_test_runner.sh。
EOF
  exit 2
fi

echo "Using TrusterAcuator binary: $TA_BIN"
echo "交互式模式：输入两列 0..100 的百分比（left right），回车发送；输入 q 退出。"

print_help() {
  cat <<EOF
示例：
  30 40    # 左推进器 30%, 右推进器 40%
  0 0      # 停止
  q        # 退出
EOF
}

print_help

while true; do
  printf "> "
  if ! IFS= read -r line; then
    echo
    break
  fi
  line="$(echo "$line" | tr -d '\r')"
  if [ "$line" = "q" ] || [ "$line" = "Q" ]; then
    echo "退出。"
    break
  fi
  if [ -z "$line" ]; then
    continue
  fi
  # 允许用空格或逗号分隔
  left=$(echo "$line" | awk -F'[ ,]+' '{print $1}')
  right=$(echo "$line" | awk -F'[ ,]+' '{print $2}')
  if [ -z "$right" ]; then
    echo "需要两个值：left right（0..100）。输入 'h' 查看帮助。"
    continue
  fi

  # 验证是数字且在 0..100
  re='^[0-9]+([.][0-9]+)?$'
  if ! [[ $left =~ $re ]] || ! [[ $right =~ $re ]]; then
    echo "输入错误：请使用数字，范围 0..100。"
    continue
  fi
  # 强制为整数（或者保留小数）
  # bounds
  left_val=$(awk "BEGIN{printf \"%.0f\", ($left<0?0:($left>100?100:$left))}")
  right_val=$(awk "BEGIN{printf \"%.0f\", ($right<0?0:($right>100?100:$right))}")

  echo "发送到 TrusterAcuator: left=$left_val% right=$right_val%"

  # 调用 TA，可根据 TA 的实际参数接口修改
  set +e
  "$TA_BIN" "$left_val" "$right_val"
  rc=$?
  set -e
  if [ $rc -ne 0 ]; then
    echo "TrusterAcuator 返回非零状态: $rc" >&2
  fi
done

echo "虚拟核心脚本结束。"
#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TA_BIN=""
VERBOSE=0

print_usage() {
  cat <<'EOF'
Virtual core smoke test for TrusterAcuator

Usage:
  bash tools/virtual_core_test.sh [--ta /path/to/truster_actuator_demo] [--verbose]

Keys:
  0  neutral: 0 0
  1  low:     25 25
  2  medium:  50 50
  3  high:    75 75
  4  max:     100 100
  l  left-only 100 0
  r  right-only 0 100
  n  next preset in cycle
  p  previous preset in cycle
  h  help
  q  quit
EOF
}

die_missing_ta() {
  echo "ERR: TrusterAcuator binary not found." >&2
  echo "Compile it first with:" >&2
  echo "  cd \"$ROOT_DIR\" && g++ -std=c++17 -Wall -Wextra -pedantic TrusterAcuator.cc -o truster_actuator_demo" >&2
  echo "Then rerun:" >&2
  echo "  bash tools/virtual_core_test.sh --ta ./truster_actuator_demo" >&2
  exit 2
}

resolve_ta() {
  if [[ -n "$TA_BIN" ]]; then
    if [[ -x "$TA_BIN" ]]; then
      printf '%s\n' "$TA_BIN"
      return 0
    fi
    return 1
  fi

  local candidates=(
    "$ROOT_DIR/truster_actuator_demo"
    "$ROOT_DIR/TrusterAcuator"
    "$ROOT_DIR/tools/truster_actuator_demo"
  )
  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

drain_target_output() {
  local line
  local last_line=""
  while IFS= read -r -t 0.05 line <&"${TA_PROC[0]}"; do
    last_line="$line"
    if [[ $VERBOSE -eq 1 ]]; then
      echo "[TA] $line"
    fi
  done

  if [[ -n "$last_line" ]]; then
    if [[ "$last_line" == *"status=0"* ]]; then
      echo "[VC] output_status=OK"
    elif [[ "$last_line" == *"status=1"* ]]; then
      echo "[VC] output_status=ERR"
    fi
  fi
}

send_command() {
  local cmd="$1"
  echo "[VC] send: $cmd"
  printf '%s\n' "$cmd" >&"${TA_PROC[1]}"
  sleep 0.05
  drain_target_output
}

show_help_once() {
  if [[ $VERBOSE -eq 1 ]]; then
    print_usage
  else
    echo "[VC] keys: 0/1/2/3/4/l/r/n/p/h/q"
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ta)
      shift
      if [[ $# -eq 0 ]]; then
        echo "ERR: --ta requires a path" >&2
        exit 2
      fi
      TA_BIN="$1"
      ;;
    --verbose)
      VERBOSE=1
      ;;
    -h|--help)
      print_usage
      exit 0
      ;;
    *)
      echo "ERR: unknown argument: $1" >&2
      exit 2
      ;;
  esac
  shift
 done

if ! TA_PATH="$(resolve_ta)"; then
  die_missing_ta
fi

if [[ $VERBOSE -eq 1 ]]; then
  echo "[VC] using TA: $TA_PATH"
fi

coproc TA_PROC { "$TA_PATH"; }
trap 'kill "$TA_PROC_PID" >/dev/null 2>&1 || true' EXIT

# Drain any startup banner from TA.
sleep 0.05
if [[ $VERBOSE -eq 1 ]]; then
  drain_target_output
fi

presets=(
  "0 0"
  "25 25"
  "50 50"
  "75 75"
  "100 100"
)
index=0
show_help_once

while true; do
  printf 'virtual-core> '
  if ! IFS= read -rsn1 key </dev/tty; then
    echo
    break
  fi
  echo

  case "$key" in
    0)
      send_command "0 0"
      ;;
    1)
      send_command "25 25"
      ;;
    2)
      send_command "50 50"
      ;;
    3)
      send_command "75 75"
      ;;
    4)
      send_command "100 100"
      ;;
    l|L)
      send_command "100 0"
      ;;
    r|R)
      send_command "0 100"
      ;;
    n|N)
      index=$(( (index + 1) % ${#presets[@]} ))
      send_command "${presets[$index]}"
      ;;
    p|P)
      index=$(( (index - 1 + ${#presets[@]}) % ${#presets[@]} ))
      send_command "${presets[$index]}"
      ;;
    h|H)
      show_help_once
      ;;
    q|Q)
      printf '%s\n' "q" >&"${TA_PROC[1]}" || true
      break
      ;;
    *)
      echo "[VC] ignored key: $key"
      ;;
  esac

done

wait "$TA_PROC_PID" >/dev/null 2>&1 || true
