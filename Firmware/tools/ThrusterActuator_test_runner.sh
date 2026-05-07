#!/usr/bin/env bash
# Virtual core test helper for ThrusterActuator
# - Single-file tool placed in tools/
# - If TA binary not found, exits with clear compile instructions
# - Interactive: 输入两个 0..100 的百分比 (left right)，或输入 q 退出

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# Candidate binary names to check (workspace-relative and PATH)
CANDIDATES=("$ROOT_DIR/thruster_actuator_demo" "$ROOT_DIR/ThrusterActuator" "$ROOT_DIR/thruster_actuator" "$ROOT_DIR/thruster")
TA_BIN=""
for c in "${CANDIDATES[@]}"; do
  if [ -x "$c" ]; then
    TA_BIN="$c"
    break
  fi
done
if [ -z "$TA_BIN" ]; then
  if command -v thruster_actuator_demo >/dev/null 2>&1; then
    TA_BIN="$(command -v thruster_actuator_demo)"
  elif command -v ThrusterActuator >/dev/null 2>&1; then
    TA_BIN="$(command -v ThrusterActuator)"
  fi
fi

if [ -z "$TA_BIN" ]; then
  cat <<EOF >&2
ERROR: 没有在仓库或 PATH 中找到 ThrusterActuator 可执行文件。
请在 Firmware 目录编译它，示例命令（在 /root/usv_better/Firmware）:

  g++ -std=c++17 -O2 -Wall ThrusterActuator.cc -o thruster_actuator_demo

或根据项目的构建系统调整编译命令。编译完成后将可执行文件放在仓库根目录或 PATH 中，脚本即会自动使用它。

此脚本不会修改现有核心代码；如需直接控制 PWM，可使用 tools/thruster_test_runner.sh。
EOF
  exit 2
fi

echo "Using ThrusterActuator binary: $TA_BIN"
echo "交互式模式：输入单字母 ACK：F(前进) L(左) R(右) S(停止)，回车发送；输入 q 退出。"
echo "Binary 输出规则：S -> 0ns，F/L/R -> ON (period-1) 。"
echo "Safety: S = hard stop (duty 0)."

print_help() {
  cat <<EOF
示例：
  30 40    # 左右都将映射为 ON (20000000ns)
  0 40     # 左 OFF, 右 ON
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
  cmd=$(echo "$line" | awk '{print toupper($1)}')
  case "$cmd" in
    F)
      echo "发送 F -> both ON"
      set +e; "$TA_BIN" F; rc=$?; set -e
      ;;
    L)
      echo "发送 L -> left ON"
      set +e; "$TA_BIN" L; rc=$?; set -e
      ;;
    R)
      echo "发送 R -> right ON"
      set +e; "$TA_BIN" R; rc=$?; set -e
      ;;
    S)
      echo "发送 S -> stop"
      set +e; "$TA_BIN" S; rc=$?; set -e
      ;;
    *)
      echo "未知命令：使用 F/L/R/S 或 q 退出"
      continue
      ;;
  esac
  if [ $rc -ne 0 ]; then
    echo "ThrusterActuator 返回非零状态: $rc" >&2
  fi
done

echo "虚拟核心脚本结束。"
