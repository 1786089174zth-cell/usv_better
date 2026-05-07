#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROCESSOR_PORT="${PROCESSOR_PORT:-19521}"
GATEWAY_HOST="${GATEWAY_HOST:-127.0.0.1}"
PROCESSOR_TIMEOUT_MS="${PROCESSOR_TIMEOUT_MS:-8000}"
MOCK_D435I="${MOCK_D435I:-0}"
CXX="${CXX:-g++}"
CXXFLAGS="${CXXFLAGS:--std=c++17 -O2 -Wall -Wextra -pedantic}"
LDFLAGS_REALSENSE="${LDFLAGS_REALSENSE:--lrealsense2}"

cd "${FW_DIR}"
echo "[1/3] compiling main_processor_demo"
${CXX} ${CXXFLAGS} MainProcessor.cc SlamExecutionLayer.cc ${LDFLAGS_REALSENSE} -o main_processor_demo

echo "[2/3] compiling communication_layer_demo"
${CXX} ${CXXFLAGS} CommunicationLayer.cc -o communication_layer_demo

cleanup() {
  if [[ -n "${PROCESSOR_PID:-}" ]] && kill -0 "${PROCESSOR_PID}" 2>/dev/null; then
    kill "${PROCESSOR_PID}" 2>/dev/null || true
    wait "${PROCESSOR_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

processor_args=("--tcp" "--port" "${PROCESSOR_PORT}")
if [[ "${MOCK_D435I}" == "1" || "${MOCK_D435I}" == "true" || "${MOCK_D435I}" == "on" ]]; then
  processor_args+=("--mock-d435i")
  echo "[INFO] D435i mode: MOCK (set MOCK_D435I=0 to use real camera)"
else
  echo "[INFO] D435i mode: REAL (set MOCK_D435I=1 for lab mock data)"
fi

echo "[3/3] starting processor backend on :${PROCESSOR_PORT}"
./main_processor_demo "${processor_args[@]}" &
PROCESSOR_PID=$!
sleep 1

echo "[READY] starting gateway on client port 19520, forwarding to ${GATEWAY_HOST}:${PROCESSOR_PORT}"
echo "[READY] run Windows client: python usv_tcp_full_flow_client.py --host <device-ip> --interactive --print-full-detail"
./communication_layer_demo \
  --processor-host "${GATEWAY_HOST}" \
  --processor-port "${PROCESSOR_PORT}" \
  --processor-timeout-ms "${PROCESSOR_TIMEOUT_MS}"
