# USV Edge Demo (Short)

## Overview
This workspace contains a lightweight three-layer demo for USV edge validation.
Current focus is communication and control command flow. SLAM processing is still placeholder.

## Files
- `CommunicationLayer.cc`: short-frame communication parser/ack demo
- `MainProcessor.cc`: processing layer parser/dispatcher demo
- `TrusterAcuator.cc`: actuator mapping/output demo

## Build
```bash
g++ -std=c++17 -Wall -Wextra -pedantic CommunicationLayer.cc -o communication_layer_demo
g++ -std=c++17 -Wall -Wextra -pedantic MainProcessor.cc -o main_processor_demo
g++ -std=c++17 -Wall -Wextra -pedantic TrusterAcuator.cc -o truster_actuator_demo
```

## Communication Frames

### 1) Realtime short frame
```text
R <seq> <F|B|L|R> [client_ts_ms]
```
- Used for high-rate action control.
- Requires active session started by `C START`.

### 2) SLAM short report frame
```text
SL <seq> <tx_ms> <control_state> <slam_status> <count> <group8hex...>
```
- `count` range: `0..256`
- each `group8hex` is packed as:
  - bits `31:24` color (0..255)
  - bits `23:12` distance (0..0xFFF)
  - bits `11:8` status
  - bits `7:0` flags

### 3) Config long frame
```text
C START hz=<1..100> max_power=<v> left_gain=<v> right_gain=<v> left_trim=<v> right_trim=<v> ts=<ms>
C STOP [client_ts_ms]
```

## ACK Format
```text
ACK <OK|ERR> seq=<n> detail=<text> rx_ms=<n> ul_ms=<n> dl_ms=<n>
```
- `ul_ms`: uplink estimate from client timestamp (or `-1.00` if missing)
- `dl_ms`: local processing time in milliseconds

## Quick Test
```bash
printf 'C START hz=20 max_power=60 left_gain=1 right_gain=1 left_trim=0 right_trim=0 ts=100\nR 1 F 101\nSL 12 102 3 0 2 010F0A0B 020F0102\nC STOP 103\nq\n' | ./communication_layer_demo
```

## Notes
- Control and SLAM are split into separate frame types.
- Config start/stop remains long-frame by design.
- Current downlink is a stub (`std::cout`) for integration testing.
