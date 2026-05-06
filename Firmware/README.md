# USV Edge Demo

## Overview
This workspace contains a lightweight three-layer demo for USV edge validation.
Current focus is the gateway/processor/SLAM-executor contract:
- Communication layer exposes a TCP listener and parses/forwards normalized frames.
- Communication layer forwards supported realtime/config/SLI frames and gateway management commands.
- Main processor owns control, SLAM ingest gating, D435i capture orchestration, fusion semantics, health, and rollback signals.
- SLAM executor bridge uses Intel RealSense D435i capture by default and also includes an explicit mock D435i mode for local smoke testing.

## Files
- `CommunicationLayer.cc`: TCP gateway adapter demo with line-oriented protocol handling and gateway management commands.
- `MainProcessor.cc`: processing layer parser/dispatcher demo, D435i capture orchestration, health output, and `--d435i-selftest` entrypoint.
- `SlamExecutionLayer.h/.cc`: hub-to-SLAM execution bridge, RealSense D435i capture bridge, mock D435i bridge, RGB/depth/gyro row feature enrichment.
- `SelD435iSmokeTest.cc`: 10-second D435i smoke test with `--mock`/`+mock` support.
- `TrusterAcuator.cc`: existing actuator mapping/output demo and dry-run sysfs PWM writer. The file name is historical.
- `tools/thruster_test_runner.sh`: single-file PWM validation entrypoint for dry-run and hardware sysfs writeout.
- `tools/TrusterAcuator_test_runner.sh`: legacy/auxiliary interactive helper that invokes a compiled `TrusterAcuator` binary if present. The file name is historical.

## Dependencies

The SLAM execution path includes `<librealsense2/rs.hpp>` and links against Intel RealSense (`librealsense2`). Install the RealSense headers/library before building targets that include `SlamExecutionLayer.cc` or `SlamExecutionLayer.h`.

Targets that only build `CommunicationLayer.cc` or `TrusterAcuator.cc` do not require RealSense.

## Build

Run these commands from `Firmware/`:

```bash
g++ -std=c++17 -Wall -Wextra -pedantic CommunicationLayer.cc -o communication_layer_demo
g++ -std=c++17 -Wall -Wextra -pedantic MainProcessor.cc SlamExecutionLayer.cc -lrealsense2 -o main_processor_demo
g++ -std=c++17 -Wall -Wextra -pedantic SelD435iSmokeTest.cc SlamExecutionLayer.cc -lrealsense2 -o sel_d435i_smoke
g++ -std=c++17 -Wall -Wextra -pedantic TrusterAcuator.cc -o truster_actuator_demo
```

If your RealSense installation is not in a default compiler search path, add the appropriate `-I`, `-L`, and runtime library path flags for your host.

## Runtime Entry Points

### Communication gateway

Run the communication layer with:

```bash
./communication_layer_demo --processor-host 127.0.0.1 --processor-port 19521
```

It listens on `0.0.0.0`. The default client-facing port is `19520`; if unavailable, it tries fallback ports in this order: `9773`, `11514`, `23758`, `52019`. Gateway traffic is forwarded to the processor TCP backend at `127.0.0.1:19521` by default. Override that backend with `--processor-host`, `--processor-port`, `--processor-timeout-ms`, or the `USV_PROCESSOR_HOST` / `USV_PROCESSOR_PORT` environment variables.

### Main processor

Run the processor in stdin/stdout mode with:

```bash
./main_processor_demo
```

Run the processor as the production TCP backend for the gateway with:

```bash
./main_processor_demo --tcp --port 19521
```

For lab end-to-end tests without a physical D435i, add `--mock-d435i`:

```bash
./main_processor_demo --tcp --port 19521 --mock-d435i
```

The `SLI` processing path attempts D435i capture through the SLAM execution bridge. On a host without a D435i device and RealSense runtime, `SLI` commands can return capture errors unless `--mock-d435i` is enabled.

### End-to-end TCP smoke test

Start the processor first, then the gateway:

```bash
./main_processor_demo --tcp --port 19521 --mock-d435i
./communication_layer_demo --processor-host 127.0.0.1 --processor-port 19521 --processor-timeout-ms 2000
```

Connect clients to the gateway at `127.0.0.1:19520` and send newline-delimited frames. `C START` must succeed before realtime `R` commands are accepted.

For a one-command local build/start workflow, run:

```bash
bash tools/run_tcp_stack.sh
```

For a full client flow (`C START` -> realtime actions -> `C STOP`) that requests 64 interval-picked D435i rows with R/depth pairs by default, run from a client machine:

```bash
python tools/usv_tcp_full_flow_client.py --host <gateway-ip> --interactive --print-full-detail
```

The client exposes C START sampling parameters such as `--max-rows`, `--sample-stride`, and `--channel-mode`; defaults are `--max-rows 64 --sample-stride 64 --channel-mode R`. Realtime ACK details return color and depth as separate lists (`r=...` and `depth=...`) and mark IMU data with the `IMU_gyro=` header. Running the client without `--actions` uses WASD interactive mode on Windows and only exits when `Q` is pressed.

### D435i self-test and smoke test

Processor self-test:

```bash
./main_processor_demo --d435i-selftest
```

Dedicated 10-second smoke test:

```bash
./sel_d435i_smoke
./sel_d435i_smoke --mock
```

`--mock`/`+mock` enables the mock D435i bridge for local validation without a camera.

## Thruster PWM Validation

The current thruster validation flow is consolidated into one script:

```bash
bash tools/thruster_test_runner.sh
sudo bash tools/thruster_test_runner.sh --hw
```

The script writes PWM duty cycles directly to sysfs and no longer compiles or invokes a temporary C++ runner. No log file is generated by default.
It only accepts positive thrust percentages in the range `0..100` for both channels.

### 常见“几步后不响应”故障排查（PWM sysfs）

如果你把 `duty_cycle` 文件句柄长期保持打开（例如在循环外 `std::ofstream duty(...);`，循环内持续 `<<`），在部分内核/驱动上会出现“前几次有效，随后看起来不再响应”的现象。

建议：
- 每次写入都重新打开并写入（脚本 `>` 重定向就是这种方式）。
- 或在每次写前明确 `seekp(0)` 并检查 `good()/fail()`，任何一次失败都立刻告警。
- 在写入路径上保留错误检查，避免静默失败。

此外需要确认：
- `duty_cycle` 不应超过 `period`（有些驱动对等于 `period` 也会拒绝或行为不稳定）。
- `export` 已存在时应容错处理（避免“设备忙”导致初始化分支未完整执行）。
- `enable`、`period`、`duty_cycle` 的写入顺序和返回状态都要检查。

### PWM topology

Orange Pi Zero 3 pin topology used by this workspace:
- Processor PWM sysfs path for channel 1: `/sys/class/pwm/pwmchip0/pwm1`
- Processor PWM sysfs path for channel 2: `/sys/class/pwm/pwmchip0/pwm2`
- Channel 1 duty file: `/sys/class/pwm/pwmchip0/pwm1/duty_cycle`
- Channel 2 duty file: `/sys/class/pwm/pwmchip0/pwm2/duty_cycle`
- Channel 1 hardware pin: `PH3`
- Channel 2 hardware pin: `PH2`
- 40-pin header mapping: `PH3 -> pin8`, `PH2 -> pin10`

Runtime mapping used by the script and current actuator demo:

- Neutral duty: `0 ns`
- Period: `20000000 ns` (`50 Hz`)
- Input format: left/right percentage values in the range `0` to `100`
- Negative values are rejected by the script; there is no reverse thrust path in the validation flow
- `0%` maps to duty `0 ns` (hard stop)
- Any positive value maps to duty `20000000 ns` in the current binary validation flow

`TrusterAcuator.cc` still contains shaping configuration fields such as `duty_span_ns` for future hardware behavior, but current output behavior is binary: non-positive input maps to `0 ns`; positive input maps to the full PWM period.

## Communication Frames

All communication frames are newline-delimited text sent over TCP to `127.0.0.1:<bound_port>` or the host IP that runs the gateway. The default bound port is usually `19520` unless fallback binding was needed.

### 1) Realtime short frame

Gateway input:

```text
R <seq> <F|L|R|S> [client_ts_ms]
RT <seq> <F|L|R|S> [client_ts_ms]
```

Processor-normalized format:

```text
R <seq> <tx_ms> <F|L|R|S>
```

- Used for high-rate action control.
- Requires active session started by `C START`.
- Legacy alias `RT` is accepted by the gateway while `legacy_alias=on` and normalized to `R`.
- Realtime commands keep FLRS semantics: `F` forward, `L` turn-left, `R` turn-right, `S` runtime stop.
- After a realtime action is accepted and sent to the downlink, the processor automatically captures/processes one D435i-backed SLAM sample and appends that result to the same `rt_apply` ACK detail as `action_sent|d435i_ok|SL_...`; fused D435i details expose color/depth as separate `r=` and `depth=` lists and IMU data under `IMU_gyro=`. If capture/processing fails, the action ACK remains `OK` but the detail becomes `action_sent|d435i_err=<reason>`.
- `C STOP` remains a session stop command and is separate from realtime `S`.

### 2) SLAM image ingest frame

```text
SLI <seq> <tx_ms> frame_id=<n> width=<w> height=<h> pixel_fmt=<GRAY8|RGB24|NV12> keyframe=<0|1> quality_hint=<0..100> payload_ref=<id>
SL <seq> <tx_ms> frame_id=<n> width=<w> height=<h> pixel_fmt=<GRAY8|RGB24|NV12> keyframe=<0|1> quality_hint=<0..100> payload_ref=<id>
```

- Sent to the processor for SLAM ingest gating and D435i-backed executor handling.
- `payload_ref` is currently an opaque correlation/reference field in this demo. The present processing path captures from the D435i bridge instead of dereferencing the payload data.
- Legacy alias `SL` is accepted by the gateway while `legacy_alias=on` and normalized to `SLI`.

### 3) Row-feature SLI frame (processor-supported)

The processor also accepts already-enriched row-feature `SLI` frames:

```text
SR <seq> <tx_ms> frame_id=<n> width=<w> height=<h> payload_ref=<id> keyframe=<0|1> quality_hint=<0..100>
```
- Used by gateway Step3 path to fetch RGB frame from adapter, extract one row, and re-upload as `SLI feature=row ...`.
- This path requires an explicit frame source from adapter/integration; there is no automatic fallback to mock input.

### 4) Config long frame

```text
C START seq=<n> ts=<ms> soft_hz=<1..100> max_power=<v> left_gain=<v> right_gain=<v> left_trim=<v> right_trim=<v> slam_max_fps=<1..30> slam_timeout_ms=<1..200> slam_max_groups=<1..64> slam_min_quality=<0..100> slam_drop_policy=<reject|oldest|newest> row_ratio=<0..1> channel_mode=<R|G|B|GRAY> sample_stride=<1..64> max_rows=<1..64> pack_mode=<bin|hex>
C STOP seq=<n> ts=<ms>
```

- Legacy aliases `CS` and `CE` are accepted by the gateway while `legacy_alias=on` and normalized to `C START` and `C STOP`.

### Unsupported / deferred frame

`SR <seq> ...` row extraction requests are **not currently implemented in `CommunicationLayer.cc`**. Send processor-supported row-feature data as `SLI feature=row ...` instead, or use normal `SLI` to trigger the current D435i-backed path.

## ACK Format

Processor ACK format:

```text
ACK <OK|ERR> seq=<n> up_ms=<n> down_ms=<n> tag=<code> detail=<code_or_payload>
```

Gateway ACK format after forwarding or gateway-local management/error handling:

```text
ACK <OK|ERR> seq=<n> up_ms=<n> down_ms=<n> tag=<code> detail=<code_or_payload> gw_trace=<id> gw_route=<result>
```

- Processor `tag` examples: `cfg_start`, `rt_apply`, `sli_ok`, `sli_fail`, `slam_row_ok`, `slam_row_fail`. For `rt_apply`, `detail` includes both the action result and the automatically fused D435i/SLAM result, for example `action_sent|d435i_ok|SL_<seq>_ctrl=..._IMU_gyro=..._r=..._depth=...`.
- Gateway `tag` examples: `gw_bad_msg`, `gw_route_timeout`, `gw_switch`, `gw_rollback`.
- `up_ms` is computed from the command transmit timestamp when possible; otherwise it can be `0`.
- `down_ms` is local processing/downlink time measured by the processor or parsed from processor ACK by the gateway.
- ACK values are single-token fields. The processor and gateway replace whitespace/control characters in `tag`, `detail`, `gw_trace`, and `gw_route` with `_` before emitting ACKs so forwarded ACK parsing remains deterministic.
- `gw_trace` is the gateway-side trace identifier used for cross-layer correlation.
- `gw_route` describes gateway routing outcome such as `forwarded`, `parse_reject`, `route_timeout`, or `mgmt`.

## Runtime Management

Gateway management commands:

```text
GW HEALTH
GW SWITCH legacy_alias=<on|off> sli_enabled=<on|off> route_timeout_ms=<1..5000>
GW ROLLBACK
```

Processor health command:

```text
HEALTH
```

Notes:
- `GW SWITCH` is the compatibility/traffic governance switch entry used by canary migration.
- `GW ROLLBACK` applies conservative defaults immediately: `legacy_alias=on`, `sli_enabled=on`, `route_timeout_ms=80`.
- `GW HEALTH` exposes gateway rollout SLO signals including `ack_p95_ms`, `ack_p99_ms`, timeout/drop counts, and rollback recommendation.
- `HEALTH` exposes processor state, ACK counters/latency percentiles, SLI drops, executor timeouts, and rollback recommendation.

## Release Gates

Block release if any condition is true:
- Protocol consistency checks fail for `C START/C STOP/R/RT/SL/SLI`.
- `rollback_recommended=1` in either gateway or processor health output for a sustained window.
- `ack_p95_ms` exceeds 80 ms in canary gateway traffic or 100 ms in sustained processor traffic.
- Route timeout, parse failure, processor ACK error, or executor timeout rate exceeds the configured threshold.
- D435i smoke test fails in a hardware-required deployment environment.

## Canary and Rollback Flow

1. Stage A (lab): keep `legacy_alias=on`, verify full regression.
2. Stage B (single vessel): disable alias gradually with `GW SWITCH legacy_alias=off`.
3. Stage C (fleet): keep alias off, monitor health, and roll forward only when stable.
4. Rollback trigger: when timeout/error SLO is violated, execute `GW ROLLBACK` and re-enable compatible path.

## Quick Test

Processor stdin/stdout flow. This command uses `SLI`, so it requires a working D435i path; on a non-camera host, expect a capture error for the `SLI` command.

```bash
printf 'C START seq=1 ts=100 soft_hz=20 max_power=60 left_gain=1 right_gain=1 left_trim=0 right_trim=0 slam_max_fps=8 slam_timeout_ms=60 slam_max_groups=4 slam_min_quality=15 slam_drop_policy=newest row_ratio=0.333333 channel_mode=G sample_stride=2 max_rows=1 pack_mode=bin\nR 2 101 F\nSLI 3 102 frame_id=11 width=640 height=480 pixel_fmt=RGB24 keyframe=1 quality_hint=60 payload_ref=buf_11\nC STOP seq=4 ts=120\nq\n' | ./main_processor_demo
```

Mock D435i smoke test for local validation without hardware:

```bash
./sel_d435i_smoke --mock
```

Gateway process:

```bash
./communication_layer_demo
```

In another terminal, send commands to the bound port shown by `communication_layer_demo`:

```bash
printf 'C START seq=1 ts=100 soft_hz=20 max_power=60 left_gain=1 right_gain=1 left_trim=0 right_trim=0 slam_max_fps=8 slam_timeout_ms=60 slam_max_groups=4 slam_min_quality=15 slam_drop_policy=newest row_ratio=0.333333 channel_mode=G sample_stride=2 max_rows=1 pack_mode=bin\nR 2 F 101\nSLI 3 102 frame_id=11 width=640 height=480 pixel_fmt=RGB24 keyframe=1 quality_hint=70 payload_ref=buf_11\nC STOP seq=4 ts=120\nq\n' | nc 127.0.0.1 19520
```

Gateway management:

```bash
printf 'GW HEALTH\nGW SWITCH legacy_alias=off route_timeout_ms=40\nGW HEALTH\nGW ROLLBACK\nGW HEALTH\nq\n' | nc 127.0.0.1 19520
```

Processor health through gateway forwarding:

```bash
printf 'HEALTH\nq\n' | nc 127.0.0.1 19520
```

Direct processor row-feature example:

```bash
printf 'C START seq=1 ts=100 soft_hz=20 max_power=60 left_gain=1 right_gain=1 left_trim=0 right_trim=0 slam_max_fps=8 slam_timeout_ms=60 slam_max_groups=4 slam_min_quality=15 slam_drop_policy=newest row_ratio=0.333333 channel_mode=G sample_stride=2 max_rows=1 pack_mode=bin\nSLI 2 101 frame_id=11 width=64 height=1 pixel_fmt=ROW1 keyframe=1 quality_hint=70 payload_ref=row_11 feature=row row_index=16 channel_mode=G stride=2 sample_count=32 payload_len=32 payload_crc32=12345\nC STOP seq=3 ts=120\nq\n' | ./main_processor_demo
```

## Processor-Executor Reserved Interface

Core session/execution interface:
- `PushConfig(session_id, config_version, slam_cfg)`
- `ProcessFrame(session_id, frame_meta, timeout_ms)`
- `StopSession(session_id)`
- `GetHealth()`

D435i capture interface:
- `InitializeD435i(error)`
- `EnableMockD435i(enabled)`
- `ShutdownD435i()`
- `CaptureD435iFrame(timeout_ms, slam_cfg, out, error)`
- `IsD435iReady()`

## Notes

- Communication layer no longer decides SLAM business semantics.
- Processing layer controls realtime rate limiting, SLAM ingest gating, drop policy, timeout classification, D435i capture orchestration, and fusion output.
- The current executor bridge performs D435i-backed row feature enrichment and simulated SLAM result generation; mock D435i is available only when explicitly enabled.
- Gateway `SR` row extraction is deferred and should not be used as a documented runnable path until implemented.
