# Core Processing Plan

Scope: focus on the core processor only. The gateway layer is intentionally deferred and will be aligned later against the finalized core contract.

## Goals
- Tighten the core state machine in `MainProcessor.cc`.
- Make ACK output explicit, stable, and easy to validate.
- Clarify health reporting and rollback signals.
- Keep executor interaction consistent with the processor state.

## Work Items
1. Define the final core ACK contract.
   - Fix the field order, field names, and success/failure semantics.
   - Separate transport timing, processor timing, and business result meaning.
   - Keep the output deterministic for the same input path.

2. Rework session state transitions.
   - Only mark a session active after configuration is fully accepted.
   - Ensure stop and failure paths always leave the state machine recoverable.
   - Avoid partial activation when executor setup fails.

3. Align ingress checks with processor policy.
   - Keep realtime rate limiting separate from SLAM ingest gating.
   - Make drop and reject reasons explicit.
   - Preserve the distinction between hard rejection and overload degradation.

4. Normalize health output.
   - Report active session state, counters, latency percentiles, drop counts, and timeout counts.
   - Make rollback recommendation based on stable thresholds.
   - Ensure health output reflects actual runnability, not just command history.

5. Verify executor-facing failure propagation.
   - Treat config push and runtime execution failures as first-class outcomes.
   - Ensure failures are visible in ACK and health output.
   - Keep executor behavior unchanged unless the core contract requires a small boundary fix.

## Validation
- Build the processor target with the executor layer.
- Replay a minimal command set covering `C START`, `R`, `SLI`, `HEALTH`, and `C STOP`.
- Check that success, rejection, overload, timeout, and executor error paths produce distinct ACK outcomes.
- Confirm the health snapshot matches the new state semantics.

## Out of Scope for Now
- Gateway protocol changes.
- Gateway ACK formatting.
- Gateway rollback policy.

## Notes
- The gateway layer will be adjusted later to match this finalized core definition.
- Keep edits small and behavior-driven so the core contract can settle before any cross-layer alignment.
