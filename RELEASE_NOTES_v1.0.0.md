# Release Notes - v1.0.0

Release date: 2026-04-24
Release branch: release/v1.0.0

## Summary
This release closes Phase 5 for the gateway-processor migration path and establishes runtime governance for canary rollout, health inspection, and fast rollback.

## Key Changes

### 1) Gateway Runtime Governance (CommunicationLayer)
- Added runtime switches:
  - `legacy_alias` (`on|off`)
  - `sli_enabled` (`on|off`)
  - `route_timeout_ms` (`1..5000`)
- Added management commands:
  - `GW HEALTH`
  - `GW SWITCH ...`
  - `GW ROLLBACK`
- Added health metrics and recommendation:
  - `rx_total`, `parse_fail`, `route_timeout`, `sli_drop`
  - `ack_ok`, `ack_err`, `ack_p95_ms`, `ack_p99_ms`
  - `rollback_recommended`
- Added route-timeout guard with explicit `route_timeout` error path.
- Added trace id generation and ACK sampling window for percentile statistics.

### 2) Processor Health and SLO Signals (MainProcessor)
- Added `HEALTH` command for runtime introspection.
- Added runtime metrics:
  - ACK totals and success/error split
  - SLI drop count
  - Executor timeout count
  - ACK downlink percentile samples (`p95`, `p99`)
- Added rollback recommendation logic based on:
  - ACK error rate threshold
  - ACK p95 latency threshold
- Unified ACK generation path through metric-aware wrapper.

### 3) Documentation Updates
- README:
  - Added Phase 5 runtime management commands and examples.
  - Added release gates and canary/rollback flow.
- SYSTEM_DESIGN:
  - Added "Phase 5 final acceptance and migration closure" section.
  - Clarified rollout gates, rollback trigger/action, and operation baseline.
  - Re-numbered subsequent sections to avoid heading conflicts.

## Compatibility
- Legacy aliases remain supported by default.
- Aliases can now be turned off progressively for migration.
- Rollback command restores conservative settings immediately.

## Validation Checklist
- Build:
  - `g++ -std=c++17 -Wall -Wextra -pedantic CommunicationLayer.cc -o /tmp/communication_layer_demo_release`
  - `g++ -std=c++17 -Wall -Wextra -pedantic MainProcessor.cc SlamExecutionLayer.cc -o /tmp/main_processor_demo_release`
- Runtime smoke tests:
  - Gateway management flow (`GW HEALTH/SWITCH/ROLLBACK`)
  - Processor health flow (`HEALTH`)
  - Session flow (`C START -> R -> SLI -> HEALTH -> C STOP`)

## Operational Notes
- Suggested canary sequence:
  1. Keep `legacy_alias=on` in lab and verify baseline.
  2. Switch to `legacy_alias=off` in single-vessel canary.
  3. Expand to fleet only when health gates remain stable.
- Immediate rollback trigger if `rollback_recommended=1` sustains in observation window.

## Known Limits
- Percentile implementation is sample-window based (not histogram/TDigest).
- Build pipeline is local/manual in this repository (no CI file added in this release).

## Upgrade Guide
1. Deploy binaries with Phase 5 code.
2. Start with default conservative switches.
3. Observe `GW HEALTH` and `HEALTH` for stable windows.
4. Apply canary switch changes gradually.
5. Use `GW ROLLBACK` if thresholds are violated.
