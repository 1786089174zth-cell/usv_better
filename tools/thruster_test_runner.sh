#!/bin/bash
set -euo pipefail

PWM_CHIP_DIR="/sys/class/pwm/pwmchip0"
PWM1_DIR="$PWM_CHIP_DIR/pwm1"
PWM2_DIR="$PWM_CHIP_DIR/pwm2"
PERIOD_NS=20000000
NEUTRAL_NS=1500000
SPAN_NS=500000

HW_MODE=0
if [[ "${1:-}" == "--hw" ]]; then
  HW_MODE=1
fi

percent_to_duty_ns() {
  local percent="$1"
  awk -v p="$percent" -v n="$NEUTRAL_NS" -v s="$SPAN_NS" 'BEGIN {
    if (p < -100) p = -100;
    if (p > 100) p = 100;
    duty = n + (p / 100.0) * s;
    min = n - s;
    max = n + s;
    if (duty < min) duty = min;
    if (duty > max) duty = max;
    printf "%.0f", duty;
  }'
}

write_duty_pair() {
  local left_percent="$1"
  local right_percent="$2"
  local left_duty right_duty

  left_duty="$(percent_to_duty_ns "$left_percent")"
  right_duty="$(percent_to_duty_ns "$right_percent")"

  if [[ $HW_MODE -eq 1 ]]; then
    printf '%s\n' "$left_duty" > "$PWM1_DIR/duty_cycle"
    printf '%s\n' "$right_duty" > "$PWM2_DIR/duty_cycle"
  fi

  echo "left=${left_percent}% -> duty_ns=${left_duty}, right=${right_percent}% -> duty_ns=${right_duty}"
}

cleanup() {
  if [[ $HW_MODE -eq 1 ]]; then
    printf '0\n' > "$PWM1_DIR/duty_cycle" || true
    printf '0\n' > "$PWM2_DIR/duty_cycle" || true
    printf '0\n' > "$PWM1_DIR/enable" || true
    printf '0\n' > "$PWM2_DIR/enable" || true
    if [[ -w "$PWM_CHIP_DIR/unexport" ]]; then
      printf '1\n' > "$PWM_CHIP_DIR/unexport" || true
      printf '2\n' > "$PWM_CHIP_DIR/unexport" || true
    fi
  fi
}

trap cleanup EXIT

if [[ $HW_MODE -eq 1 ]]; then
  if [[ $EUID -ne 0 ]]; then
    echo "HW mode requires root access to write PWM sysfs." >&2
    exit 2
  fi
  if [[ ! -d "$PWM_CHIP_DIR" ]]; then
    echo "PWM chip directory not found: $PWM_CHIP_DIR" >&2
    exit 3
  fi
  if [[ ! -d "$PWM1_DIR" ]]; then
    printf '1\n' > "$PWM_CHIP_DIR/export"
    sleep 0.1
  fi
  if [[ ! -d "$PWM2_DIR" ]]; then
    printf '2\n' > "$PWM_CHIP_DIR/export"
    sleep 0.1
  fi
  if [[ ! -d "$PWM1_DIR" || ! -d "$PWM2_DIR" ]]; then
    echo "Failed to export pwm channels or they do not appear." >&2
    ls -la "$PWM_CHIP_DIR"
    exit 4
  fi
  printf '%s\n' "$PERIOD_NS" > "$PWM1_DIR/period"
  printf '0\n' > "$PWM1_DIR/duty_cycle"
  printf '1\n' > "$PWM1_DIR/enable"

  printf '%s\n' "$PERIOD_NS" > "$PWM2_DIR/period"
  printf '0\n' > "$PWM2_DIR/duty_cycle"
  printf '1\n' > "$PWM2_DIR/enable"
fi

echo "Running in $([[ $HW_MODE -eq 1 ]] && echo HW || echo dry-run) mode"
echo "Input format: <left_percent> <right_percent>"
echo "Percent range: -100 to 100, mapped around neutral duty ${NEUTRAL_NS}ns with span ${SPAN_NS}ns"
echo "Type q to quit."
echo "Hardware pin mapping: pwm1 -> pin8 (PH3), pwm2 -> pin10 (PH2)"

step=0
while true; do
  printf 'thruster> '
  if ! IFS= read -r line; then
    break
  fi
  if [[ -z "$line" ]]; then
    continue
  fi
  if [[ "$line" == "q" || "$line" == "Q" ]]; then
    break
  fi

  read -r left_percent right_percent <<< "$line" || true
  if [[ -z "${left_percent:-}" || -z "${right_percent:-}" ]]; then
    echo "WARN: expected two numbers, for example: 30 -25"
    continue
  fi

  step=$((step + 1))
  echo "step=${step}"
  write_duty_pair "$left_percent" "$right_percent"
done

echo "done"
