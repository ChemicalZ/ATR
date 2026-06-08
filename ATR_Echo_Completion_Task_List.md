# ATR Echo System — Completion & Refinement Task List

Derived from `echo_model_completion_refinement.md` cross-checked against the live source in
`Source/AllThatRemains/Echo/`. Every task names the concrete file(s) and the current code state so
the work can be picked up directly. Git history shows Phases 0–12 are landed; this list is the
gap between that branch and the document's required end state.

Legend: **[P0]** correctness/realism blockers · **[P1]** required for the stated end state ·
**[P2]** hardening / future-seam / tooling. Effort is rough (S < 0.5d, M 0.5–1.5d, L 2–4d).

---

## 0. Status summary

The architecture the document wants to preserve already exists and is sound: SoA runtime is
canonical (`ATR_EchoSubsystem`), identity is a stable `EchoId` keyed into `RuntimeStates`,
`FATR_EchoRuntimeState` holds awareness/search/movement/obstacle, the active controller bridges
perception/movement into the subsystem, hearing is modelled as a location-only `Noise` stimulus,
the agitation field is indirect, and replication is presentation-only. What remains is (a) closing
real no-cheat holes, (b) making the move/search/demotion logic correct, (c) promoting lower-tier
simulation from a steering stub to real LowDetail + Abstract tiers, (d) moving ~40 hardcoded tuning
values into `UATR_EchoSettings`/DataAssets, and (e) deleting the transitional/phase scaffolding.

---

## 1. Realism — no-cheat violations  [P0]

These are the document's non-negotiables and the only places where the active layer currently feeds
the subsystem information the Echo could not plausibly know.

### 1.1 Stop sampling live actor velocity on sight loss  [P0, M]

- **Where:** `ATR_EchoAIController.cpp::ReportPerceptionFacts` (line ~170) passes
  `Actor->GetVelocity()` and `Stim.StimulusLocation` into `ReportEchoLostSight`. The subsystem
  (`ATR_EchoSubsystem.cpp::ReportEchoLostSight`, line ~731) then **overwrites**
  `A.LastSeenLocation` and `A.LastSeenVelocity` with those live values. This is the headline cheat.
- **Do:**
  - Change the signature to `ReportEchoLostSight(int32 EchoId, AActor* Actor, float TimeSeconds)`
    (header `ATR_EchoSubsystem.h` line ~300). Optionally use the `TWeakObjectPtr` overload from the doc.
  - In the body: keep the `bWasConfirmedActor` guard, set `Mode = LostSightSearch`,
    clear `ConfirmedVisibleActor` and `bHasCurrentLineOfSight`, set `LastSeenTime`, and **do not touch**
    `LastSeenLocation` / `LastSeenVelocity` (they were captured while visible).
  - Update the caller to `ReportEchoLostSight(CachedEchoId, Actor, Now)`.
- **Accept:** grep for `GetVelocity()` and `GetActorLocation()` in the controller shows them used
  only inside the `WasSuccessfullySensed()` (currently-visible) branch.

### 1.2 Capture observed velocity only while visible, via position delta  [P0, M]

- **Where:** `ReportEchoSawActor` (line ~706) stores `Velocity` raw; the controller passes
  `Actor->GetVelocity()` directly (line ~165).
- **Do:**
  - Per the doc's "best realism path," compute observed velocity from sampled visible positions:
    cache `PreviousVisibleLocation`/`PreviousVisibleTime` per confirmed target on the controller and
    derive `ObservedVelocity = (CurrentVisibleLocation - PreviousVisibleLocation) / DeltaTime`.
    This avoids reading a movement component that may know more than the Echo saw.
  - In `ReportEchoSawActor`, smooth into stored velocity while visible:
    `A.LastSeenVelocity = Lerp(A.LastSeenVelocity, ObservedVelocity, LastSeenVelocitySmoothingAlpha)`;
    on first observation / zero prior, assign directly.
- **Depends on:** 5.x settings (`LastSeenVelocitySmoothingAlpha`).

### 1.3 Audit remaining no-cheat surfaces  [P0, S]

- Confirm `MoveToActor` (actor-typed move request) is only issued for `ChaseVisibleActor`
  (`bSeeing` true). Current `UpdateEchoIntent` already gates this correctly — add a comment
  documenting the invariant and a guard so a future edit can't issue an actor move from memory.
- Confirm hearing never sets `ConfirmedVisibleActor` (currently correct in `ReportEchoHeardLocation`)
  and `SourceActor_DebugOnly` is never read for behavior (grep: only assigned, never consumed). Keep.
- Confirm horde/agitation spread never carries an actor or exact player location (currently correct —
  only scalar + direction). Keep, and add an invariant comment on `AddWorldAgitation`.

---

## 2. Movement correctness  [P0]

### 2.1 Move task must not treat path `Idle` as success  [P0, M]

- **Where:** `ATR_EchoMoveRequestTask.cpp::Tick` and `ATR_EchoHandleObstacleTask.cpp::Tick` both
  `return Succeeded` on `EPathFollowingStatus::Idle` regardless of the real outcome. A path that
  aborts/fails goes Idle and is reported to the StateTree as success.
- **Do:** drive task status from the subsystem's classified result, not path status:
  - Add `uint32 MoveRequestSerial` and `uint32 LastCompletedMoveRequestSerial` to
    `FATR_EchoMovementIntent` (`ATR_EchoRuntimeTypes.h`).
  - `IssueMoveRequest` increments the serial and stamps it on the in-progress request;
    `ReportEchoMoveResult` writes `LastCompletedMoveRequestSerial` + `bLastMoveSucceeded` + `LastFailure`.
  - Task `Tick`: if `bMoveInProgress && serial not yet completed` → Running; if completed serial matches
    and succeeded → Succeeded; if completed and failed → Failed; if request invalid/stale → Failed.
  - This also fixes stale results from a previous move resolving a new StateTree state.
- **Accept:** a move that ends in `Blocked`/`OffPath`/`Aborted` makes the task report Failed, letting
  the tree transition to HandleObstacle/search instead of falsely succeeding.

### 2.2 Nav-project every generated search point  [P0, M]

- **Where:** `ComputeEchoSearchPoint` / `AdvanceLostSightSearch` (`ATR_EchoSubsystem.cpp` ~966–1043)
  emit raw world points straight into move requests with no navmesh projection.
- **Do:** project each candidate via `UNavigationSystemV1::ProjectPointToNavigation` using
  `SearchPointNavProjectionRadius` (settings). On failure, try the next fan point; if all fail, end the
  search (or transition to obstacle/fallback) rather than pathing to an off-mesh point.
- **Note:** projection touches the navigation system — keep it on the game thread (this pass already runs
  there). Document that constraint.

---

## 3. World-level stimulus API  [P0/P1]

### 3.1 Add `EmitWorldStimulus`  [P1, L]

- **Where:** today only `ReportEchoStimulus(EchoId, Event)` exists (per-Echo, called by the active
  controller). There is no gameplay-facing world entry point.
- **Do:** add `void EmitWorldStimulus(const FATR_StimulusEvent& Event)` on the subsystem that:
  - validates/clamps fields and applies radius/falloff;
  - deposits agitation into the field (`AddWorldAgitation`);
  - updates nearby **active** Echo awareness (radius query on the fine grid);
  - updates nearby **LowDetail** Echo awareness (radius/cell query — see §4);
  - updates **Abstract/cell** pressure where applicable (see §4);
  - raises promotion priority for strongly affected Echoes (see §6.2);
  - **never** grants actor-target knowledge.
- **Wire sources:** gunshot, footstep/sprint, melee impact, door slam, window break, vehicle engine,
  alarm, scream, Echo vocalization, blood pool, corpse smell. Expose `BlueprintCallable` so gameplay
  and the player pawn can emit. (Player noise emission from `ATR_Player` is a natural first caller.)
- **Accept:** firing a gunshot stimulus with no active controllers still agitates and migrates nearby
  LowDetail/Abstract Echoes.

---

## 4. Lower-tier simulation — promote to an early milestone  [P0/P1]

Currently "lower-tier" is only `RunSteeringPass`'s agitation-drift for non-active local entities
(`ATR_EchoSubsystem.cpp` ~1475–1490). `RunIntentPass` iterates **only** `PromotedIndices`. There is no
real LowDetail individual simulation and no Abstract/cell tier. The document explicitly wants this moved
up in priority because it underpins both realism and scalability.

### 4.1 LowDetail budgeted individual update  [P1, L]

- Add a budgeted pass (default ~5–10 Hz, `LowDetailUpdateHz`, `LowDetailMaxUpdatesPerTick`) over
  non-active Echoes within a working radius. Each update should:
  1. sync location/velocity/facing from the SoA row;
  2. decay confidence/urgency/agitation from settings;
  3. ingest nearby world stimuli + sample the agitation field;
  4. update awareness mode **without actor knowledge**;
  5. choose intent using the same `EATR_EchoIntent` enum;
  6. produce a low-detail velocity (not an active `MoveTo`) at low-detail speeds;
  7. mark transform dirty only past thresholds;
  8. update promotion score.
- Reuse `UpdateEchoIntent`'s decision logic where possible; factor the shared decision out of the
  actor-coupled bits so both tiers share one intent function.
- **Demotion continuity:** an Echo demoted mid-search must continue a simplified search from runtime
  state (`LowDetailSearch*` settings). The awareness/search structs already survive demotion — verify and test.

### 4.2 Abstract/cell tier  [P1, L]

- Add a coarse cell record (`Population`, `Agitation`, `PressureDirection`, `NoiseMemory`,
  `SmellMemory`, `LastUpdatedTime`) and a budgeted cell pass (~0.5–2 Hz, `AbstractUpdateHz`,
  `AbstractMaxCellsPerTick`). Behavior: stimuli add pressure to nearby cells; pressure decays;
  population migrates fractionally toward high-pressure neighbors; cells seed LowDetail state when a
  player/relevancy enters; **no per-frame fine pathing**.
- **Persistence:** preserve cell pressure across unload/reload (streaming) boundaries so a horde that
  built pressure off-screen is still there when the area reloads. (See §9 research note on save/stream.)

### 4.3 Tier budgets & scheduling  [P1, M]

- Active = full tick/StateTree; LowDetail = budgeted individual; Abstract = cell-level. All rates and
  per-tick caps come from `Echo|LowerTierSimulation`. Round-robin the budgets so cost is bounded and
  fair. Add trace scopes for each pass.

---

## 5. Configuration — kill hardcoded behavior numbers  [P1]

The document forbids hardcoded behavior tuning. These live values must move into `UATR_EchoSettings`
(`Config`, `EditAnywhere`, tooltips + `ClampMin`/`ForceUnits`), be clamped in `ValidateAndClamp`, and be
loaded into the subsystem at `Initialize` (the existing pattern at `ATR_EchoSubsystem.cpp` ~149–204).

**Current hardcoded offenders found:**

- Subsystem header (`ATR_EchoSubsystem.h` ~466–499): `AgitationCellSize`, `AgitationFieldDecayPerSec`,
  `HordeCuriosityThreshold`, `ConfidenceDecayPerSec`, `UrgencyDecayPerSec`, `AgitationDecayPerSec`,
  `LostSightMemoryThreshold`, `HeardInvestigateUrgency`, `HeardMemorySeconds`, `AgitationJoinThreshold`,
  `ReachLocationRadius`, `SightProjectionSeconds`, `MaxSightProjectionDistance`,
  `ObstacleHandleTimeoutSeconds`.
- Search internals (`BeginEchoSearch`/`ComputeEchoSearchPoint`): ±20° jitter, `300`/`MaxProjection`
  lead clamp, `0.8/0.6/0.3` radius weights, `12s` duration, fan pattern offsets, sidestep `300`/`100`.
- Agitation amounts: `0.6` on sight (`ReportEchoSawActor`), `0.25` on noise, `0.8` horde-pressure move
  distance (`UpdateEchoIntent`), `800` join distance.
- Controller (`ATR_EchoAIController`): sight `2000`/lose `2500`/peripheral `90`/maxage `5`
  (hardcoded in the constructor), `HearingRange 3000`, `ObstacleTraceDistance 200`, `LoyaltyBonusMultiplier`.

### 5.1 Add `Echo|Awareness`  [P1, S]
`ConfidenceDecayPerSecond, UrgencyDecayPerSecond, LostSightMemoryThreshold, HeardMemorySeconds,
HeardInvestigateUrgency, SmellMemorySeconds, SmellInvestigateUrgency, ReacquireSightConfidence,
SightLossGraceSeconds`.

### 5.2 Add `Echo|Sight`  [P1, S]
`ActiveSightRadius, ActiveLoseSightRadius, ActivePeripheralVisionAngleDegrees, ActiveSightMaxAgeSeconds,
LastSeenProjectionSeconds, MaxLastSeenProjectionDistance, LastSeenVelocitySmoothingAlpha`.
Apply these to the perception sense config (move them out of the controller constructor; configure in
`OnPossess` or from settings at spawn). `LastSeenVelocitySmoothingAlpha` is consumed by §1.2 and must only
update while visible.

### 5.3 Add `Echo|Hearing`  [P1, S]
`ActiveHearingRange, ActiveHearingMaxAgeSeconds, NoiseStrengthToUrgencyScale, NoiseStrengthToAgitationScale,
WeakNoiseTurnOnlyThreshold, StrongNoiseInvestigateThreshold`. Replaces controller `HearingRange` and the
`0.25`/`Loud` scalars in `ReportEchoHeardLocation`.

### 5.4 Add `Echo|Search`  [P1, S]
`ReachLocationRadius, SearchDefaultRadius, SearchMaxDurationSeconds, SearchStepAcceptanceRadius,
SearchPointNavProjectionRadius, SearchRandomAngleDegrees, MaxSearchSteps,
DefaultSearchPattern (TSoftObjectPtr<UATR_EchoSearchPatternDataAsset>)`. Consumed by §2.2 and §7.

### 5.5 Add `Echo|Agitation`  [P1, S]
`AgitationCellSize, AgitationFieldDecayPerSecond, EchoPersonalAgitationDecayPerSecond,
HordeCuriosityThreshold, AgitationJoinThreshold, SightAgitationAmount, NoiseAgitationAmountScale,
CombatAgitationAmount, EchoAgitationSpreadRadius, HordePressureMoveDistance,
HordePressureDirectionSmoothingAlpha`.

### 5.6 Add `Echo|LowerTierSimulation`  [P1, S]
`LowDetailUpdateHz, AbstractUpdateHz, LowDetailMaxUpdatesPerTick, AbstractMaxCellsPerTick,
LowDetailStimulusQueryRadius, LowDetailInvestigateSpeed, LowDetailWanderSpeed, LowDetailSearchSpeed,
LowDetailSearchRadius, LowDetailSearchDurationSeconds, LowDetailPromotionUrgencyBoost,
LowDetailPromotionAgitationBoost, AbstractCellMigrationRate, AbstractCellNoiseAttractionScale,
AbstractCellAgitationAttractionScale`. Consumed by §4.

### 5.7 Add `Echo|ObstacleHooks`  [P1, S]
`ObstacleForwardTraceLength, ObstacleTraceRadius, ObstacleHandleTimeoutSeconds, ObstacleSidestepDistance,
ObstacleForwardNudgeDistance, ObstacleRetryCooldownSeconds,
DefaultObstacleBehavior (TSoftObjectPtr<UATR_EchoObstacleBehaviorDataAsset>)`. Replaces controller
`ObstacleTraceDistance` and the `300`/`100` sidestep constants.

### 5.8 Add `Echo|Demotion`  [P1, S]
`MinTimeInTierSeconds` (already exists in Promotion — reconcile/move), `DemotionConfidenceBlockThreshold,
DemotionUrgencyBlockThreshold, DemotionSearchBlockSeconds, bBlockDemotionDuringVisibleChase,
bBlockDemotionDuringFreshSearch, bBlockDemotionDuringObstacleHandling`. Consumed by §6.

### 5.9 Extend `ValidateAndClamp` + subsystem load  [P1, M]
Add clamp lines for every new field (`ATR_EchoSettings.cpp`) and load them in `Initialize`
(`ATR_EchoSubsystem.cpp`). Remove the now-dead hardcoded member initializers from the subsystem header.

---

## 6. Promotion / demotion intelligence  [P1]

### 6.1 Stronger demotion guards  [P1, M]

- **Where:** `RunPromotionPass` demote loop (`ATR_EchoSubsystem.cpp` ~1402–1433) currently blocks only
  on `MinTimeInTierSeconds`, `bBlockDemotion`, and `bHasCurrentLineOfSight`.
- **Do:** also refuse demotion when (all from `Echo|Demotion`): `Confidence >= DemotionConfidenceBlockThreshold`,
  `Urgency >= DemotionUrgencyBlockThreshold`, intent is `ChaseVisibleActor`, or intent is
  `ChaseLastSeenLocation`/`SearchProjectedDirection`/`FanSearchArea` with a fresh search
  (`Now - Search.StartedTime <= DemotionSearchBlockSeconds`), or intent is `HandleObstacle` with a fresh
  obstacle, or the actor sets `bBlockDemotion`.

### 6.2 Score-based promotion & victim selection  [P1, M]

- **Where:** promotion currently sorts candidates purely by distance and demotes nothing to make room
  except via a distance "far list."
- **Do:** implement the document's promotion score:
  `DistancePriority + MustPromoteBonus + UrgencyBoost + ConfidenceBoost + AgitationBoost +
  PlayerFacing/VisibilityRelevanceBoost − RecentlyDemotedPenalty`, all weights in settings. When the pool
  is exhausted, pick victims by **lowest** score (urgency/confidence/distance), not arbitrary order. Keep
  the hard pool cap.
- This lets an agitated/searching Echo just outside the nearest ring stay eligible — required for the
  "single edge Echo pulled away, horde escalates" behavior.

---

## 7. DataAssets  [P1]

Create three `UDataAsset` subclasses (new files under `Source/AllThatRemains/Echo/Data/`), exposed as
`TSoftObjectPtr` defaults in settings and overridable per-archetype.

### 7.1 `UATR_EchoSearchPatternDataAsset`  [P1, M]
Fields: `TArray<FVector2D> SearchOffsets; float SearchRadiusScale; float MaxDurationScale;
float RandomAngleDegrees; bool bUseProjectedDirection; bool bAllowLocalRandomFallback;`.
Replace the hardcoded `Pattern[]` in `ComputeEchoSearchPoint` with this. Ship variants: standard
directional fan, aggressive forward-biased, confused local circling, pack-search, indoor tight.

### 7.2 `UATR_EchoArchetypeDataAsset`  [P1, M]
Fields: `Aggression, SightScale, HearingScale, SmellScale, SearchPersistenceScale,
AgitationSensitivityScale, MovementSpeedScale, ObstacleAggressionScale`, plus
`SearchPatternOverride`, `ObstacleBehaviorOverride`. Add a per-Echo archetype reference to
`FATR_EchoRuntimeState` (soft ptr or index) and apply modifiers where the corresponding base values are
used. Replaces the ad-hoc `State.Aggression` scalar.

### 7.3 `UATR_EchoObstacleBehaviorDataAsset`  [P1, S]
Fields: `bCanAttackDoors, bCanBreakWindows, bCanClimbFences, bCanCallNearbyEchoes,
AttackObstacleDelaySeconds, ObstacleDamagePerHit, GroupPoundingAgitationAmount,
RepathAfterFailedObstacleSeconds`. Keep current sidestep/repath as the only active behavior; this asset
defines the future break/climb seam so no architecture change is needed later.

---

## 8. Cleanup & documentation  [P1/P2]

### 8.1 Remove transitional / phase scaffolding  [P1, M]
- Delete legacy target path: `FATR_EchoTargetEvaluator` (`.h/.cpp`) and `FATR_EchoMoveToTask`
  (`.h/.cpp`) — the doc's "old target evaluator / move task." Confirm the StateTree asset no longer
  references them first (§8.3).
- Remove controller `CurrentTarget`, `SelectBestTarget`, `GetCurrentTarget`, `LoyaltyBonusMultiplier`,
  and the transitional `CurrentTarget = SelectBestTarget()` line in `HandlePerceptionUpdated`. These are
  explicitly marked "removed in the final cleanup pass."
- Strip the `Phase N` comments throughout (`ATR_EchoRuntimeTypes.h` header block; `ATR_EchoSubsystem.*`
  "Phase 1/2/3/5/6/8/9/11" headers; controller "Phase 2/Phase 5" notes; settings "Phase 3"). Rewrite as
  functional comments per the doc's good/bad examples (ownership, threading, authority, invariants,
  no-cheat rationale, algorithm grouping). Replace "placeholder" obstacle comments with the functional
  description the doc gives.

### 8.2 Replace stale StateTree text doc  [P1, S]
- The repo has no `statetree.txt`, but it has the `BP_ATR_EchoStateTree` asset and the doc calls for a
  fresh StateTree setup guide. Produce `Docs/Echo_StateTree_Setup.md` describing: Context Actor =
  AIController, Evaluator = Echo Intent, Move task = Echo Move Request, Obstacle task = Echo Handle
  Obstacle, transitions driven by `EATR_EchoIntent` values + task completion status. Delete any old
  text doc if one surfaces.

### 8.3 Verify the StateTree asset  [P1, S]
- Open `Content/Echo/BP_ATR_EchoStateTree` and confirm it binds the Echo Intent evaluator and the
  typed Move Request / Handle Obstacle tasks — not the legacy Target evaluator / MoveTo task. Fix
  bindings before deleting the legacy classes in §8.1.

---

## 9. Additional considerations (from comparable systems)  [P2]

Beyond the document, these are worth scoping now because the architecture is at the point where they're
cheap to add and expensive to retrofit. Sources at the end.

### 9.1 Fixed population + future Spawn Strategy  [P2, L]
**Design intent (explicit, non-negotiable):** the Echo count is a **fixed hard cap**. The population is
generated once at world start and stays constant — Echoes are a finite resource the player can grind
down and, however unlikely, kill to extinction. There is **no omnipotent/dramatic director** that
modulates difficulty or funnels Echoes toward the player to keep tension up. (This is a deliberate
rejection of the L4D AI-Director model; promotion/demotion remains a pure spatial LOD mechanism, not a
pacing dial.) The end-state model in the source document already aligns with this — keep it that way and
make sure nothing in §4 (Abstract tier) ever *creates* Echoes to respect a cap; it may only migrate or
dormant-ize existing ones.

**Future direction — Spawn Strategy (not a director):** realistic placement and distribution rather than
demand-driven spawning. At world generation, seed the fixed population by location plausibility —
hotspots like hospitals, grocery stores, military outposts, and highways carry higher density; sparse
areas carry less. This is a one-time distribution over a fixed total, not an ongoing spawner.

**Future direction — lightweight horde coordinator (event-driven, bounded):** a thin, *non-omnipotent*
layer that can **form and break up hordes from the existing population** for world events and route them
through neighborhoods — modelled as if *other survivors* (offscreen actors) triggered the movement, not
as the game targeting the player. It must obey the same rules as everything else: it works through the
agitation/pressure field and existing Echoes only, never spawns new ones, never shares exact player
knowledge, and never exceeds the fixed cap. It is a coordination/illusion-of-a-living-world tool, not a
difficulty governor. Scope this as a later milestone once §4's tiers exist (it is their natural consumer).

### 9.2 Treat the agitation field as a real influence map  [P2, M]
The field today blends a 3×3 neighborhood **on read** but does no true **diffusion/propagation** on
write — pressure doesn't actually spread cell-to-cell over time, it just deposits and decays. Influence-map
practice separates *placement* (deposit) from *diffusion* (spread/blur each tick). Adding a budgeted
diffusion step would make hordes coalesce and "flow" toward sustained activity rather than only reacting
within one cell radius, and gives you emergent front-lines/gradients for free.

### 9.3 Smell / blood trail system  [P2, M]
`EATR_StimulusType::Smell`/`Blood` and `LastSmelledLocation` exist but only bump agitation; there's no
trail following. The settings already reserve `SmellMemorySeconds`/`SmellInvestigateUrgency`. A
location/field-based trail (decaying breadcrumb cells the Echo can follow gradient-wise) fits the
no-cheat model and the influence-map machinery in §9.2.

### 9.4 Observability / debugging  [P2, M]
Add a `FGameplayDebuggerCategory` for Echo: per-Echo intent/awareness/confidence/urgency, agitation
field heatmap, search fan + nav-projection hits/misses, tier, promotion score. This is how you'll
actually verify the no-cheat and demotion-continuity invariants at runtime.

### 9.5 Automated tests  [P2, M]
Add functional/automation tests for the invariants that are easy to silently regress: lost-sight does
not change `LastSeen*`; an actor move is never issued from memory; search points are on the navmesh;
demotion preserves search state; LowDetail continues a demoted search. The repo already has an
`Automation_AllThatRemains` solution — wire these in.

### 9.6 Threading & determinism review  [P2, S]
New LowDetail/Abstract passes and nav projection must respect thread ownership (nav queries on game
thread; SoA writes match the existing `ParallelFor` discipline in `RunSteeringPass`). Decide whether the
sim needs to stay deterministic for replay/networking and, if so, keep the per-Echo hash approach
(`EchoHash01`) as the only randomness source rather than `FMath::Rand`.

### 9.7 Abstract-cell save/stream persistence  [P2, M]
For a large/streamed world, persist Abstract cell pressure/population across level-stream and save/load
so off-screen horde state survives — already called out in §4.2 but worth tracking as its own task with
the save system.

---

## 10. Suggested sequencing

1. **Realism blockers first** (§1, §2) — small, high-value, unblock honest behavior.
2. **Settings scaffolding** (§5) — everything below consumes it; do the categories + clamp + load pass.
3. **DataAssets** (§7) — needed by search/obstacle/archetype work.
4. **Lower-tier simulation** (§3 world stimulus → §4 LowDetail → §4 Abstract) — the largest effort and
   the document's raised-priority milestone.
5. **Promotion/demotion intelligence** (§6).
6. **Cleanup + StateTree docs** (§8) — last, once the new paths are proven and legacy is unreferenced.
7. **Additional considerations** (§9) — schedule per appetite; 9.4/9.5 pay for themselves during 1–6.

---

## Final checklist (from the document, mapped to tasks)

Realism: 1.1, 1.2, 1.3, 2.1, 2.2, 4.1 · Performance: 4.3, 6.2, replication unchanged (verify) ·
Configuration: 5.1–5.9, 7.1–7.3 · Cleanup: 8.1, 8.2, 8.3.

---

### Sources

- [The AI Systems of Left 4 Dead — Michael Booth, Valve (GDC)](https://steamcdn-a.akamaihd.net/apps/valve/2009/ai_systems_of_l4d_mike_booth.pdf)
- [11 Secrets about Left 4 Dead's AI Director and its Procedural Zombie Population (AiGameDev)](https://www.cs.drexel.edu/~santi/teaching/2012/CS680/papers/11%20Secrets%20about%20LEFT%204%20DEAD%E2%80%99s%20AI%20Director%20and%20its%20Procedural%20Zombie%20Population%20%7C%20AiGameDev.com.pdf)
- [The Director — Left 4 Dead Wiki](https://left4dead.fandom.com/wiki/The_Director)
- [Escaping the Grid: Infinite-Resolution Influence Mapping — Mike Lewis (Game AI Pro 2)](http://www.gameaipro.com/GameAIPro2/GameAIPro2_Chapter29_Escaping_the_Grid_Infinite-Resolution_Influence_Mapping.pdf)
- [The Core Mechanics of Influence Mapping (GameDev.net)](https://gamedev.net/tutorials/programming/artificial-intelligence/the-core-mechanics-of-influence-mapping-r2799/)
