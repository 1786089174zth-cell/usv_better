# USV Edge Demo
## Overview
This workspace contains a lightweight three-layer demo for USV edge validation.
Current focus is Step3 gateway extension on top of Step2:
- Communication layer parses and forwards normalized frames.
- Communication layer can ingest `SR` row requests and generate row-feature `SLI` payload metadata.
- Main processor owns control + SLAM ingest + fusion semantics.
- SLAM executor is currently a mock client with stable interface.

## Files
- `CommunicationLayer.cc`: short-frame communication parser/ack demo
- `MainProcessor.cc`: processing layer parser/dispatcher demo
- `SlamExecutionLayer.h/.cc`: dedicated hub-to-SLAM execution bridge + RGB row preprocessing
- `TrusterAcuator.cc`: actuator mapping/output demo

## Build
```bash
g++ -std=c++17 -Wall -Wextra -pedantic CommunicationLayer.cc -o communication_layer_demo
g++ -std=c++17 -Wall -Wextra -pedantic MainProcessor.cc SlamExecutionLayer.cc -o main_processor_demo
g++ -std=c++17 -Wall -Wextra -pedantic TrusterAcuator.cc -o truster_actuator_demo
```

## Communication Frames

### 1) Realtime short frame
```text
R <seq> <F|B|L|R> [client_ts_ms]
```
- Used for high-rate action control.
- Requires active session started by `C START`.
- Gateway normalizes this to processor format: `R <seq> <tx_ms> <action>`.
- Legacy alias `RT` is accepted by the gateway and normalized to `R`.

### 2) SLAM image ingest frame
```text
SLI <seq> <tx_ms> frame_id=<n> width=<w> height=<h> pixel_fmt=<GRAY8|RGB24|NV12> keyframe=<0|1> quality_hint=<0..100> payload_ref=<id>
```
- Sent to processing hub for executor call and fusion output.
- `payload_ref` is an opaque frame reference (buffer ID / shared memory key / URI).
- Legacy alias `SL` is accepted by the gateway and normalized to `SLI`.

### 3) SLAM row extraction request (gateway local)
```text
SR <seq> <tx_ms> frame_id=<n> width=<w> height=<h> payload_ref=<id> keyframe=<0|1> quality_hint=<0..100>
```
- Used by gateway Step3 path to fetch RGB frame from adapter, extract one row, and re-upload as `SLI feature=row ...`.
- If local socket adapter is unavailable, demo falls back to deterministic mock frame source.

### 4) Config long frame
```text
C START seq=<n> ts=<ms> soft_hz=<1..100> max_power=<v> left_gain=<v> right_gain=<v> left_trim=<v> right_trim=<v> slam_max_fps=<1..30> slam_timeout_ms=<1..200> slam_max_groups=<1..64> slam_min_quality=<0..100> slam_drop_policy=<reject|oldest|newest> row_ratio=<0..1> channel_mode=<R|G|B|GRAY> sample_stride=<1..64> max_rows=<1..8> pack_mode=<bin|hex>
C STOP seq=<n> ts=<ms>
```
- Legacy aliases `CS` and `CE` are accepted by the gateway and normalized to `C START` and `C STOP`.

## ACK Format
```text
ACK <OK|ERR> seq=<n> detail=<code> rx_ms=<n> ul_ms=<n> dl_ms=<n> trace=<id> route=<result>
```
- `tag` examples: `cfg_start`, `rt_apply`, `sli_fusion_ok`, `sli_exec_timeout`.
- Step3 row tags: `slam_row_ok`, `slam_row_drop`, `slam_row_cfg_err`, `slam_row_exec_timeout`.
- `trace` is the gateway-side trace identifier used for cross-layer correlation.

## Quick Test
```bash
printf 'C START seq=1 ts=100 soft_hz=20 max_power=60 left_gain=1 right_gain=1 left_trim=0 right_trim=0 slam_max_fps=8 slam_timeout_ms=60 slam_max_groups=4 slam_min_quality=15 slam_drop_policy=newest\nR 2 F 101\nSLI 3 102 frame_id=11 width=640 height=480 pixel_fmt=RGB24 keyframe=1 quality_hint=60 payload_ref=buf_11\nC STOP seq=4 ts=120\nq\n' | ./main_processor_demo
```

```bash
printf 'C START seq=1 ts=100 soft_hz=20 max_power=60 left_gain=1 right_gain=1 left_trim=0 right_trim=0 slam_max_fps=8 slam_timeout_ms=60 slam_max_groups=4 slam_min_quality=15 slam_drop_policy=newest row_ratio=0.333333 channel_mode=G sample_stride=2 max_rows=1 pack_mode=bin\nSR 2 101 frame_id=11 width=64 height=48 payload_ref=buf_11 keyframe=1 quality_hint=70\nC STOP seq=3 ts=120\nq\n' | ./communication_layer_demo
```

## Processor-Executor Reserved Interface
- `PushConfig(session_id, config_version, control_cfg, slam_cfg)`
- `ProcessFrame(session_id, frame_meta, timeout_ms)`
- `StopSession(session_id)`
- `GetHealth()`

## Notes
- Communication layer no longer decides SLAM business semantics.
- Processing layer controls rate-limit, drop policy, timeout, and fusion output.
- Current executor is mock; interface is fixed for real implementation swap-in.
