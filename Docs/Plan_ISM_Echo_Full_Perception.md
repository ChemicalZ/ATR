# Plan: Full-Perception ISM Echoes (no actor required)

## Problem

ISM-only Echoes in the player's near band are functionally **blind**. They can hear noise via `EmitWorldStimulus → ReportEchoHeardLocation`, but no code path performs a sight check for them. The only sight writes (`ReportEchoSawActor`) come from `ATR_EchoAIController::ReportPerceptionFacts`, which runs only on the AI controller of a promoted Echo.

Concrete consequence: an ISM Echo standing 200 cm from the player with unobstructed line of sight will not aggro. It will only react if a noise reaches it loudly enough to (a) raise its `Awareness` toward promotion thresholds or (b) push its promotion score above siblings'.

Goal: **ISM Echoes should behave like Actors do, minus the Actor**. Same canonical state, same perception inputs, same intent decisions. The Actor is presentation + physics, not the source of intelligence.

## Non-goals

- Driving full skeletal animation, navmesh pathfinding, or CMC physics from the ISM tier. These are why we promote.
- Replicating an exact per-frame mirror of `UAIPerceptionComponent` semantics (max-age decay, multi-sense fusion). The subsystem path stays cheaper and coarser.
- Per-Echo sight cones with hearing fusion graphs. One conservative sight check per Echo per pass.

## Current state (verified)

| Capability | Promoted Actor | ISM Echo |
|---|---|---|
| Noise stim → `Awareness.LastHeardLocation` + Confidence/Urgency | ✓ subsystem-side (`EmitWorldStimulus`) | ✓ same path, same writes |
| Sight stim → `Awareness.ConfirmedVisibleActor` + `bHasCurrentLineOfSight` | ✓ via `UAIPerceptionComponent` → `ReportEchoSawActor` | ✗ no caller |
| Intent eval ticks (uses `RuntimeState`) | ✓ via `FATR_EchoIntentEvaluator` on controller's StateTree | ⚠ unclear — see open question 1 |
| Movement based on intent | ✓ AIController + CMC + path follow | ✗ position updates only via `Velocities[i]` integration in `SimTick`. `Velocities[i]` is written only by sound-impulse path (gated by `bEnableHordeMomentum`). No intent → velocity path exists. |
| Sees player → aggro instantly | ✓ | ✗ — must hear first to raise promotion score, get promoted, then sight kicks in |

## Design

### Phase 1 — Subsystem-side sight pass for Near band

A periodic processor that walks Near-band ISM Echoes and performs a single line trace per Echo to the nearest player pawn. On hit, it writes the same `ReportEchoSawActor` it would have come from the controller — reusing the existing facing-cone / cone-angle / confidence-update path.

**Trigger frequency**: 4–8 Hz, configurable via new `UATR_EchoSettings::ISMSightCheckHz`. Lower than active-tier AIPerception sight (typically ~10 Hz) because ISM cost should remain low. Configurable budget — max N traces per frame — to bound cost in dense hordes.

**Eligibility** (cheap pre-filters, applied in order):
1. `IsEchoInActiveTier(Index)` → false (skip promoted; they already see via AIPerception)
2. `HealthModel.IsDead(Index)` → false (dead can't see)
3. `CapabilityFlags & ATR_EchoCapability::IsImmobile` not gating — immobile Echoes still see. Only `IsDead` skips.
4. Echo is within `NearRelevancyRange` of any player pawn (use the same coarse-grid query the replication scheduler uses)
5. Distance to nearest player ≤ `ISMSightRange` (separate from `ActiveSightRadius`; can be the same value, but kept separate so designers can tune ISM-tier cheaper)
6. Player within `ISMSightConeDegrees` of Echo's `Yaws[Index]` facing — Echoes don't have eyes in the back of their head
7. Distance LOD: an Echo at the **outer** Near band shouldn't trace every pass. Use a per-Echo phase offset (`EchoId % NumPhaseBuckets`) so the population's traces spread across frames

**The trace**:
- Channel: a new `ECC_EchoSight` collision channel (or reuse an existing visibility channel). Single-line, capsule-eye-height start, player pawn eye-height end.
- Ignore: the Echo Manager's ISM components (they're not real bodies — they don't block sight). Other ISM Echoes also don't block. Static world + player capsule are what matters.
- On hit:
  - If hit pawn is a player → `ReportEchoSawActor(EchoId, Pawn, Pawn->GetActorLocation(), Pawn->GetVelocity(), Now)`. **Identical to the controller's call shape**, so all downstream awareness state machines (urgency, confidence, fluctuation, promotion scoring) work unchanged.
  - If hit world → no-op. We don't track miss-direction memory at this tier.
- On miss (no trace ran because out of range/cone): also no-op. If the Echo previously had `bHasCurrentLineOfSight=true`, the normal `ReportEchoLostSight` path needs a parallel here — see open question 2.

**Where it lives**: new file `Source/AllThatRemains/Echo/ATR_EchoISMPerception.{h,cpp}` containing `UATR_EchoISMPerception` (no actor — pure stateless static, takes `UATR_EchoSubsystem*`), called from the subsystem's existing tick loop.

### Phase 2 — Intent eval pass for ISM tier (open question)

Promoted Echoes re-evaluate intent every tick because the controller's `StateTree` ticks the `FATR_EchoIntentEvaluator`. The evaluator reads `RuntimeState` and writes back `Intent` + `Movement.Request`.

For ISM Echoes, that re-evaluation must happen somewhere. **Open question 1**: where is it currently happening, if at all? Three possibilities:
- (a) It's not happening for ISM Echoes. They have stale Intent from their last promotion, or `Dormant` from spawn. Promotion is the entry point at which Intent gets re-evaluated.
- (b) A subsystem-side pass already exists somewhere I missed.
- (c) Promotion happens fast enough on stimulus that we always re-evaluate in the active tier before the ISM Echo would need to act on the new awareness.

This needs a grep pass before Phase 1 implementation. If (a), Phase 2 adds an `EvaluateIntentForISMEcho(Index)` call to the same sight pass — read `Awareness`, pick `InvestigateHeard` / `ChaseLastSeenLocation` / `Idle`, write `Movement.Request`. Keep the logic shared between controller-driven and subsystem-driven calls (extract to a free function in `ATR_EchoIntentEvaluator`).

### Phase 3 — Velocity-from-intent for ISM tier (separate effort)

Currently `Velocities[i]` is only written by the sound impulse path. To make ISM Echoes physically move toward intended targets without promotion, we need a velocity-from-intent integration step.

- Walk Near-band ISM Echoes (same filter as sight)
- For each: read `Movement.Request`. If `Type == Actor`, target = actor location; if `Type == Location`, target = stored location; else zero velocity.
- Compute desired direction = `(target - position).GetSafeNormal2D()`. Set `Velocities[i] = Dir * SettingsBasedSpeed`.
- Honor `CapabilityFlags` (limp speed scale, crawl speed, immobile zero — same logic as `AATR_ActiveEcho::ApplyStructuralStateToMovement`).
- Yaw = atan2 of direction.
- Existing `SimTick` already integrates position from velocity, dirties the index.

**Risk**: navmesh. ISM Echoes drawing a straight line toward target will phase through walls. Mitigation options:
- Restrict velocity-from-intent to ISM Echoes in the **outer** Near band, where promotion will catch them before they need real path following.
- Use a coarse cell-grid traversal map (collide vs. world geometry once per cell, ~1 m grid).
- Accept the artifact at LowDetail — same compromise other horde games make.

Phase 3 is a larger architectural decision and intentionally separated from Phases 1–2.

## Open questions

1. Does the subsystem currently run any intent-evaluation pass over ISM-tier Echoes' `RuntimeState`? Grep for callers of `EvaluateIntentFor*` / mutations of `RuntimeStates[i].Intent` outside the controller path.
2. Does `bHasCurrentLineOfSight` need to be cleared by the subsystem sight pass on sight-loss, or is the active-tier `ReportEchoLostSight` enough? Likely we need a parallel: if a previous sight hit set it true and this pass's trace misses (and Echo is still in range), emit `ReportEchoLostSight`. Otherwise demoted-while-seeing Echoes stay stuck at `bHasCurrentLineOfSight=true`.
3. Trace cost budget: at 100 ISM Echoes in Near band, 4 Hz × 100 = 400 traces/sec. Manageable. At 500 Echoes, 2000 traces/sec — push to async traces or reduce frequency.
4. Multi-player split-screen / coop: pick the nearest player per Echo. Iterate `GetPlayerControllerIterator` (already done in `RebuildFineGrid`).

## Settings to add

New `UATR_EchoSettings` properties (Category: `Echo|Perception|ISM`):
- `ISMSightCheckHz` (float, default 4.0)
- `ISMSightRange` (float cm, default = `ActiveSightRadius`)
- `ISMSightConeDegrees` (float, default 200.0 — slightly wider than active to compensate for the lower update rate's miss window)
- `ISMSightMaxTracesPerFrame` (int32, default 64)
- `bEnableISMSight` (bool, default true) — kill switch for stealth tuning / perf debugging

## Rollout

1. **Land Phase 1 behind `bEnableISMSight=true` default-off.** Lets us merge without changing behavior; enable per-test once verified.
2. Run the existing 3-Echo + 1-Actor scenario the user reported. Expect: ISM Echo with line of sight aggros without promotion; ISM Echo without line of sight stays calm. Killing one in plain view of another → that other should aggro the killer (the player), independent of noise.
3. Phase 2 once we answer open question 1.
4. Phase 3 is a separate document.

## Not changing

- The promotion system. Promotion still happens (it's how we get full physics/CMC/path-following). This work makes the **decision to promote** more accurate (a real sighting registers as urgency immediately, no noise required) and makes the period before promotion no longer functionally blind.
- `bEnableHordeMomentum`. Sound impulse stays an independent knob.
- AIController perception. Promoted Echoes use it; this plan adds a parallel cheap path for the ISM tier only.
